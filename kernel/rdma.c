// kernel/rdma.c - RDMA subsystem implementation

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "rdma.h"

/* ============================================
 * GLOBAL STATE
 * ============================================ */

struct rdma_mr mr_table[MAX_MRS];
struct spinlock mr_lock;

struct rdma_qp qp_table[MAX_QPS];
struct spinlock qp_lock;

static volatile uint32 *rdma_regs;  // Will be initialized in rdma_hw_init()

/* ============================================
 * HARDWARE ACCESS FUNCTIONS
 * ============================================ */

/* Write to RDMA register */
static inline void
rdma_writereg(uint32 offset, uint32 value)
{
    rdma_regs[offset / 4] = value;
}

/* Read from RDMA register */
static inline uint32
rdma_readreg(uint32 offset)
{
    return rdma_regs[offset / 4];
}

/* Initialize hardware interface */
void
rdma_hw_init(void)
{
    // Initialize MMIO pointer to kernel virtual address
    // E1000_RDMA_BASE is physical address, must add KERNBASE for kernel VA
    rdma_regs = (volatile uint32 *)(E1000_RDMA_BASE + KERNBASE);
    
    // Set MR table pointer to physical address of hardware-visible part
    uint64 mr_hw_va = (uint64)(&mr_table[0].hw);
    uint64 mr_table_pa;
    
    // Convert kernel virtual address to physical
    if (mr_hw_va >= KERNBASE) {
        mr_table_pa = mr_hw_va - KERNBASE;
    } else {
        panic("rdma_hw_init: MR table not in kernel space");
    }
    
    // Validate physical address is reasonable
    if (mr_table_pa == 0 || mr_table_pa >= PHYSTOP) {
        panic("rdma_hw_init: invalid MR table physical address");
    }
    
    rdma_writereg(E1000_MR_TABLE_PTR, (uint32)mr_table_pa);
    rdma_writereg(E1000_MR_TABLE_LEN, MAX_MRS);
    
    printf("rdma_hw: MR table at PA 0x%lx, %d entries\n", mr_table_pa, MAX_MRS);
}

/* Enable RDMA in hardware */
void
rdma_hw_enable(void)
{
    rdma_writereg(E1000_RDMA_CTRL, RDMA_CTRL_ENABLE);
    
    // Wait for hardware to be ready (with timeout)
    int timeout = 1000;
    while (!(rdma_readreg(E1000_RDMA_STATUS) & RDMA_STATUS_READY) && timeout > 0) {
        timeout--;
    }
    
    if (timeout == 0) {
        panic("rdma_hw: hardware failed to initialize");
    }
    
    printf("rdma_hw: hardware enabled and ready\n");
}

/* Configure hardware for a queue pair */
void
rdma_hw_setup_qp(int qp_id, struct rdma_qp *qp)
{
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    
    // Write Send Queue configuration
    rdma_writereg(qp_base + E1000_QP_SQ_BASE, (uint32)qp->sq_paddr);
    rdma_writereg(qp_base + E1000_QP_SQ_SIZE, qp->sq_size);
    
    // Write Completion Queue configuration
    rdma_writereg(qp_base + E1000_QP_CQ_BASE, (uint32)qp->cq_paddr);
    rdma_writereg(qp_base + E1000_QP_CQ_SIZE, qp->cq_size);
    
    printf("rdma_hw: QP %d configured (SQ: 0x%p/%d, CQ: 0x%p/%d)\n",
           qp_id, qp->sq_paddr, qp->sq_size, qp->cq_paddr, qp->cq_size);
}

/* Ring doorbell - tell hardware new work is available */
void
rdma_hw_ring_doorbell(int qp_id, uint32 sq_tail)
{
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    rdma_writereg(qp_base + E1000_QP_SQ_TAIL, sq_tail);
}

/* ============================================
 * MEMORY REGION MANAGEMENT
 * ============================================ */

/* Initialize MR subsystem */
void
rdma_mr_init(void)
{
    initlock(&mr_lock, "rdma_mr");
    
    for (int i = 0; i < MAX_MRS; i++) {
        mr_table[i].hw.valid = 0;
        mr_table[i].hw.id = 0;
        mr_table[i].owner = 0;
        mr_table[i].owner_pid = 0;
        mr_table[i].refcount = 0;
    }
    
    printf("rdma_mr: initialized %d MR slots\n", MAX_MRS);
}

/* Register a memory region
 * 
 * This is one of the most important functions! It:
 * 1. Validates the user memory address
 * 2. Translates virtual -> physical address
 * 3. "Pins" the memory (in xv6, memory is already pinned - no swapping)
 * 4. Records the registration for future validation
 * 
 * Returns: MR ID (1-based) on success, -1 on error
 */
int
rdma_mr_register(uint64 addr, uint64 len, int flags)
{
    struct proc *p = myproc();
    struct rdma_mr *mr = 0;
    int mr_id = -1;
    
    // Validate arguments
    if (addr == 0 || len == 0) {
        printf("rdma_mr_register: invalid addr or len\n");
        return -1;
    }
    
    // Check that address is in user space
    if (addr >= p->sz || addr + len > p->sz) {
        printf("rdma_mr_register: address out of bounds (addr=0x%p, len=%d, sz=%d)\n",
               addr, len, p->sz);
        return -1;
    }

    // Check that the memory region is not crossing a page boundary
    uint64 start_page = PGROUNDDOWN(addr);
    uint64 end_page = PGROUNDDOWN(addr + len - 1);
    
    if (start_page != end_page) {
        printf("rdma_mr_register: MR cannot cross page boundary (addr=0x%lx len=%d)\n",
               addr, len);
        printf("  Start page: 0x%lx, End page: 0x%lx\n", start_page, end_page);
        return -1;
    }

    // Find free MR slot
    acquire(&mr_lock);
    
    for (int i = 0; i < MAX_MRS; i++) {
        if (!mr_table[i].hw.valid) {
            mr = &mr_table[i];
            mr_id = i + 1;  // 1-based IDs (0 = invalid)
            break;
        }
    }
    
    if (!mr) {
        release(&mr_lock);
        printf("rdma_mr_register: no free MR slots\n");
        return -1;  // No free slots
    }
    
    // Translate virtual to physical address using page table walk
    pte_t *pte = walk(p->pagetable, addr, 0);
    if (pte == 0 || (*pte & PTE_V) == 0) {
        release(&mr_lock);
        printf("rdma_mr_register: page not mapped\n");
        return -1;  // Page not mapped
    }
    
    // Extract physical address from PTE
    // PTE2PA gives us the physical page address
    // Then add the page offset from the virtual address
    uint64 paddr = PTE2PA(*pte) | (addr & (PGSIZE - 1));
    
    // Fill in hardware-visible MR structure
    mr->hw.id = mr_id;
    mr->hw.access_flags = flags;
    mr->hw.vaddr = addr;
    mr->hw.paddr = paddr;
    mr->hw.length = len;
    mr->hw.lkey = mr_id;  // Simplified: use ID as key
    mr->hw.rkey = mr_id;  // Simplified: same key for local/remote
    mr->hw.valid = 1;
    
    // Fill in kernel metadata
    mr->owner = p;
    mr->owner_pid = p->pid;
    mr->refcount = 0;
    
    release(&mr_lock);
    
    printf("rdma_mr: registered MR %d for PID %d: vaddr=0x%p paddr=0x%p len=%d flags=0x%x\n",
           mr_id, p->pid, addr, paddr, len, flags);
    
    return mr_id;
}

/* Deregister a memory region
 * 
 * Safely removes an MR, checking:
 * - Ownership (only owner can deregister)
 * - Reference count (no in-flight operations)
 * 
 * Returns: 0 on success, -1 on error
 */
int
rdma_mr_deregister(int mr_id)
{
    if (mr_id < 1 || mr_id > MAX_MRS) {
        printf("rdma_mr_deregister: invalid MR ID %d\n", mr_id);
        return -1;
    }
    
    struct proc *p = myproc();
    
    acquire(&mr_lock);
    
    struct rdma_mr *mr = &mr_table[mr_id - 1];
    
    // Check validity
    if (!mr->hw.valid) {
        release(&mr_lock);
        printf("rdma_mr_deregister: MR %d not valid\n", mr_id);
        return -1;
    }
    
    // Check ownership (both pointer and PID for safety)
    if (mr->owner != p || mr->owner_pid != p->pid) {
        release(&mr_lock);
        printf("rdma_mr_deregister: MR %d not owned by PID %d\n", mr_id, p->pid);
        return -1;
    }
    
    // Check reference count - can't deregister while in use
    if (mr->refcount > 0) {
        release(&mr_lock);
        printf("rdma_mr_deregister: MR %d still has %d in-flight operations\n",
               mr_id, mr->refcount);
        return -1;
    }
    
    // Clear the MR
    mr->hw.valid = 0;
    mr->hw.id = 0;
    mr->owner = 0;
    mr->owner_pid = 0;
    
    release(&mr_lock);
    
    printf("rdma_mr: deregistered MR %d\n", mr_id);
    
    return 0;
}

/* Get MR by ID - returns NULL if invalid or not owned by current process
 * 
 * Note: Caller should hold mr_lock if they need consistent view
 */
struct rdma_mr*
rdma_mr_get(int mr_id)
{
    if (mr_id < 1 || mr_id > MAX_MRS) {
        return 0;
    }
    
    struct proc *p = myproc();
    struct rdma_mr *mr = &mr_table[mr_id - 1];
    
    // Only return if valid and owned by calling process
    if (!mr->hw.valid || mr->owner != p || mr->owner_pid != p->pid) {
        return 0;
    }
    
    return mr;
}

/* ============================================
 * QUEUE PAIR MANAGEMENT
 * ============================================ */

/* Initialize QP subsystem */
void
rdma_qp_init(void)
{
    initlock(&qp_lock, "rdma_qp");
    
    for (int i = 0; i < MAX_QPS; i++) {
        qp_table[i].valid = 0;
        qp_table[i].id = 0;
        qp_table[i].state = QP_STATE_RESET;
        qp_table[i].outstanding_ops = 0;
        qp_table[i].connected = 0;
        qp_table[i].stats_sends = 0;
        qp_table[i].stats_completions = 0;
        qp_table[i].stats_errors = 0;
    }
    
    printf("rdma_qp: initialized %d QP slots\n", MAX_QPS);
}

/* Create a queue pair
 * 
 * Allocates kernel memory for SQ and CQ, configures hardware
 * 
 * Returns: QP ID (0-based) on success, -1 on error
 */
int
rdma_qp_create(uint32 sq_size, uint32 cq_size)
{
    struct proc *p = myproc();
    struct rdma_qp *qp = 0;
    int qp_id = -1;
    
    // Validate sizes - must be power of 2 for efficient ring buffer
    if (sq_size == 0 || (sq_size & (sq_size - 1)) != 0 ||
        cq_size == 0 || (cq_size & (cq_size - 1)) != 0) {
        printf("rdma_qp_create: sizes must be power of 2\n");
        return -1;
    }
    
    // Limit to page size
    if (sq_size > PGSIZE / sizeof(struct rdma_work_request) ||
        cq_size > PGSIZE / sizeof(struct rdma_completion)) {
        printf("rdma_qp_create: sizes too large\n");
        return -1;
    }
    
    // Find free QP slot
    acquire(&qp_lock);
    
    for (int i = 0; i < MAX_QPS; i++) {
        if (!qp_table[i].valid) {
            qp = &qp_table[i];
            qp_id = i;
            break;
        }
    }
    
    if (!qp) {
        release(&qp_lock);
        printf("rdma_qp_create: no free QP slots\n");
        return -1;  // No free slots
    }
    
    // Allocate Send Queue (one page of kernel memory)
    qp->sq = (struct rdma_work_request *)kalloc();
    if (!qp->sq) {
        release(&qp_lock);
        printf("rdma_qp_create: failed to allocate SQ\n");
        return -1;
    }
    memset(qp->sq, 0, PGSIZE);
    qp->sq_size = sq_size;
    qp->sq_head = 0;
    qp->sq_tail = 0;
    
    // Convert to physical address for DMA
    uint64 sq_va = (uint64)qp->sq;
    qp->sq_paddr = (sq_va >= KERNBASE) ? (sq_va - KERNBASE) : sq_va;
    
    // Allocate Completion Queue (one page of kernel memory)
    qp->cq = (struct rdma_completion *)kalloc();
    if (!qp->cq) {
        kfree((void *)qp->sq);
        release(&qp_lock);
        printf("rdma_qp_create: failed to allocate CQ\n");
        return -1;
    }
    memset(qp->cq, 0, PGSIZE);
    qp->cq_size = cq_size;
    qp->cq_head = 0;
    qp->cq_tail = 0;
    
    // Convert to physical address for DMA
    uint64 cq_va = (uint64)qp->cq;
    qp->cq_paddr = (cq_va >= KERNBASE) ? (cq_va - KERNBASE) : cq_va;
    
    // Fill in QP metadata
    qp->id = qp_id;
    qp->owner = p;
    qp->valid = 1;
    qp->state = QP_STATE_INIT;
    qp->outstanding_ops = 0;
    qp->connected = 0;
    qp->stats_sends = 0;
    qp->stats_completions = 0;
    qp->stats_errors = 0;
    
    // Configure hardware
    rdma_hw_setup_qp(qp_id, qp);
    
    // Transition to READY state
    qp->state = QP_STATE_READY;
    
    release(&qp_lock);
    
    printf("rdma_qp: created QP %d for PID %d (sq_size=%d cq_size=%d)\n",
           qp_id, p->pid, sq_size, cq_size);
    
    return qp_id;
}

/* Destroy a queue pair
 * 
 * Frees resources and marks QP as invalid
 * 
 * Returns: 0 on success, -1 on error
 */
int
rdma_qp_destroy(int qp_id)
{
    if (qp_id < 0 || qp_id >= MAX_QPS) {
        printf("rdma_qp_destroy: invalid QP ID %d\n", qp_id);
        return -1;
    }
    
    struct proc *p = myproc();
    
    acquire(&qp_lock);
    
    struct rdma_qp *qp = &qp_table[qp_id];
    
    // Check ownership
    if (!qp->valid || qp->owner != p) {
        release(&qp_lock);
        printf("rdma_qp_destroy: QP %d not owned by PID %d\n", qp_id, p->pid);
        return -1;
    }
    
    // Warn if outstanding operations exist
    if (qp->outstanding_ops > 0) {
        printf("rdma_qp_destroy: WARNING - QP %d has %d outstanding ops\n",
               qp_id, qp->outstanding_ops);
    }
    
    // Free memory
    if (qp->sq) kfree((void *)qp->sq);
    if (qp->cq) kfree((void *)qp->cq);
    
    // Print statistics before destroying
    printf("rdma_qp: destroying QP %d (sends=%d comps=%d errors=%d)\n",
           qp_id, qp->stats_sends, qp->stats_completions, qp->stats_errors);
    
    // Mark invalid
    qp->valid = 0;
    qp->id = 0;
    qp->state = QP_STATE_RESET;
    
    release(&qp_lock);
    
    return 0;
}

/* Post a work request to the send queue
 * 
 * This is where zero-copy happens! We:
 * 1. Validate the MR and QP state
 * 2. Copy work request to kernel's SQ
 * 3. Ring doorbell to notify hardware
 * 
 * Hardware will then DMA directly from user memory!
 * 
 * Returns: 0 on success, -1 on error
 * 
 * NOTE: 'wr' MUST point to kernel memory. Syscall handlers must
 * use copyin() to copy user WRs into kernel space first.
 */
int
rdma_qp_post_send(int qp_id, struct rdma_work_request *wr)
{
    if (qp_id < 0 || qp_id >= MAX_QPS || !wr) {
        printf("rdma_qp_post_send: invalid parameters\n");
        return -1;
    }
    
    struct proc *p = myproc();
    
    // This prevents lock ordering issues
    acquire(&mr_lock);
    
    struct rdma_mr *mr = rdma_mr_get(wr->local_mr_id);
    if (!mr) {
        release(&mr_lock);
        printf("rdma_qp_post_send: invalid MR ID %d\n", wr->local_mr_id);
        return -1;
    }
    
    // Check bounds
    if (wr->local_offset + wr->length > mr->hw.length) {
        release(&mr_lock);
        printf("rdma_qp_post_send: access out of MR bounds\n");
        return -1;
    }
    
    // Increment MR refcount (operation in flight)
    mr->refcount++;
    
    // Translate to physical address for DMA
    uint64 physical_offset = mr->hw.paddr + wr->local_offset;
    
    release(&mr_lock);
    
    // NOW acquire qp_lock (proper lock ordering: mr_lock -> qp_lock)
    acquire(&qp_lock);
    
    struct rdma_qp *qp = &qp_table[qp_id];
    
    // Check ownership
    if (!qp->valid || qp->owner != p) {
        release(&qp_lock);
        // Undo refcount increment
        acquire(&mr_lock);
        mr->refcount--;
        release(&mr_lock);
        printf("rdma_qp_post_send: QP %d not owned by current process\n", qp_id);
        return -1;
    }
    
    // Check QP state
    if (qp->state != QP_STATE_READY) {
        release(&qp_lock);
        // Undo refcount
        acquire(&mr_lock);
        mr->refcount--;
        release(&mr_lock);
        printf("rdma_qp_post_send: QP %d not in READY state\n", qp_id);
        return -1;
    }
    
    // Check if queue is full
    uint32 next_tail = (qp->sq_tail + 1) % qp->sq_size;
    if (next_tail == qp->sq_head) {
        qp->stats_errors++;
        release(&qp_lock);
        // Undo refcount
        acquire(&mr_lock);
        mr->refcount--;
        release(&mr_lock);
        printf("rdma_qp_post_send: QP %d SQ is full\n", qp_id);
        return -1;
    }
    
    struct rdma_work_request kernel_wr = *wr;
    kernel_wr.local_offset = physical_offset;
    
    // Copy modified WR to Send Queue
    qp->sq[qp->sq_tail] = kernel_wr;
    
    // Update tail pointer
    qp->sq_tail = next_tail;
    
    // Update statistics
    qp->outstanding_ops++;
    qp->stats_sends++;
    
    // Ensure SQ write is visible to hardware before ringing doorbell
    __sync_synchronize();
    
    // Ring doorbell - tells hardware to process the new work
    rdma_hw_ring_doorbell(qp_id, qp->sq_tail);
    
    release(&qp_lock);
    
    return 0;
}

/* Poll completion queue for completed operations
 * 
 * Returns: number of completions found (0 to max_comps), -1 on error
 */
int
rdma_qp_poll_cq(int qp_id, struct rdma_completion *comp_array, int max_comps)
{
    if (qp_id < 0 || qp_id >= MAX_QPS || !comp_array || max_comps <= 0) {
        printf("rdma_qp_poll_cq: invalid parameters\n");
        return -1;
    }
    
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    
    // *** CRITICAL: Array to collect MR IDs for later processing ***
    uint32 mr_ids_to_decrement[DEFAULT_SQ_SIZE];
    int num_to_decrement = 0;
    
    // ==========================================
    // STEP 1: Process queues (holding qp_lock)
    // ==========================================
    acquire(&qp_lock);
    
    struct rdma_qp *qp = &qp_table[qp_id];
    
    // Check ownership
    if (!qp->valid || qp->owner != myproc()) {
        release(&qp_lock);
        return -1;
    }
    
    // --- Part A: Reap Send Queue ---
    uint32 hw_sq_head = rdma_readreg(qp_base + E1000_QP_SQ_HEAD);
    
    // Validate hardware value
    if (hw_sq_head >= qp->sq_size) {
        printf("rdma_qp_poll_cq: invalid SQ head %d\n", hw_sq_head);
        hw_sq_head = hw_sq_head % qp->sq_size;
    }
    
    // *** CRITICAL FIX: Just collect MR IDs, DON'T acquire mr_lock here! ***
    while (qp->sq_head != hw_sq_head) {
        struct rdma_work_request *consumed_wr = &qp->sq[qp->sq_head];
        uint32 mr_id = consumed_wr->local_mr_id;
        
        // Store MR ID for later processing (no lock needed)
        if (mr_id >= 1 && mr_id <= MAX_MRS && num_to_decrement < DEFAULT_SQ_SIZE) {
            mr_ids_to_decrement[num_to_decrement++] = mr_id;
        }
        
        qp->sq_head = (qp->sq_head + 1) % qp->sq_size;
    }
    
    // --- Part B: Poll Completion Queue ---
    uint32 hw_tail = rdma_readreg(qp_base + E1000_QP_CQ_TAIL);
    
    // Validate hardware value
    if (hw_tail >= qp->cq_size) {
        printf("rdma_qp_poll_cq: invalid CQ tail %d\n", hw_tail);
        hw_tail = hw_tail % qp->cq_size;
    }
    
    // *** NEW: Memory barrier to ensure we see latest CQ data ***
    __sync_synchronize();
    
    int n = 0;
    
    // Copy completions from CQ to user buffer
    while (qp->cq_head != hw_tail && n < max_comps) {
        comp_array[n] = qp->cq[qp->cq_head];
        
        // Update statistics
        if (comp_array[n].status == RDMA_WC_SUCCESS) {
            qp->stats_completions++;
        } else {
            qp->stats_errors++;
        }
        
        // Decrement outstanding operations
        if (qp->outstanding_ops > 0) {
            qp->outstanding_ops--;
        }
        
        qp->cq_head = (qp->cq_head + 1) % qp->cq_size;
        n++;
    }
    
    // Update hardware CQ head
    if (n > 0) {
        rdma_writereg(qp_base + E1000_QP_CQ_HEAD, qp->cq_head);
    }
    
    // *** CRITICAL: Release qp_lock BEFORE acquiring mr_lock ***
    release(&qp_lock);
    
    // ==========================================
    // STEP 2: Decrement MR refcounts (holding ONLY mr_lock)
    // ==========================================
    if (num_to_decrement > 0) {
        acquire(&mr_lock);
        
        for (int i = 0; i < num_to_decrement; i++) {
            uint32 mr_id = mr_ids_to_decrement[i];
            struct rdma_mr *mr = &mr_table[mr_id - 1];  // mr_id is 1-based
            
            if (mr->hw.valid && mr->refcount > 0) {
                mr->refcount--;
            }
        }
        
        release(&mr_lock);
    }
    
    return n;
}

/* Connect QP to remote peer (for network RDMA)
 * 
 * Sets up connection parameters for two-host RDMA
 * 
 * Returns: 0 on success, -1 on error
 */
int
rdma_qp_connect(int qp_id, uint8 mac[6], uint32 remote_qp)
{
    if (qp_id < 0 || qp_id >= MAX_QPS || !mac) {
        return -1;
    }
    
    acquire(&qp_lock);
    
    struct rdma_qp *qp = &qp_table[qp_id];
    
    // Check ownership
    if (!qp->valid || qp->owner != myproc()) {
        release(&qp_lock);
        return -1;
    }
    
    // Check state (must be INIT or READY)
    if (qp->state != QP_STATE_INIT && qp->state != QP_STATE_READY) {
        release(&qp_lock);
        return -1;
    }
    
    // Set connection parameters
    for (int i = 0; i < 6; i++) {
        qp->remote_mac[i] = mac[i];
    }
    qp->remote_qp_num = remote_qp;
    qp->connected = 1;
    
    release(&qp_lock);
    
    printf("rdma_qp: QP %d connected to %02x:%02x:%02x:%02x:%02x:%02x QP %d\n",
           qp_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], remote_qp);
    
    return 0;
}

/* ============================================
 * INITIALIZATION
 * ============================================ */

void
rdma_init(void)
{
    printf("rdma: initializing subsystem\n");
    
    rdma_mr_init();
    rdma_qp_init();
    rdma_hw_init();
    rdma_hw_enable();
    
    printf("rdma: initialization complete\n");
}