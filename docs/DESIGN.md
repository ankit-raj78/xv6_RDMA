# RDMA xv6 Design Documentation

## Architecture Overview

This is a **software-only loopback** RDMA implementation for xv6. It provides RDMA semantics (memory registration, queue pairs, work requests, completions) without requiring QEMU modifications or network hardware simulation.

### System Layers
```
+------------------+
|  User Programs   |
+------------------+
|   RDMA Library   |  (user/rdma.h)
+------------------+
|  System Calls    |
+------------------+
|   RDMA Core      |  (kernel/rdma.c) - SOFTWARE LOOPBACK
+------------------+
| Virtual Memory   |  (kernel/vm.c)
+------------------+
```

### Key Design Decision: Software Loopback

**Why Software-Only?**
- Eliminates dependency on modified QEMU
- Simplifies development and testing
- Focuses on RDMA semantics rather than hardware emulation
- Perfect for educational purposes and protocol development

**How It Works:**
1. RDMA operations (RDMA_WRITE) execute synchronously
2. Memory copies happen directly via `memmove()`
3. Completions are posted immediately to CQ
4. No network packets, no DMA, no hardware registers

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
1. **RDMA_WRITE**: Write local memory to remote memory (synchronous, software)
2. **RDMA_READ**: Not yet implemented
3. **RDMA_SEND**: Not yet implemented

### Operation Flow (Software Loopback)
```
1. User calls rdma_qp_post_send(qp, work_request)
   ↓
2. Kernel validates MRs and permissions
   ↓  
3. rdma_process_work_requests() executes immediately:
   - Validates source and destination MRs
   - Performs memmove(dst_paddr, src_paddr, length)
   - Posts completion to CQ
   ↓
4. User calls rdma_qp_poll_cq() to get completion
```

### Simplifications vs Production RDMA

| Feature | Production RDMA | This Implementation |
|---------|----------------|---------------------|
| Network | Real packets over Ethernet | No network, local only |
| DMA | Hardware DMA engines | Software memcpy/memmove |
| Async Processing | Hardware processes WRs async | Synchronous execution |
| Reliability | ACK/NAK, retry logic | Best-effort, no retry |
| QP Types | RC, UC, UD | Single unified type |
| Operations | 10+ opcodes | RDMA_WRITE only |
| Atomics | Compare-and-swap, fetch-add | Not implemented |
| QEMU | Modified E1000 emulation | No QEMU changes needed |

## Implementation Status

- [x] Memory Registration (Days 2-3)
- [x] Queue Pairs (Days 4-6)
- [x] Software Loopback RDMA WRITE (Day 7)
- [ ] Network RDMA (Days 8-11) - Not applicable in software mode
- [ ] Additional Operations (Day 12)
