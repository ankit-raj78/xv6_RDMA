# RDMA API Documentation

## User-Space API (user/rdma.h)

### Memory Registration

#### `rdma_reg_mr()`
```c
int rdma_reg_mr(void *addr, uint64 length, int access_flags);
```
**Description**: Register a memory region for RDMA operations.

**Parameters**:
- `addr`: Starting virtual address of memory region
- `length`: Size of memory region in bytes
- `access_flags`: Bitwise OR of:
  - `RDMA_ACCESS_LOCAL_WRITE`: Allow local writes
  - `RDMA_ACCESS_REMOTE_WRITE`: Allow remote writes
  - `RDMA_ACCESS_REMOTE_READ`: Allow remote reads

**Returns**: Memory region handle (>=0) on success, -1 on error

**Example**:
```c
char buffer[4096];
int mr = rdma_reg_mr(buffer, sizeof(buffer), 
                     RDMA_ACCESS_LOCAL_WRITE | RDMA_ACCESS_REMOTE_WRITE);
if (mr < 0) {
    printf("Failed to register memory\n");
    exit(1);
}
```

---

#### `rdma_dereg_mr()`
```c
int rdma_dereg_mr(int mr_handle);
```
**Description**: Deregister a previously registered memory region.

**Parameters**:
- `mr_handle`: Handle returned from `rdma_reg_mr()`

**Returns**: 0 on success, -1 on error

---

### Queue Pair Management

#### `rdma_create_qp()`
```c
int rdma_create_qp(int send_cq_size, int recv_cq_size);
```
**Description**: Create a queue pair for RDMA communication.

**Parameters**:
- `send_cq_size`: Number of entries in send completion queue
- `recv_cq_size`: Number of entries in receive completion queue

**Returns**: Queue pair number (>=0) on success, -1 on error

**Example**:
```c
int qp = rdma_create_qp(64, 64);
if (qp < 0) {
    printf("Failed to create QP\n");
    exit(1);
}
```

---

#### `rdma_destroy_qp()`
```c
int rdma_destroy_qp(int qp_num);
```
**Description**: Destroy a queue pair.

**Parameters**:
- `qp_num`: Queue pair number from `rdma_create_qp()`

**Returns**: 0 on success, -1 on error

---

### RDMA Operations

#### `rdma_post_write()`
```c
int rdma_post_write(int qp_num, int local_mr, uint64 local_offset,
                    uint64 remote_addr, uint32 rkey, uint64 length);
```
**Description**: Post an RDMA WRITE operation.

**Parameters**:
- `qp_num`: Queue pair to use
- `local_mr`: Local memory region handle
- `local_offset`: Offset within local memory region
- `remote_addr`: Remote virtual address
- `rkey`: Remote key for access validation
- `length`: Number of bytes to write

**Returns**: Work request ID (>=0) on success, -1 on error

**Example**:
```c
char data[1024] = "Hello RDMA!";
int mr = rdma_reg_mr(data, sizeof(data), RDMA_ACCESS_LOCAL_WRITE);
int qp = rdma_create_qp(64, 64);

// Write to remote address 0x80000000
int wr_id = rdma_post_write(qp, mr, 0, 0x80000000, remote_rkey, 1024);
```

---

#### `rdma_post_send()`
```c
int rdma_post_send(int qp_num, int local_mr, uint64 length);
```
**Description**: Post an RDMA SEND operation (message passing).

**Parameters**:
- `qp_num`: Queue pair to use
- `local_mr`: Local memory region containing message
- `length`: Message length in bytes

**Returns**: Work request ID (>=0) on success, -1 on error

---

#### `rdma_poll_cq()`
```c
int rdma_poll_cq(int qp_num, struct rdma_wc *wc, int max_entries);
```
**Description**: Poll completion queue for completed operations.

**Parameters**:
- `qp_num`: Queue pair to poll
- `wc`: Array to store completion entries
- `max_entries`: Maximum number of completions to retrieve

**Returns**: Number of completions found (>=0), -1 on error

**Example**:
```c
struct rdma_wc wc[10];
int n = rdma_poll_cq(qp, wc, 10);
for (int i = 0; i < n; i++) {
    if (wc[i].status == RDMA_WC_SUCCESS) {
        printf("Operation %d completed successfully\n", wc[i].wr_id);
    } else {
        printf("Operation %d failed\n", wc[i].wr_id);
    }
}
```

---

## Data Structures

### `struct rdma_wc` - Work Completion
```c
struct rdma_wc {
    int wr_id;          // Work request ID
    int status;         // Completion status
    int opcode;         // Operation type
    uint64 byte_len;    // Number of bytes transferred
};
```

**Status Values**:
- `RDMA_WC_SUCCESS`: Operation completed successfully
- `RDMA_WC_LOC_PROT_ERR`: Local protection error
- `RDMA_WC_REM_ACCESS_ERR`: Remote access error
- `RDMA_WC_GENERAL_ERR`: General error

---

## Complete Example Program

```c
#include "kernel/types.h"
#include "user/user.h"
#include "user/rdma.h"

int main(void) {
    // Allocate and register memory
    char send_buf[4096] = "Test message";
    char recv_buf[4096];
    
    int send_mr = rdma_reg_mr(send_buf, sizeof(send_buf), 
                              RDMA_ACCESS_LOCAL_WRITE);
    int recv_mr = rdma_reg_mr(recv_buf, sizeof(recv_buf),
                              RDMA_ACCESS_LOCAL_WRITE | RDMA_ACCESS_REMOTE_WRITE);
    
    // Create queue pair
    int qp = rdma_create_qp(64, 64);
    
    // Post RDMA write
    uint64 remote_addr = 0x80000000;  // Example remote address
    uint32 remote_rkey = 12345;        // Example remote key
    
    int wr_id = rdma_post_write(qp, send_mr, 0, remote_addr, remote_rkey, 12);
    
    // Poll for completion
    struct rdma_wc wc;
    while (rdma_poll_cq(qp, &wc, 1) == 0) {
        // Spin waiting for completion
    }
    
    if (wc.status == RDMA_WC_SUCCESS) {
        printf("RDMA write completed successfully!\n");
    }
    
    // Cleanup
    rdma_destroy_qp(qp);
    rdma_dereg_mr(send_mr);
    rdma_dereg_mr(recv_mr);
    
    exit(0);
}
```

---

## Error Handling

All RDMA functions return -1 on error. Use `errno` (future enhancement) or check return values carefully.

Common errors:
- Invalid memory region handle
- Invalid queue pair number
- Memory region not registered
- Insufficient permissions
- Queue full

---

## Notes

1. **Page Alignment**: For best performance, align memory regions to page boundaries (4096 bytes)
2. **Synchronization**: RDMA operations are asynchronous - always poll for completions
3. **Cleanup**: Always deregister memory regions and destroy queue pairs before exit
4. **Multiple QPs**: A process can create multiple queue pairs for parallel operations
