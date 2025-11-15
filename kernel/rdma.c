// kernel/rdma.c - RDMA subsystem implementation

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "rdma.h"
#include "rdma_net.h"
/* ============================================
 * GLOBAL STATE
 * ============================================ */

struct rdma_mr mr_table[MAX_MRS];
struct spinlock mr_lock;

struct rdma_qp qp_table[MAX_QPS];
struct spinlock qp_lock;

/* ============================================
 * SOFTWARE LOOPBACK IMPLEMENTATION
 * ============================================ */

/* Process work requests in software (loopback mode)
 * 
 * This function replaces hardware processing. It:
 * 1. Processes all pending work requests in the send queue
 * 2. Performs memory operations directly (no DMA, no network)
 * 3. Posts completions to the completion queue
 * 
 * This is a simplified software-only approach that doesn't require
 * QEMU modifications. Perfect for local testing and development.
 */
static void
rdma_process_work_requests(int qp_id, struct rdma_qp *qp)
{
    // Process all pending work requests
    while (qp->sq_head != qp->sq_tail) {
        struct rdma_work_request *wr = &qp->sq[qp->sq_head];
        
        // Validate source MR
        struct rdma_mr *src_mr = rdma_mr_get(wr->local_mr_id);
        if (!src_mr) {
            // Post error completion
            struct rdma_completion comp = {
                .wr_id = wr->wr_id,
                .byte_len = 0,
                .status = RDMA_WC_LOC_PROT_ERR,
                .opcode = wr->opcode,
                .reserved = 0
            };
            qp->cq[qp->cq_tail] = comp;
            qp->cq_tail = (qp->cq_tail + 1) % qp->cq_size;
            qp->stats_errors++;
            goto next_wr;
        }
        
        // Check operation mode: network vs loopback
        if (qp->network_mode && qp->state == QP_STATE_RTS) {
            // Network mode: Send RDMA packet
            switch (wr->opcode) {
            case RDMA_OP_WRITE:
                if (rdma_net_tx_write(qp, wr) < 0) {
                    // Post error completion
                    struct rdma_completion comp = {
                        .wr_id = wr->wr_id,
                        .byte_len = 0,
                        .status = RDMA_WC_LOC_PROT_ERR,
                        .opcode = wr->opcode,
                    };
                    qp->cq[qp->cq_tail] = comp;
                    qp->cq_tail = (qp->cq_tail + 1) % qp->cq_size;
                    qp->stats_errors++;
                }
                // Note: Completion will be posted when ACK is received
                break;
                
            default:
                // Unsupported opcode in network mode
                break;
            }
        } else {
            // Loopback mode: Use existing local processing
            
            // Process based on opcode
            uint8 status = RDMA_WC_SUCCESS;
            
            switch (wr->opcode) {
            case RDMA_OP_WRITE: {
                // Validate destination MR
                struct rdma_mr *dst_mr = rdma_mr_get(wr->remote_mr_id);
                if (!dst_mr) {
                    status = RDMA_WC_REM_ACCESS_ERR;
                    break;
                }
                
                // Check access permissions
                if (!(dst_mr->hw.access_flags & RDMA_ACCESS_REMOTE_WRITE)) {
                    status = RDMA_WC_REM_ACCESS_ERR;
                    break;
                }
                
                // remote_addr can be either:
                // 1. An offset within the MR (when < length)
                // 2. An absolute virtual address within the MR (when >= vaddr)
                uint64 offset;
                if (wr->remote_addr >= dst_mr->hw.vaddr && 
                    wr->remote_addr < dst_mr->hw.vaddr + dst_mr->hw.length) {
                    // Absolute address - convert to offset
                    offset = wr->remote_addr - dst_mr->hw.vaddr;
                } else if (wr->remote_addr < dst_mr->hw.length) {
                    // Already an offset
                    offset = wr->remote_addr;
                } else {
                    // Invalid address
                    status = RDMA_WC_REM_INV_REQ;
                    break;
                }
                
                // Check destination bounds
                if (offset + wr->length > dst_mr->hw.length) {
                    status = RDMA_WC_REM_INV_REQ;
                    break;
                }
                
                // Perform software memory copy (local_offset already has physical address)
                uint64 src_addr = wr->local_offset;
                uint64 dst_addr = dst_mr->hw.paddr + offset;
                
                // Copy memory directly (destination, source, length)
                memmove((void *)dst_addr, (void *)src_addr, wr->length);
                
                break;
            }
            
            case RDMA_OP_READ:
                // Not yet implemented - return error
                status = RDMA_WC_LOC_PROT_ERR;
                break;
                
            case RDMA_OP_SEND:
                // Not yet implemented - return error
                status = RDMA_WC_LOC_PROT_ERR;
                break;
                
            default:
                status = RDMA_WC_LOC_PROT_ERR;
                break;
            }
            
            // Post completion if signaled or if error occurred
            if ((wr->flags & RDMA_WR_SIGNALED) || (status != RDMA_WC_SUCCESS)) {
                struct rdma_completion comp = {
                    .wr_id = wr->wr_id,
                    .byte_len = (status == RDMA_WC_SUCCESS) ? wr->length : 0,
                    .status = status,
                    .opcode = wr->opcode,
                    .reserved = 0
                };
                
                qp->cq[qp->cq_tail] = comp;
                qp->cq_tail = (qp->cq_tail + 1) % qp->cq_size;
                
                if (status == RDMA_WC_SUCCESS) {
                    qp->stats_completions++;
                } else {
                    qp->stats_errors++;
                }
            }
        }
        
        // Decrement MR refcount
        acquire(&mr_lock);
        src_mr->refcount--;
        release(&mr_lock);
        
next_wr:
        // Move to next work request
        qp->sq_head = (qp->sq_head + 1) % qp->sq_size;
        qp->outstanding_ops--;
    }
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
        printf("rdma_mr_register: address out of bounds (addr=0x%lx, len=%ld, sz=%ld)\n",
            addr, len, p->sz);
        return -1;
    }

    // Check that the memory region is not crossing a page boundary
    uint64 start_page = PGROUNDDOWN(addr);
    uint64 end_page = PGROUNDDOWN(addr + len - 1);
    
    if (start_page != end_page) {
        printf("rdma_mr_register: MR cannot cross page boundary (addr=0x%lx len=%ld)\n",
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
    
    printf("rdma_mr: registered MR %d for PID %d: vaddr=0x%lx paddr=0x%lx len=%ld flags=0x%x\n",
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
    
    // Initialize network fields
    qp->network_mode = 0;  // Start in loopback mode
    qp->tx_seq_num = 0;
    qp->rx_expected_seq = 0;
    for (int i = 0; i < 64; i++) {
        qp->pending_acks[i].valid = 0;
    }
    
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
    
    // Check QP state - must be INIT (for loopback) or RTS (for network after connect)
    if (qp->state != QP_STATE_INIT && qp->state != QP_STATE_RTR && qp->state != QP_STATE_RTS) {
        release(&qp_lock);
        // Undo refcount
        acquire(&mr_lock);
        mr->refcount--;
        release(&mr_lock);
        printf("rdma_qp_post_send: QP %d not in valid state (state=%d)\n", qp_id, qp->state);
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
    
    // Ensure SQ write is visible before processing
    __sync_synchronize();
    
    // Process work requests immediately in software
    rdma_process_work_requests(qp_id, qp);
    
    release(&qp_lock);
    
    return 0;
}

/* Poll completion queue for completed operations
 * 
 * In software loopback mode, this is simplified since work requests
 * are processed synchronously in rdma_qp_post_send.
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
    
    acquire(&qp_lock);
    
    struct rdma_qp *qp = &qp_table[qp_id];
    
    // Check ownership
    if (!qp->valid || qp->owner != myproc()) {
        release(&qp_lock);
        return -1;
    }
    
    // Memory barrier to ensure we see latest CQ data
    __sync_synchronize();
    
    int n = 0;
    
    // Copy completions from CQ to user buffer
    while (qp->cq_head != qp->cq_tail && n < max_comps) {
        comp_array[n] = qp->cq[qp->cq_head];
        
        // Update statistics
        if (comp_array[n].status == RDMA_WC_SUCCESS) {
            qp->stats_completions++;
        } else {
            qp->stats_errors++;
        }
        
        qp->cq_head = (qp->cq_head + 1) % qp->cq_size;
        n++;
    }
    
    release(&qp_lock);
    
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
    
    // Validate QP exists and is owned by current process
    if (!qp->valid || qp->owner != myproc()) {
        release(&qp_lock);
        return -1;
    }
    
    // QP must be in INIT state
    if (qp->state != QP_STATE_INIT) {
        release(&qp_lock);
        printf("rdma_qp_connect: QP %d not in INIT state (state=%d)\n", qp_id, qp->state);
        return -1;
    }
    
    // Store remote connection information
    memmove(qp->remote_mac, mac, 6);
    qp->remote_qp_num = remote_qp;
    qp->network_mode = 1;
    qp->connected = 1;
    
    // Initialize sequence numbers
    qp->tx_seq_num = 1;
    qp->rx_expected_seq = 1;
    
    // Transition to RTR (Ready To Receive) then immediately to RTS (Ready To Send)
    // In a real RDMA implementation, RTR→RTS would be a separate step,
    // but for our simple implementation we can do it immediately
    qp->state = QP_STATE_RTS;
    
    release(&qp_lock);
    
    printf("rdma_qp_connect: QP %d connected to remote QP %d (MAC: %x:%x:%x:%x:%x:%x)\n",
           qp_id, remote_qp, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return 0;
}

/* ============================================
 * KERNEL-SPACE UNIT TESTS
 * ============================================ */

#ifdef RDMA_TESTING
#include "rdma_test.h"
#endif

/* ============================================
 * INITIALIZATION
 * ============================================ */

void
rdma_init(void)
{
    printf("rdma: initializing subsystem (software loopback mode)\n");
    
    rdma_mr_init();
    rdma_qp_init();
    
    printf("rdma: initialization complete\n");
    
#ifdef RDMA_TESTING
    // Run kernel tests immediately after initialization
    rdma_run_kernel_tests();
#endif
}