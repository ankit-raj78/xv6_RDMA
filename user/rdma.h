// user/rdma.h - User-space RDMA library for xv6

#ifndef _USER_RDMA_H_
#define _USER_RDMA_H_

/* ============================================
 * CONSTANTS
 * ============================================ */

// RDMA opcodes
#define RDMA_OP_WRITE      0x01
#define RDMA_OP_READ       0x02
#define RDMA_OP_SEND       0x03
#define RDMA_OP_READ_RESP  0x04

// Access flags for memory regions
#define RDMA_ACCESS_LOCAL_READ    0x01
#define RDMA_ACCESS_LOCAL_WRITE   0x02
#define RDMA_ACCESS_REMOTE_READ   0x04
#define RDMA_ACCESS_REMOTE_WRITE  0x08

// Work request flags
#define RDMA_WR_SIGNALED   (1 << 0)

// Completion status codes
#define RDMA_WC_SUCCESS        0x00
#define RDMA_WC_LOC_PROT_ERR   0x01
#define RDMA_WC_REM_ACCESS_ERR 0x02
#define RDMA_WC_LOC_LEN_ERR    0x03
#define RDMA_WC_REM_INV_REQ    0x04

/* ============================================
 * DATA STRUCTURES
 * ============================================ */

// Work request structure - describes RDMA operation
struct rdma_work_request {
    unsigned long wr_id;         // User-provided ID for tracking
    unsigned char opcode;        // RDMA_OP_*
    unsigned char flags;         // RDMA_WR_*
    unsigned short reserved;
    unsigned int local_mr_id;    // Source memory region
    unsigned long local_offset;  // Offset in source MR
    unsigned int remote_mr_id;   // Destination memory region
    unsigned long remote_addr;   // Remote address
    unsigned int remote_key;     // Remote key
    unsigned int length;         // Transfer size
} __attribute__((packed));

// Completion entry - reports operation completion
struct rdma_completion {
    unsigned long wr_id;         // Matches work_request.wr_id
    unsigned int byte_len;       // Bytes transferred
    unsigned char status;        // RDMA_WC_*
    unsigned char opcode;        // Operation type
    unsigned short reserved;
} __attribute__((packed));

/* ============================================
 * SYSTEM CALL WRAPPERS
 * ============================================ */

// Register memory region
// Returns: mr_id >= 0 on success, -1 on failure
int rdma_reg_mr(void *addr, unsigned long len, int flags);

// Deregister memory region
// Returns: 0 on success, -1 on failure
int rdma_dereg_mr(int mr_id);

// Create queue pair
// Returns: qp_id >= 0 on success, -1 on failure
int rdma_create_qp(int sq_size, int cq_size);

// Destroy queue pair
// Returns: 0 on success, -1 on failure
int rdma_destroy_qp(int qp_id);

// Post send work request
// Returns: 0 on success, -1 on failure
int rdma_post_send(int qp_id, struct rdma_work_request *wr);

// Poll completion queue
// Returns: number of completions (>= 0), -1 on failure
int rdma_poll_cq(int qp_id, struct rdma_completion *comps, int max_comps);

// Connect QP to remote peer
// Returns: 0 on success, -1 on failure
int rdma_connect(int qp_id, unsigned char mac[6], unsigned int remote_qp);

/* ============================================
 * HELPER FUNCTIONS
 * ============================================ */

// Build an RDMA_WRITE work request
static inline void
rdma_build_write_wr(struct rdma_work_request *wr,
                   unsigned long wr_id,
                   int local_mr_id,
                   unsigned long local_offset,
                   int remote_mr_id,
                   unsigned long remote_addr,
                   unsigned int remote_key,
                   unsigned int length)
{
    wr->wr_id = wr_id;
    wr->opcode = RDMA_OP_WRITE;
    wr->flags = RDMA_WR_SIGNALED;
    wr->reserved = 0;
    wr->local_mr_id = local_mr_id;
    wr->local_offset = local_offset;
    wr->remote_mr_id = remote_mr_id;
    wr->remote_addr = remote_addr;
    wr->remote_key = remote_key;
    wr->length = length;
}

// Build an RDMA_READ work request
static inline void
rdma_build_read_wr(struct rdma_work_request *wr,
                  unsigned long wr_id,
                  int local_mr_id,
                  unsigned long local_offset,
                  int remote_mr_id,
                  unsigned long remote_addr,
                  unsigned int remote_key,
                  unsigned int length)
{
    wr->wr_id = wr_id;
    wr->opcode = RDMA_OP_READ;
    wr->flags = RDMA_WR_SIGNALED;
    wr->reserved = 0;
    wr->local_mr_id = local_mr_id;
    wr->local_offset = local_offset;
    wr->remote_mr_id = remote_mr_id;
    wr->remote_addr = remote_addr;
    wr->remote_key = remote_key;
    wr->length = length;
}

// Check if completion indicates success
static inline int
rdma_comp_is_success(struct rdma_completion *comp)
{
    return (comp->status == RDMA_WC_SUCCESS);
}

// Get completion status string
static inline char*
rdma_comp_status_str(unsigned char status)
{
    switch (status) {
        case RDMA_WC_SUCCESS:        return "SUCCESS";
        case RDMA_WC_LOC_PROT_ERR:   return "LOCAL_PROT_ERR";
        case RDMA_WC_REM_ACCESS_ERR: return "REMOTE_ACCESS_ERR";
        case RDMA_WC_LOC_LEN_ERR:    return "LOCAL_LEN_ERR";
        case RDMA_WC_REM_INV_REQ:    return "REMOTE_INV_REQ";
        default:                      return "UNKNOWN";
    }
}

#endif // _USER_RDMA_H_
