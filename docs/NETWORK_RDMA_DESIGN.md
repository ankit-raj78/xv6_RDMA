# Network RDMA Architecture Design

## Overview

This document describes the architecture for extending xv6's software-only RDMA to support **two-host RDMA** communication over the network using the E1000 NIC.

## Current State vs Target State

### Current: Software Loopback
```
┌─────────────────────────────────┐
│         xv6 Instance            │
│                                 │
│  ┌──────┐    memmove()   ┌──────┐
│  │ MR 1 │ ──────────────> │ MR 2 │
│  └──────┘                 └──────┘
│         (same process)          │
└─────────────────────────────────┘
```

### Target: Network RDMA
```
┌──────────────────┐                    ┌──────────────────┐
│   xv6 Host A     │                    │   xv6 Host B     │
│                  │                    │                  │
│  ┌──────┐        │   Ethernet/IP     │        ┌──────┐  │
│  │ MR 1 │────────┼───────────────────┼───────>│ MR 2 │  │
│  └──────┘        │   RDMA Packets    │        └──────┘  │
│   (QP 0)         │                    │         (QP 1)   │
│                  │                    │                  │
└──────────────────┘                    └──────────────────┘
```

## Architecture Layers

### Full Stack View
```
┌─────────────────────────────────────────────────────────┐
│                    User Application                      │
│  (rdmatest, uses rdma_reg_mr, rdma_post_send, etc.)    │
└─────────────────────────────────────────────────────────┘
                         ↓ System Calls
┌─────────────────────────────────────────────────────────┐
│              RDMA Subsystem (kernel/rdma.c)             │
│  ┌───────────────┐  ┌──────────────┐  ┌─────────────┐ │
│  │ MR Management │  │ QP Management│  │  Completion │ │
│  └───────────────┘  └──────────────┘  │   Queue     │ │
│                                        └─────────────┘ │
│  ┌─────────────────────────────────────────────────┐  │
│  │        Work Request Processing                   │  │
│  │  - rdma_process_work_requests()                 │  │
│  │  - Mode: SOFTWARE_LOOPBACK | NETWORK            │  │
│  └─────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
                         ↓ Network Mode
┌─────────────────────────────────────────────────────────┐
│            RDMA Network Layer (NEW)                     │
│  ┌──────────────┐  ┌─────────────┐  ┌──────────────┐  │
│  │ Packet Build │  │ TX Handler  │  │  RX Handler  │  │
│  └──────────────┘  └─────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────────┐
│         Network Stack (kernel/net.c, kernel/e1000.c)    │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│  │  mbuf    │  │ Ethernet │  │  E1000   │             │
│  │ Mgmt     │  │  Layer   │  │  Driver  │             │
│  └──────────┘  └──────────┘  └──────────┘             │
└─────────────────────────────────────────────────────────┘
                         ↓
                    Physical NIC
```

## Packet Format Design

### RDMA Packet Structure
```
┌────────────────────────────────────────────────────────┐
│                  Ethernet Header                        │
│  ┌──────────┬──────────┬──────────┐                   │
│  │ Dst MAC  │ Src MAC  │ EtherType│  (14 bytes)       │
│  │ 6 bytes  │ 6 bytes  │  0x8915  │                   │
│  └──────────┴──────────┴──────────┘                   │
├────────────────────────────────────────────────────────┤
│                   RDMA Header                           │
│  ┌──────┬──────┬───────┬─────────┬──────────────────┐ │
│  │ Op   │Flags │Src QP │ Dst QP  │   Sequence #     │ │
│  │1 byte│1 byte│2 bytes│ 2 bytes │    4 bytes       │ │
│  └──────┴──────┴───────┴─────────┴──────────────────┘ │
│  ┌──────────────┬─────────────┬──────────────────────┐│
│  │ Local MR ID  │ Remote MR ID│   Remote Addr/Offset││
│  │   4 bytes    │  4 bytes    │      8 bytes        ││
│  └──────────────┴─────────────┴──────────────────────┘│
│  ┌──────────────┬────────────────────────────────────┐│
│  │   Length     │         Remote Key                 ││
│  │   4 bytes    │         4 bytes                    ││
│  └──────────────┴────────────────────────────────────┘│
│                    (40 bytes total)                    │
├────────────────────────────────────────────────────────┤
│                   Payload Data                          │
│  (variable length, up to MTU - headers)                │
│  For RDMA_WRITE: actual data to write                 │
│  For RDMA_READ: request (no data)                     │
│  For RDMA_READ_RESP: response data                    │
└────────────────────────────────────────────────────────┘
```

### RDMA Opcodes
```c
#define RDMA_OP_WRITE       0x01  // Write data to remote memory
#define RDMA_OP_READ        0x02  // Read data from remote memory
#define RDMA_OP_READ_RESP   0x03  // Response to RDMA_READ
#define RDMA_OP_ACK         0x04  // Acknowledgment (for reliability)
```

### Ethernet Type
```c
#define ETHTYPE_RDMA  0x8915  // Custom ethertype for RDMA
```

## Connection Establishment

### QP Connection State Machine
```
    ┌────────┐
    │ RESET  │  (Initial state)
    └───┬────┘
        │ rdma_qp_create()
        ↓
    ┌────────┐
    │  INIT  │  (Allocated, not connected)
    └───┬────┘
        │ rdma_qp_connect(remote_mac, remote_qp_num)
        ↓
    ┌────────┐
    │  RTR   │  (Ready To Receive)
    └───┬────┘
        │ Connection confirmed
        ↓
    ┌────────┐
    │  RTS   │  (Ready To Send) - Can post send operations
    └───┬────┘
        │ Error or rdma_qp_destroy()
        ↓
    ┌────────┐
    │ ERROR  │
    └────────┘
```

### Connection Setup Sequence
```
Host A                              Host B
  │                                   │
  │ 1. rdma_qp_create()              │
  │    (QP 0 created)                │
  │                                   │
  │                                   │ 2. rdma_qp_create()
  │                                   │    (QP 1 created)
  │                                   │
  │ 3. rdma_qp_connect(B_mac, 1) ────┼──> Store in QP 0
  │    State: INIT → RTR              │
  │                                   │
  │ <────────────────────────────────┼─ 4. rdma_qp_connect(A_mac, 0)
  │                                   │    State: INIT → RTR
  │                                   │
  │ 5. First RDMA_WRITE ─────────────┼──>
  │    State: RTR → RTS               │    State: RTR → RTS
  │                                   │
  │ <────────────────────────────────┼─ 6. ACK
  │    Both QPs now in RTS state     │
```

## Transmission Path (RDMA_WRITE)

### Sequence Diagram
```
User App              RDMA Core           Network Layer        E1000 NIC
   │                      │                     │                  │
   │ rdma_post_send()     │                     │                  │
   ├─────────────────────>│                     │                  │
   │                      │                     │                  │
   │                      │ 1. Validate WR      │                  │
   │                      │    (MR, QP state)   │                  │
   │                      │                     │                  │
   │                      │ 2. Check QP mode    │                  │
   │                      │    NETWORK?         │                  │
   │                      │                     │                  │
   │                      │ 3. Build RDMA pkt   │                  │
   │                      ├────────────────────>│                  │
   │                      │                     │                  │
   │                      │                     │ 4. Allocate mbuf │
   │                      │                     │    Add Eth hdr   │
   │                      │                     │    Add RDMA hdr  │
   │                      │                     │    Copy payload  │
   │                      │                     │                  │
   │                      │                     │ 5. e1000_transmit()
   │                      │                     ├─────────────────>│
   │                      │                     │                  │
   │                      │                     │                  │ 6. Send packet
   │                      │                     │                  │    on wire
   │                      │                     │                  │
   │ <────────────────────┤ 7. Return 0        │                  │
   │                      │    (async)          │                  │
```

### Pseudocode: TX Path
```c
int rdma_process_work_requests(int qp_id, struct rdma_qp *qp)
{
    while (qp->sq_head != qp->sq_tail) {
        struct rdma_work_request *wr = &qp->sq[qp->sq_head];
        
        // Check QP connection state
        if (qp->state != QP_STATE_RTS) {
            // Post error completion
            continue;
        }
        
        // Determine operation mode
        if (qp->connected) {
            // Network mode: Send RDMA packet
            rdma_transmit_packet(qp, wr);
        } else {
            // Loopback mode: Direct memory copy (existing code)
            rdma_process_local(qp, wr);
        }
        
        // Advance SQ head
        qp->sq_head = (qp->sq_head + 1) % qp->sq_size;
    }
}

void rdma_transmit_packet(struct rdma_qp *qp, struct rdma_work_request *wr)
{
    // 1. Allocate mbuf for packet
    struct mbuf *m = mbufalloc(0);
    
    // 2. Build Ethernet header
    struct eth *ethhdr = mbufputhdr(m, *ethhdr);
    memmove(ethhdr->dhost, qp->remote_mac, 6);
    memmove(ethhdr->shost, local_mac, 6);
    ethhdr->type = htons(ETHTYPE_RDMA);
    
    // 3. Build RDMA header
    struct rdma_pkt_hdr *rdmahdr = mbufputhdr(m, *rdmahdr);
    rdmahdr->opcode = wr->opcode;
    rdmahdr->flags = wr->flags;
    rdmahdr->src_qp = htons(qp->id);
    rdmahdr->dst_qp = htons(qp->remote_qp_num);
    rdmahdr->seq_num = htonl(qp->tx_seq++);
    rdmahdr->local_mr_id = htonl(wr->local_mr_id);
    rdmahdr->remote_mr_id = htonl(wr->remote_mr_id);
    rdmahdr->remote_addr = htonll(wr->remote_addr);
    rdmahdr->length = htonl(wr->length);
    rdmahdr->remote_key = htonl(wr->remote_key);
    
    // 4. Copy payload data
    struct rdma_mr *src_mr = rdma_mr_get(wr->local_mr_id);
    char *payload = mbufput(m, wr->length);
    memmove(payload, (void*)(src_mr->hw.paddr + wr->local_offset), wr->length);
    
    // 5. Transmit packet
    e1000_transmit(m);
    
    // Note: Completion will be posted when ACK is received
}
```

## Reception Path (RDMA_WRITE)

### Sequence Diagram
```
E1000 NIC        Network Layer      RDMA Core         User App
   │                  │                  │                │
   │ 1. Packet RX     │                  │                │
   ├─────────────────>│                  │                │
   │                  │                  │                │
   │                  │ 2. Parse Eth hdr │                │
   │                  │    Check type    │                │
   │                  │    = ETHTYPE_RDMA│                │
   │                  │                  │                │
   │                  │ 3. Call rdma_rx()│                │
   │                  ├─────────────────>│                │
   │                  │                  │                │
   │                  │                  │ 4. Parse RDMA hdr
   │                  │                  │    Extract QP, MR info
   │                  │                  │                │
   │                  │                  │ 5. Validate dest MR
   │                  │                  │    Check permissions
   │                  │                  │                │
   │                  │                  │ 6. Write data to memory
   │                  │                  │    memmove(dst_mr->paddr,
   │                  │                  │            payload, len)
   │                  │                  │                │
   │                  │                  │ 7. Post completion to CQ
   │                  │                  │                │
   │                  │                  │ 8. Send ACK packet
   │                  │ <────────────────┤                │
   │                  │                  │                │
   │ <────────────────┤ 9. Transmit ACK  │                │
   │                  │                  │                │
   │                  │                  │                │
   │                  │                  │ <───────────── │ rdma_poll_cq()
   │                  │                  │    Return comp │ (later)
```

### Pseudocode: RX Path
```c
void net_rx(struct mbuf *m)
{
    struct eth *ethhdr = mbufpullhdr(m, *ethhdr);
    uint16 type = ntohs(ethhdr->type);
    
    if (type == ETHTYPE_RDMA) {
        rdma_rx(m);
        return;
    }
    
    // Other protocols (ARP, IP, etc.)
    // ...
}

void rdma_rx(struct mbuf *m)
{
    // 1. Parse RDMA header
    struct rdma_pkt_hdr *hdr = mbufpullhdr(m, *hdr);
    
    uint8 opcode = hdr->opcode;
    uint16 dst_qp = ntohs(hdr->dst_qp);
    uint16 src_qp = ntohs(hdr->src_qp);
    uint32 remote_mr_id = ntohl(hdr->remote_mr_id);
    uint64 remote_addr = ntohll(hdr->remote_addr);
    uint32 length = ntohl(hdr->length);
    
    // 2. Get destination QP
    struct rdma_qp *qp = rdma_qp_get(dst_qp);
    if (!qp || !qp->valid) {
        mbuffree(m);
        return;
    }
    
    // 3. Process based on opcode
    switch (opcode) {
    case RDMA_OP_WRITE: {
        // Validate destination MR
        struct rdma_mr *dst_mr = rdma_mr_get(remote_mr_id);
        if (!dst_mr) {
            // Send NACK
            mbuffree(m);
            return;
        }
        
        // Check permissions
        if (!(dst_mr->hw.access_flags & RDMA_ACCESS_REMOTE_WRITE)) {
            mbuffree(m);
            return;
        }
        
        // Calculate destination address
        uint64 offset = (remote_addr >= dst_mr->hw.vaddr) ?
                        (remote_addr - dst_mr->hw.vaddr) : remote_addr;
        
        if (offset + length > dst_mr->hw.length) {
            mbuffree(m);
            return;
        }
        
        // Write data to memory
        char *payload = mbufpull(m, length);
        memmove((void*)(dst_mr->hw.paddr + offset), payload, length);
        
        // Post completion to CQ
        struct rdma_completion comp = {
            .wr_id = 0,  // Remote doesn't know our wr_id
            .byte_len = length,
            .status = RDMA_WC_SUCCESS,
            .opcode = RDMA_OP_WRITE,
        };
        qp->cq[qp->cq_tail] = comp;
        qp->cq_tail = (qp->cq_tail + 1) % qp->cq_size;
        
        // Send ACK back to sender
        rdma_send_ack(qp, src_qp, ntohs(hdr->seq_num));
        
        break;
    }
    
    case RDMA_OP_ACK: {
        // Sender received ACK - post completion
        uint32 seq_num = ntohl(hdr->seq_num);
        
        // Find matching WR and post completion
        // (implement sequence number tracking)
        
        break;
    }
    
    case RDMA_OP_READ: {
        // Handle RDMA_READ request
        // Read local memory and send RDMA_READ_RESP
        break;
    }
    
    case RDMA_OP_READ_RESP: {
        // Handle RDMA_READ response
        // Write data to local MR and post completion
        break;
    }
    }
    
    mbuffree(m);
}
```

## Memory Consistency and Ordering

### Write Ordering Guarantees
```
Request Order:  WR1 → WR2 → WR3
                 ↓     ↓     ↓
Packet Order:   P1  → P2  → P3  (sequence numbers)
                 ↓     ↓     ↓
Network:        P1 ────────────> Host B
                      P2 ───────> Host B
                           P3 ──> Host B
                 ↓     ↓     ↓
Execution:      E1  → E2  → E3  (sequence number check)
                 ↓     ↓     ↓
Completion:     C1  → C2  → C3
```

### Out-of-Order Handling
```c
struct rdma_qp {
    uint32 tx_seq;          // Next sequence number to send
    uint32 rx_expected_seq; // Next sequence number expected
    
    // Out-of-order queue
    struct ooo_entry {
        uint32 seq_num;
        struct mbuf *packet;
    } ooo_queue[MAX_OOO];
};
```

## Configuration and Setup

### QEMU Network Configuration
```bash
# Start two xv6 instances with networking

# Terminal 1: Host A
qemu-system-riscv64 \
    -machine virt -bios none -kernel kernel/kernel \
    -m 128M -smp 3 -nographic \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -netdev socket,id=net0,listen=:1234

# Terminal 2: Host B  
qemu-system-riscv64 \
    -machine virt -bios none -kernel kernel/kernel \
    -m 128M -smp 3 -nographic \
    -device e1000,netdev=net0,mac=52:54:00:12:34:57 \
    -netdev socket,id=net0,connect=127.0.0.1:1234
```

### Discovering Remote MAC Address
```c
// Simple approach: Manual configuration
#define HOST_A_MAC {0x52, 0x54, 0x00, 0x12, 0x34, 0x56}
#define HOST_B_MAC {0x52, 0x54, 0x00, 0x12, 0x34, 0x57}

// Advanced: ARP-like RDMA discovery protocol
// (broadcast "who has QP X?" and get MAC response)
```

## Performance Considerations

### Zero-Copy Optimization
```
Traditional:
User Buffer → Kernel Copy → mbuf → NIC
   (copy 1)      (copy 2)

Zero-Copy (future):
User Buffer → Pin → DMA → NIC
              (no copy)
```

### Batch Processing
```c
// Process multiple WRs in one network round-trip
#define MAX_BATCH_SIZE 32

void rdma_process_batch(struct rdma_qp *qp)
{
    int batch_count = 0;
    struct mbuf *batch[MAX_BATCH_SIZE];
    
    while (batch_count < MAX_BATCH_SIZE && has_pending_wr(qp)) {
        batch[batch_count++] = build_rdma_packet(qp);
    }
    
    // Send all packets at once
    for (int i = 0; i < batch_count; i++) {
        e1000_transmit(batch[i]);
    }
}
```

## Error Handling

### Network Errors
```
Error Type             Detection              Recovery
─────────────────────────────────────────────────────────
Packet Loss           Timeout + Retransmit    Resend WR
Checksum Error        Verify checksum         Drop + NACK
Invalid MR            Permission check        Post error CQ
QP Not Connected      State check             Return -ENOTCONN
Out of Memory         mbuf allocation         Flow control
NIC TX Queue Full     e1000_transmit() fail   Backpressure
```

### Timeout and Retransmission
```c
struct rdma_qp {
    uint64 timeout_ns;          // Retransmit timeout (e.g., 1ms)
    
    struct inflight_wr {
        uint32 seq_num;
        uint64 send_time;       // When packet was sent
        struct rdma_work_request wr;
    } inflight[MAX_INFLIGHT];
};

// Periodic timer checks for timeouts
void rdma_timeout_handler(void)
{
    uint64 now = get_time_ns();
    
    for each QP:
        for each inflight WR:
            if (now - wr->send_time > qp->timeout_ns):
                // Retransmit
                rdma_transmit_packet(qp, &wr->wr);
                wr->send_time = now;
}
```

## Testing Strategy

### Phase 1: Local Packet Loopback
```
1. Build RDMA packet in TX path
2. Instead of e1000_transmit(), directly call rdma_rx()
3. Verify packet parsing works
4. Verify memory write works
5. Verify completion generation works
```

### Phase 2: Two QEMU Instances
```
1. Start two xv6 with socket network
2. Ping between hosts (verify connectivity)
3. Run simple RDMA_WRITE test
4. Verify data arrives correctly
5. Test error cases
```

### Phase 3: Performance Testing
```
1. Measure latency (time from post_send to completion)
2. Measure throughput (bytes/second)
3. Test large transfers (> MTU, requires fragmentation)
4. Test concurrent QPs
```

## File Structure

### New Files to Create
```
kernel/rdma_net.c          # Network RDMA implementation
kernel/rdma_net.h          # Network RDMA headers and packet format
user/rdmanet_test.c        # Two-host RDMA test program
```

### Modified Files
```
kernel/rdma.c              # Add network mode switch
kernel/rdma.h              # Add network-related structures
kernel/net.c               # Register RDMA ethertype handler
kernel/defs.h              # Add rdma_rx() declaration
```

## Implementation Phases

### Phase 1: Packet Format (30 min)
- [ ] Define `struct rdma_pkt_hdr` in `kernel/rdma_net.h`
- [ ] Define ethertype constant
- [ ] Add endianness conversion helpers

### Phase 2: Transmission (1 hour)
- [ ] Implement `rdma_transmit_packet()`
- [ ] Modify `rdma_process_work_requests()` to check QP mode
- [ ] Implement `rdma_send_ack()`

### Phase 3: Reception (1 hour)
- [ ] Implement `rdma_rx()` in `kernel/rdma_net.c`
- [ ] Register handler in `net_rx()`
- [ ] Handle RDMA_WRITE packets
- [ ] Handle ACK packets

### Phase 4: Connection (30 min)
- [ ] Implement `rdma_qp_connect()`
- [ ] Add QP state management
- [ ] Store remote MAC and QP number

### Phase 5: Testing (1 hour)
- [ ] Create test program for two hosts
- [ ] Test basic RDMA_WRITE
- [ ] Test error cases
- [ ] Document setup instructions

**Total Estimate: 4 hours**

## Benefits of This Design

1. **Minimal Changes**: Reuses existing RDMA semantics and E1000 driver
2. **Clean Separation**: Network layer is separate from core RDMA
3. **Backward Compatible**: Loopback mode still works
4. **Extensible**: Easy to add RDMA_READ, reliability, etc.
5. **Educational**: Shows real RDMA protocol concepts

## Next Steps

Would you like me to:
1. **Start implementing this design** (begin with Phase 1)?
2. **Create a detailed implementation guide** you can follow?
3. **Explain any specific part** in more detail?
