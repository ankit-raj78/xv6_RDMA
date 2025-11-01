# RDMA xv6 Design Documentation

## Architecture Overview

[Diagram to be added]

### System Layers
```
+------------------+
|  User Programs   |
+------------------+
|   RDMA Library   |  (user/rdma.h)
+------------------+
|  System Calls    |
+------------------+
|   RDMA Core      |  (kernel/rdma.c)
+------------------+
|  E1000 Driver    |  (kernel/e1000.c)
+------------------+
|    Hardware      |
+------------------+
```

## Memory Registration

### Design Goals
- Pin user memory pages to prevent swapping
- Translate virtual to physical addresses
- Manage access permissions (local/remote read/write)
- Track memory region lifecycle

### Data Structures
```c
struct rdma_mr {
  uint64 addr;           // Virtual address
  uint64 length;         // Region size
  uint64 paddr;          // Physical address (array for multi-page)
  uint32 lkey;           // Local protection key
  uint32 rkey;           // Remote protection key
  int access_flags;      // Permission bits
  int refcount;          // Reference counting
};
```

### API
- `rdma_reg_mr(addr, length, access_flags)` - Register memory region
- `rdma_dereg_mr(mr_handle)` - Deregister memory region

## Queue Pairs

### Design Goals
- Manage send and receive queues
- Handle work requests and completions
- Provide per-process isolation
- Support multiple QPs per process

### Data Structures
```c
struct rdma_qp {
  int qp_num;            // Queue pair number
  int state;             // QP state (RESET, INIT, RTR, RTS)
  struct rdma_cq *send_cq;
  struct rdma_cq *recv_cq;
  struct rdma_sq send_queue;
  struct rdma_rq recv_queue;
};

struct rdma_cq {
  int cq_num;
  int num_entries;
  struct rdma_cqe *entries;  // Completion queue entries
  int head, tail;            // Ring buffer indices
};
```

## RDMA Operations

### Supported Operations
1. **RDMA_WRITE**: Write local memory to remote memory
2. **RDMA_READ**: Read remote memory to local (simplified)
3. **RDMA_SEND**: Send message (zero-copy)

### Packet Format
```
+----------------+
| Ethernet Hdr   |
+----------------+
| RDMA Header    |
| - opcode       |
| - rkey         |
| - remote_addr  |
| - length       |
+----------------+
| Data           |
+----------------+
```

## E1000 Extensions

### Modifications Required
- Add RDMA descriptor format
- Implement zero-copy DMA
- Add RDMA packet handling
- Support doorbell mechanism

## Simplifications vs Production

1. **Reliability**: Best-effort (no ACK/retry)
2. **QP Types**: Single unified type (not RC/UC/UD)
3. **Operations**: 3 core ops (not 10+)
4. **Memory**: Static regions (no dynamic windows)
5. **Atomics**: Not implemented
6. **Connection**: Simplified management

## Implementation Status

- [ ] Memory Registration (Days 2-3)
- [ ] Queue Pairs (Days 4-6)
- [ ] Loopback RDMA (Day 7)
- [ ] Network RDMA (Days 8-11)
- [ ] Additional Operations (Day 12)
