// kernel/rdma.h - RDMA subsystem for xv6

#ifndef _RDMA_H_
#define _RDMA_H_

#include "types.h"
#include "spinlock.h"

/* ============================================
 * CONSTANTS AND CONFIGURATION
 * ============================================ */

#define MAX_MRS 64              // Maximum memory regions system-wide
#define MAX_QPS 16              // Maximum queue pairs system-wide
#define DEFAULT_SQ_SIZE 64      // Default send queue size
#define DEFAULT_CQ_SIZE 64      // Default completion queue size

/* ============================================
 * MEMORY REGION (MR) MANAGEMENT
 * ============================================ */

/* Access permission flags - what operations are allowed on this memory */
#define RDMA_ACCESS_LOCAL_READ    0x01  // Process can read locally
#define RDMA_ACCESS_LOCAL_WRITE   0x02  // Process can write locally
#define RDMA_ACCESS_REMOTE_READ   0x04  // Remote can read via RDMA
#define RDMA_ACCESS_REMOTE_WRITE  0x08  // Remote can write via RDMA

/* Hardware-visible MR structure (MUST match QEMU's e1000.c definition)
 * This is what QEMU reads via DMA when validating RDMA operations
 */
struct rdma_mr_hw {
    uint32 id;                   // MR identifier (1-based, 0 = invalid)
    uint32 access_flags;         // Permission bits (combination of RDMA_ACCESS_*)
    uint64 vaddr;                // Virtual address in user space
    uint64 paddr;                // Physical address for DMA
    uint64 length;               // Size in bytes
    uint32 lkey;                 // Local key for validation
    uint32 rkey;                 // Remote key for validation
    uint32 valid;                // 1 = active, 0 = free slot
} __attribute__((packed));

/* Full Memory Region structure with kernel metadata
 * The hw part MUST be first so &mr_table[0].hw gives correct pointer to QEMU
 */
struct rdma_mr {
    struct rdma_mr_hw hw;        // Hardware-visible part (MUST be first!)
    
    /* Kernel-only metadata (not visible to QEMU/hardware) */
    struct proc *owner;          // Process that owns this MR (for fast access)
    int owner_pid;               // PID at registration time (for safe validation)
    int refcount;                // Reference count for in-flight operations
};

/* Global MR table and lock */
extern struct rdma_mr mr_table[MAX_MRS];
extern struct spinlock mr_lock;

/* MR management functions */
void rdma_mr_init(void);
int rdma_mr_register(uint64 addr, uint64 len, int flags);
int rdma_mr_deregister(int mr_id);
struct rdma_mr* rdma_mr_get(int mr_id);

/* ============================================
 * QUEUE PAIR (QP) MANAGEMENT
 * ============================================ */

/* RDMA opcodes - types of operations */
#define RDMA_OP_WRITE      0x01  // Write local data to remote memory
#define RDMA_OP_READ       0x02  // Read remote memory to local
#define RDMA_OP_SEND       0x03  // Send message
#define RDMA_OP_READ_RESP  0x04  // Response to READ request

/* Work Request flags */
#define RDMA_WR_SIGNALED   (1 << 0)  // Generate completion entry when done

/* QP State Machine - tracks queue pair lifecycle */
enum rdma_qp_state {
    QP_STATE_RESET = 0,          // Initial state, not configured
    QP_STATE_INIT,               // Allocated, being configured
    QP_STATE_READY,              // Ready for operations
    QP_STATE_ERROR               // Error state, needs reset
};

/* Work Request - describes an RDMA operation to perform */
struct rdma_work_request {
    uint64 wr_id;                // User-provided identifier for tracking
    uint8 opcode;                // RDMA_OP_* operation type
    uint8 flags;                 // Control flags (RDMA_WR_SIGNALED, etc.)
    uint16 reserved;             // Padding for alignment
    uint32 local_mr_id;          // Source memory region ID
    uint64 local_offset;         // Offset within local MR
    uint32 remote_mr_id;         // Destination memory region ID
    uint64 remote_addr;          // Remote memory address
    uint32 remote_key;           // Remote key for validation
    uint32 length;               // Transfer length in bytes
} __attribute__((packed));

/* Completion status codes */
#define RDMA_WC_SUCCESS        0x00  // Operation completed successfully
#define RDMA_WC_LOC_PROT_ERR   0x01  // Local protection violation
#define RDMA_WC_REM_ACCESS_ERR 0x02  // Remote access denied
#define RDMA_WC_LOC_LEN_ERR    0x03  // Local length error
#define RDMA_WC_REM_INV_REQ    0x04  // Remote invalid request

/* Completion Entry - reports completion of an operation */
struct rdma_completion {
    uint64 wr_id;                // Matches work_request.wr_id
    uint32 byte_len;             // Bytes actually transferred
    uint8 status;                // RDMA_WC_* status code
    uint8 opcode;                // Echo of operation type
    uint16 reserved;             // Padding
} __attribute__((packed));

/* Queue Pair - send queue + completion queue */
struct rdma_qp {
    int id;                              // QP identifier (0-15)
    
    // Send Queue (SQ) - where user posts work requests
    struct rdma_work_request *sq;        // Ring buffer in kernel memory
    uint32 sq_head;                      // Next entry to process (kernel)
    uint32 sq_tail;                      // Next entry to submit (user)
    uint32 sq_size;                      // Queue size (must be power of 2)
    uint64 sq_paddr;                     // Physical address for DMA
    
    // Completion Queue (CQ) - where NIC posts completions
    struct rdma_completion *cq;          // Ring buffer in kernel memory
    uint32 cq_head;                      // Next entry to read (user)
    uint32 cq_tail;                      // Next entry to write (NIC)
    uint32 cq_size;                      // Queue size (must be power of 2)
    uint64 cq_paddr;                     // Physical address for DMA
    
    struct proc *owner;                  // Owning process
    int valid;                           // 1 = active, 0 = free
    
    /* State management and flow control */
    enum rdma_qp_state state;            // QP state machine
    uint32 outstanding_ops;              // Number of operations in flight
    
    /* Network RDMA connection info (for Days 8-11: two-host RDMA) */
    uint8 remote_mac[6];                 // Destination MAC address
    uint32 remote_qp_num;                // Remote QP to target
    int connected;                       // 0 = not connected, 1 = connected
    
    /* Statistics and debugging (useful for evaluation phase) */
    uint32 stats_sends;                  // Total send operations posted
    uint32 stats_completions;            // Total completions received
    uint32 stats_errors;                 // Total errors encountered
};

/* Global QP table and lock */
extern struct rdma_qp qp_table[MAX_QPS];
extern struct spinlock qp_lock;

/* QP management functions */
void rdma_qp_init(void);
int rdma_qp_create(uint32 sq_size, uint32 cq_size);
int rdma_qp_destroy(int qp_id);
int rdma_qp_post_send(int qp_id, struct rdma_work_request *wr);
int rdma_qp_poll_cq(int qp_id, struct rdma_completion *comp, int max_comps);

/* QP connection management (for network RDMA) */
int rdma_qp_connect(int qp_id, uint8 mac[6], uint32 remote_qp);

/* ============================================
 * HARDWARE INTERFACE (E1000 RDMA Registers)
 * ============================================ */

/* Base address for RDMA registers in E1000 NIC */
#define E1000_RDMA_BASE 0x40005800UL // TODO: check the actual address

/* Register offsets (must match QEMU implementation) */
#define E1000_RDMA_CTRL    0x00  // Control register
#define E1000_RDMA_STATUS  0x04  // Status register
#define E1000_MR_TABLE_PTR 0x08  // MR table physical address
#define E1000_MR_TABLE_LEN 0x0C  // MR table length

/* Queue Pair register block */
#define E1000_QP_BASE      0x100 // QP registers start
#define E1000_QP_STRIDE    0x20  // 32 bytes per QP

/* Per-QP register offsets (add to QP_BASE + qp_id*QP_STRIDE) */
#define E1000_QP_SQ_BASE   0x00  // Send Queue base address
#define E1000_QP_SQ_SIZE   0x08  // Send Queue size
#define E1000_QP_SQ_HEAD   0x0C  // Send Queue head (read by kernel)
#define E1000_QP_SQ_TAIL   0x10  // Send Queue tail (doorbell!)
#define E1000_QP_CQ_BASE   0x14  // Completion Queue base
#define E1000_QP_CQ_SIZE   0x18  // Completion Queue size
#define E1000_QP_CQ_HEAD   0x1C  // Completion Queue head
#define E1000_QP_CQ_TAIL   0x20  // Completion Queue tail

/* Control register bits */
#define RDMA_CTRL_ENABLE  (1 << 0)  // Enable RDMA
#define RDMA_CTRL_RESET   (1 << 1)  // Reset RDMA state

/* Status register bits */
#define RDMA_STATUS_READY (1 << 0)  // RDMA ready

/* Hardware interface functions */
void rdma_hw_init(void);
void rdma_hw_enable(void);
void rdma_hw_setup_qp(int qp_id, struct rdma_qp *qp);
void rdma_hw_ring_doorbell(int qp_id, uint32 sq_tail);

/* ============================================
 * HELPER FUNCTIONS
 * ============================================ */

/* Inline helper to safely check MR ownership */
static inline int
rdma_mr_is_owned_by_current(struct rdma_mr *mr, struct proc *p)
{
    return (mr->owner == p && mr->owner_pid == p->pid);
}

/* ============================================
 * INITIALIZATION
 * ============================================ */
void rdma_init(void);

#endif // _RDMA_H_