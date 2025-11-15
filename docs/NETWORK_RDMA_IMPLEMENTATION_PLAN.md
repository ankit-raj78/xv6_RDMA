# Network RDMA Implementation Plan

## Overview
This document provides a step-by-step implementation plan for adding network RDMA support to xv6. Follow these phases sequentially.

---

## Phase 1: Packet Format and Data Structures (30 minutes)

### Goal
Define the RDMA network packet format and supporting data structures.

### Tasks

#### 1.1 Create `kernel/rdma_net.h`
```c
// kernel/rdma_net.h - RDMA Network Protocol Definitions

#ifndef _RDMA_NET_H_
#define _RDMA_NET_H_

#include "types.h"

// Ethernet type for RDMA protocol
#define ETHTYPE_RDMA  0x8915

// RDMA network opcodes (same as RDMA_OP_* but for clarity)
#define RDMA_NET_OP_WRITE       0x01
#define RDMA_NET_OP_READ        0x02
#define RDMA_NET_OP_READ_RESP   0x03
#define RDMA_NET_OP_ACK         0x04

// RDMA packet flags
#define RDMA_PKT_FLAG_SIGNALED  0x01

// RDMA packet header (40 bytes)
struct rdma_pkt_hdr {
    uint8  opcode;           // RDMA_NET_OP_*
    uint8  flags;            // Packet flags
    uint16 src_qp;           // Source QP number
    uint16 dst_qp;           // Destination QP number
    uint16 reserved1;        // Padding for alignment
    uint32 seq_num;          // Sequence number
    uint32 local_mr_id;      // Source MR ID (sender's perspective)
    uint32 remote_mr_id;     // Destination MR ID
    uint64 remote_addr;      // Remote address or offset
    uint32 length;           // Data length in bytes
    uint32 remote_key;       // Remote key for validation
} __attribute__((packed));

// Endianness helpers for 64-bit values
static inline uint64 htonll(uint64 val)
{
    return (((uint64)htonl(val & 0xFFFFFFFF)) << 32) | htonl(val >> 32);
}

static inline uint64 ntohll(uint64 val)
{
    return (((uint64)ntohl(val & 0xFFFFFFFF)) << 32) | ntohl(val >> 32);
}

// Function declarations
void rdma_net_init(void);
void rdma_net_rx(struct mbuf *m, uint8 src_mac[6]);
int  rdma_net_tx_write(struct rdma_qp *qp, struct rdma_work_request *wr);
void rdma_net_tx_ack(struct rdma_qp *qp, uint16 remote_qp, uint32 seq_num, uint8 dst_mac[6]);

#endif // _RDMA_NET_H_
```

#### 1.2 Update `kernel/rdma.h` - Add Network Fields
Add to `struct rdma_qp`:
```c
struct rdma_qp {
    // ... existing fields ...
    
    // Network RDMA state
    int network_mode;                    // 0 = loopback, 1 = network
    uint32 tx_seq_num;                   // Next TX sequence number
    uint32 rx_expected_seq;              // Expected RX sequence number
    
    // Pending ACKs (for matching completions)
    struct {
        uint32 seq_num;
        uint64 wr_id;
        int valid;
    } pending_acks[64];
};
```

#### 1.3 Update `kernel/defs.h`
Add declarations:
```c
// rdma_net.c
void            rdma_net_init(void);
void            rdma_net_rx(struct mbuf*, uint8*);
int             rdma_net_tx_write(struct rdma_qp*, struct rdma_work_request*);
void            rdma_net_tx_ack(struct rdma_qp*, uint16, uint32, uint8*);
```

#### 1.4 Verification
- [ ] Files compile without errors
- [ ] No syntax errors in struct definitions
- [ ] Endianness helpers work correctly

---

## Phase 2: Queue Pair Connection (30 minutes)

### Goal
Implement QP connection functionality to enable network mode.

### Tasks

#### 2.1 Add QP State Enum to `kernel/rdma.h`
```c
// QP State Machine - update existing enum
enum rdma_qp_state {
    QP_STATE_RESET = 0,          // Initial state
    QP_STATE_INIT,               // Allocated, not configured
    QP_STATE_RTR,                // Ready To Receive (connected)
    QP_STATE_RTS,                // Ready To Send (can transmit)
    QP_STATE_ERROR               // Error state
};
```

#### 2.2 Implement `rdma_qp_connect()` in `kernel/rdma.c`
```c
/* Connect queue pair to remote peer
 * 
 * Args:
 *   qp_id - Local QP ID
 *   mac - Remote MAC address (6 bytes)
 *   remote_qp - Remote QP number
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
    
    // Transition to RTR (Ready To Receive)
    qp->state = QP_STATE_RTR;
    
    release(&qp_lock);
    
    printf("rdma_qp_connect: QP %d connected to remote QP %d (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
           qp_id, remote_qp, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
    return 0;
}
```

#### 2.3 Update `rdma_qp_create()` to Initialize Network Fields
In `kernel/rdma.c`, after allocating QP:
```c
// Initialize network fields
qp->network_mode = 0;  // Start in loopback mode
qp->tx_seq_num = 0;
qp->rx_expected_seq = 0;
for (int i = 0; i < 64; i++) {
    qp->pending_acks[i].valid = 0;
}
```

#### 2.4 Add System Call for `rdma_qp_connect()`

In `kernel/syscall.h`:
```c
#define SYS_rdma_connect   28
```

In `kernel/syscall.c`:
```c
extern uint64 sys_rdma_connect(void);

[SYS_rdma_connect]   sys_rdma_connect,
```

In `kernel/sysrdma.c`:
```c
// Connect queue pair to remote peer
// args: qp_id (int), mac (uint8[6]), remote_qp (uint32)
uint64
sys_rdma_connect(void)
{
    int qp_id;
    uint64 mac_ptr;
    int remote_qp;
    uint8 mac[6];
    struct proc *p = myproc();
    
    argint(0, &qp_id);
    argaddr(1, &mac_ptr);
    argint(2, &remote_qp);
    
    // Validate parameters
    if (qp_id < 0 || qp_id >= MAX_QPS || remote_qp < 0) {
        return -1;
    }
    
    // Copy MAC address from user space
    if (copyin(p->pagetable, (char*)mac, mac_ptr, 6) < 0) {
        return -1;
    }
    
    return rdma_qp_connect(qp_id, mac, (uint32)remote_qp);
}
```

In `user/usys.pl`:
```perl
entry("rdma_connect");
```

In `user/rdma.h`:
```c
// Connect QP to remote peer
int rdma_connect(int qp_id, unsigned char mac[6], unsigned int remote_qp);
```

#### 2.5 Verification
- [ ] `rdma_qp_connect()` compiles
- [ ] System call infrastructure added
- [ ] Can call from user space
- [ ] QP state transitions to RTR

---

## Phase 3: Transmission Path (1 hour)

### Goal
Implement RDMA packet transmission over the network.

### Tasks

#### 3.1 Create `kernel/rdma_net.c` - Initialization
```c
// kernel/rdma_net.c - RDMA Network Protocol Implementation

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "net.h"
#include "rdma.h"
#include "rdma_net.h"

// Local MAC address (should match QEMU configuration)
static uint8 local_mac[6];

void
rdma_net_init(void)
{
    // Get local MAC address from E1000
    e1000_get_mac(local_mac);
    
    printf("rdma_net: initialized (MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
           local_mac[0], local_mac[1], local_mac[2], 
           local_mac[3], local_mac[4], local_mac[5]);
}
```

#### 3.2 Implement TX Function in `kernel/rdma_net.c`
```c
/* Transmit RDMA_WRITE packet
 * 
 * Builds and sends an RDMA packet over the network.
 * Called from rdma_process_work_requests() in network mode.
 */
int
rdma_net_tx_write(struct rdma_qp *qp, struct rdma_work_request *wr)
{
    // Get source MR
    struct rdma_mr *src_mr = rdma_mr_get(wr->local_mr_id);
    if (!src_mr) {
        return -1;
    }
    
    // Allocate mbuf for packet
    struct mbuf *m = mbufalloc(0);
    if (!m) {
        return -1;
    }
    
    // Build Ethernet header
    struct eth *ethhdr = mbufputhdr(m, *ethhdr);
    memmove(ethhdr->dhost, qp->remote_mac, 6);
    memmove(ethhdr->shost, local_mac, 6);
    ethhdr->type = htons(ETHTYPE_RDMA);
    
    // Build RDMA header
    struct rdma_pkt_hdr *rdmahdr = mbufputhdr(m, *rdmahdr);
    rdmahdr->opcode = RDMA_NET_OP_WRITE;
    rdmahdr->flags = wr->flags & RDMA_WR_SIGNALED ? RDMA_PKT_FLAG_SIGNALED : 0;
    rdmahdr->src_qp = htons(qp->id);
    rdmahdr->dst_qp = htons(qp->remote_qp_num);
    rdmahdr->seq_num = htonl(qp->tx_seq_num);
    rdmahdr->local_mr_id = htonl(wr->local_mr_id);
    rdmahdr->remote_mr_id = htonl(wr->remote_mr_id);
    rdmahdr->remote_addr = htonll(wr->remote_addr);
    rdmahdr->length = htonl(wr->length);
    rdmahdr->remote_key = htonl(wr->remote_key);
    
    // Copy payload data from source MR
    char *payload = mbufput(m, wr->length);
    if (!payload) {
        mbuffree(m);
        return -1;
    }
    memmove(payload, (void*)(wr->local_offset), wr->length);
    
    // Track this WR for ACK matching (if signaled)
    if (wr->flags & RDMA_WR_SIGNALED) {
        for (int i = 0; i < 64; i++) {
            if (!qp->pending_acks[i].valid) {
                qp->pending_acks[i].seq_num = qp->tx_seq_num;
                qp->pending_acks[i].wr_id = wr->wr_id;
                qp->pending_acks[i].valid = 1;
                break;
            }
        }
    }
    
    // Increment sequence number
    qp->tx_seq_num++;
    
    // Transition QP to RTS (Ready To Send) on first transmission
    if (qp->state == QP_STATE_RTR) {
        qp->state = QP_STATE_RTS;
    }
    
    // Transmit packet
    e1000_transmit(m);
    
    return 0;
}
```

#### 3.3 Implement ACK Transmission
```c
/* Send ACK packet to remote peer */
void
rdma_net_tx_ack(struct rdma_qp *qp, uint16 remote_qp, uint32 seq_num, uint8 dst_mac[6])
{
    // Allocate mbuf
    struct mbuf *m = mbufalloc(0);
    if (!m) return;
    
    // Build Ethernet header
    struct eth *ethhdr = mbufputhdr(m, *ethhdr);
    memmove(ethhdr->dhost, dst_mac, 6);
    memmove(ethhdr->shost, local_mac, 6);
    ethhdr->type = htons(ETHTYPE_RDMA);
    
    // Build RDMA ACK header (no payload)
    struct rdma_pkt_hdr *rdmahdr = mbufputhdr(m, *rdmahdr);
    rdmahdr->opcode = RDMA_NET_OP_ACK;
    rdmahdr->flags = 0;
    rdmahdr->src_qp = htons(qp->id);
    rdmahdr->dst_qp = htons(remote_qp);
    rdmahdr->seq_num = htonl(seq_num);
    rdmahdr->local_mr_id = 0;
    rdmahdr->remote_mr_id = 0;
    rdmahdr->remote_addr = 0;
    rdmahdr->length = 0;
    rdmahdr->remote_key = 0;
    
    // Transmit ACK
    e1000_transmit(m);
}
```

#### 3.4 Modify `rdma_process_work_requests()` in `kernel/rdma.c`
Add network mode check:
```c
static void
rdma_process_work_requests(int qp_id, struct rdma_qp *qp)
{
    while (qp->sq_head != qp->sq_tail) {
        struct rdma_work_request *wr = &qp->sq[qp->sq_head];
        
        // Validate source MR
        struct rdma_mr *src_mr = rdma_mr_get(wr->local_mr_id);
        if (!src_mr) {
            // Post error completion
            // ... existing error handling code ...
            goto next_wr;
        }
        
        // Check operation mode
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
            // ... existing RDMA_OP_WRITE code ...
        }
        
        // Decrement MR refcount
        acquire(&mr_lock);
        src_mr->refcount--;
        release(&mr_lock);
        
next_wr:
        qp->sq_head = (qp->sq_head + 1) % qp->sq_size;
        qp->outstanding_ops--;
    }
}
```

#### 3.5 Add MAC Address Getter to `kernel/e1000.c`
```c
void
e1000_get_mac(uint8 mac[6])
{
    uint32 ral = regs[E1000_RAL];
    uint32 rah = regs[E1000_RAH];
    
    mac[0] = (ral >> 0) & 0xFF;
    mac[1] = (ral >> 8) & 0xFF;
    mac[2] = (ral >> 16) & 0xFF;
    mac[3] = (ral >> 24) & 0xFF;
    mac[4] = (rah >> 0) & 0xFF;
    mac[5] = (rah >> 8) & 0xFF;
}
```

In `kernel/defs.h`:
```c
void            e1000_get_mac(uint8*);
```

#### 3.6 Verification
- [ ] Packets are built correctly
- [ ] E1000 transmit is called
- [ ] No memory leaks (mbuf freed on error)
- [ ] Sequence numbers increment

---

## Phase 4: Reception Path (1 hour)

### Goal
Receive and process RDMA packets from the network.

### Tasks

#### 4.1 Implement RX Handler in `kernel/rdma_net.c`
```c
/* Receive and process RDMA packet
 * 
 * Called from net_rx() when ETHTYPE_RDMA packet arrives.
 */
void
rdma_net_rx(struct mbuf *m, uint8 src_mac[6])
{
    // Parse RDMA header
    struct rdma_pkt_hdr *hdr = mbufpullhdr(m, *hdr);
    if (!hdr) {
        mbuffree(m);
        return;
    }
    
    // Convert from network byte order
    uint8 opcode = hdr->opcode;
    uint16 dst_qp_num = ntohs(hdr->dst_qp);
    uint16 src_qp_num = ntohs(hdr->src_qp);
    uint32 seq_num = ntohl(hdr->seq_num);
    uint32 remote_mr_id = ntohl(hdr->remote_mr_id);
    uint64 remote_addr = ntohll(hdr->remote_addr);
    uint32 length = ntohl(hdr->length);
    uint8 flags = hdr->flags;
    
    // Get destination QP
    acquire(&qp_lock);
    
    if (dst_qp_num >= MAX_QPS || !qp_table[dst_qp_num].valid) {
        release(&qp_lock);
        mbuffree(m);
        return;
    }
    
    struct rdma_qp *qp = &qp_table[dst_qp_num];
    
    // Process based on opcode
    switch (opcode) {
    case RDMA_NET_OP_WRITE: {
        // Transition to RTS if first packet received
        if (qp->state == QP_STATE_RTR) {
            qp->state = QP_STATE_RTS;
        }
        
        // Validate destination MR
        struct rdma_mr *dst_mr = rdma_mr_get(remote_mr_id);
        if (!dst_mr) {
            release(&qp_lock);
            mbuffree(m);
            return;
        }
        
        // Check permissions
        if (!(dst_mr->hw.access_flags & RDMA_ACCESS_REMOTE_WRITE)) {
            release(&qp_lock);
            mbuffree(m);
            return;
        }
        
        // Calculate destination offset
        uint64 offset;
        if (remote_addr >= dst_mr->hw.vaddr && 
            remote_addr < dst_mr->hw.vaddr + dst_mr->hw.length) {
            offset = remote_addr - dst_mr->hw.vaddr;
        } else if (remote_addr < dst_mr->hw.length) {
            offset = remote_addr;
        } else {
            release(&qp_lock);
            mbuffree(m);
            return;
        }
        
        // Check bounds
        if (offset + length > dst_mr->hw.length) {
            release(&qp_lock);
            mbuffree(m);
            return;
        }
        
        // Pull payload from mbuf
        char *payload = mbufpull(m, length);
        if (!payload) {
            release(&qp_lock);
            mbuffree(m);
            return;
        }
        
        // Write data to destination memory
        memmove((void*)(dst_mr->hw.paddr + offset), payload, length);
        
        // Post completion to CQ (receiver side)
        struct rdma_completion comp = {
            .wr_id = 0,  // Receiver doesn't know sender's wr_id
            .byte_len = length,
            .status = RDMA_WC_SUCCESS,
            .opcode = RDMA_OP_WRITE,
        };
        qp->cq[qp->cq_tail] = comp;
        qp->cq_tail = (qp->cq_tail + 1) % qp->cq_size;
        qp->stats_completions++;
        
        // Send ACK back to sender
        rdma_net_tx_ack(qp, src_qp_num, seq_num, src_mac);
        
        break;
    }
    
    case RDMA_NET_OP_ACK: {
        // Find matching pending WR
        for (int i = 0; i < 64; i++) {
            if (qp->pending_acks[i].valid && 
                qp->pending_acks[i].seq_num == seq_num) {
                
                // Post completion for sender
                struct rdma_completion comp = {
                    .wr_id = qp->pending_acks[i].wr_id,
                    .byte_len = length,  // Will be 0 for ACK
                    .status = RDMA_WC_SUCCESS,
                    .opcode = RDMA_OP_WRITE,
                };
                qp->cq[qp->cq_tail] = comp;
                qp->cq_tail = (qp->cq_tail + 1) % qp->cq_size;
                qp->stats_completions++;
                
                // Mark this ACK as processed
                qp->pending_acks[i].valid = 0;
                break;
            }
        }
        break;
    }
    
    default:
        // Unknown opcode
        break;
    }
    
    release(&qp_lock);
    mbuffree(m);
}
```

#### 4.2 Register RDMA Handler in `kernel/net.c`
Modify `net_rx()` to handle RDMA packets:
```c
static void
net_rx(void)
{
    struct mbuf *m = e1000_recv();
    if (!m)
        return;
    
    // Parse Ethernet header
    struct eth *ethhdr = mbufpullhdr(m, *ethhdr);
    if (!ethhdr) {
        mbuffree(m);
        return;
    }
    
    uint16 type = ntohs(ethhdr->type);
    
    // Save source MAC before processing
    uint8 src_mac[6];
    memmove(src_mac, ethhdr->shost, 6);
    
    switch (type) {
    case ETHTYPE_IP:
        net_rx_ip(m);
        break;
    case ETHTYPE_ARP:
        net_rx_arp(m);
        break;
    case ETHTYPE_RDMA:
        rdma_net_rx(m, src_mac);
        break;
    default:
        mbuffree(m);
        break;
    }
}
```

#### 4.3 Initialize RDMA Network Layer
In `kernel/main.c`, add initialization:
```c
void
main()
{
    // ... existing initialization ...
    
    e1000init(mbi);
    rdma_init();
    rdma_net_init();  // Add this line
    
    // ... rest of main ...
}
```

#### 4.4 Verification
- [ ] Packets are received and parsed
- [ ] Data is written to correct memory location
- [ ] Completions are posted on both sides
- [ ] ACKs are sent and received

---

## Phase 5: Build System Updates (15 minutes)

### Goal
Update Makefile to include new files.

### Tasks

#### 5.1 Update `Makefile`
Add to OBJS:
```makefile
OBJS = \
  # ... existing objects ...
  $K/rdma.o \
  $K/rdma_net.o
```

#### 5.2 Verification
- [ ] `make clean && make` succeeds
- [ ] `kernel/rdma_net.o` is created
- [ ] No undefined symbols

---

## Phase 6: User-Space Test Program (45 minutes)

### Goal
Create a test program to verify two-host RDMA communication.

### Tasks

#### 6.1 Create `user/rdmanet_test.c`
```c
// user/rdmanet_test.c - Two-host RDMA network test

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/rdma.h"

#define TEST_SIZE 256
#define PGSIZE 4096

// Host A MAC: 52:54:00:12:34:56 (QEMU default)
#define HOST_A_MAC {0x52, 0x54, 0x00, 0x12, 0x34, 0x56}
// Host B MAC: 52:54:00:12:34:57 (configured in QEMU)
#define HOST_B_MAC {0x52, 0x54, 0x00, 0x12, 0x34, 0x57}

void* alloc_page_aligned(int size)
{
    char *p = sbrk(size + PGSIZE);
    if (p == (char*)-1) return 0;
    uint64 addr = (uint64)p;
    uint64 aligned = (addr + PGSIZE - 1) & ~(PGSIZE - 1);
    return (void*)aligned;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: rdmanet_test <host_a|host_b>\n");
        exit(1);
    }
    
    int is_host_a = (strcmp(argv[1], "host_a") == 0);
    
    printf("=== RDMA Network Test ===\n");
    printf("Running as: %s\n\n", is_host_a ? "Host A (sender)" : "Host B (receiver)");
    
    // Allocate buffers
    char *buf = alloc_page_aligned(TEST_SIZE);
    if (!buf) {
        printf("Failed to allocate buffer\n");
        exit(1);
    }
    
    if (is_host_a) {
        // HOST A: Sender
        printf("Host A: Preparing to send data...\n");
        
        // Fill buffer with test pattern
        for (int i = 0; i < TEST_SIZE; i++) {
            buf[i] = (char)(i % 256);
        }
        
        // Register memory region
        int mr_id = rdma_reg_mr(buf, TEST_SIZE, 
                               RDMA_ACCESS_LOCAL_READ | RDMA_ACCESS_REMOTE_READ);
        if (mr_id < 0) {
            printf("Failed to register MR\n");
            exit(1);
        }
        printf("Host A: Registered MR %d\n", mr_id);
        
        // Create queue pair
        int qp_id = rdma_create_qp(64, 64);
        if (qp_id < 0) {
            printf("Failed to create QP\n");
            exit(1);
        }
        printf("Host A: Created QP %d\n", qp_id);
        
        // Connect to Host B
        unsigned char host_b_mac[] = HOST_B_MAC;
        if (rdma_connect(qp_id, host_b_mac, 0) < 0) {
            printf("Failed to connect QP\n");
            exit(1);
        }
        printf("Host A: Connected to Host B (QP 0)\n");
        
        // Sleep to let Host B set up
        sleep(2);
        
        // Build and post RDMA_WRITE
        struct rdma_work_request wr;
        rdma_build_write_wr(&wr,
                           1,              // wr_id
                           mr_id,          // local MR
                           0,              // local offset
                           1,              // remote MR (Host B's MR ID)
                           0,              // remote offset
                           1,              // remote key
                           TEST_SIZE);     // length
        
        printf("Host A: Posting RDMA_WRITE (256 bytes)...\n");
        if (rdma_post_send(qp_id, &wr) < 0) {
            printf("Failed to post send\n");
            exit(1);
        }
        
        // Poll for completion
        printf("Host A: Waiting for completion...\n");
        struct rdma_completion comp;
        int num_comps;
        for (int retry = 0; retry < 10; retry++) {
            num_comps = rdma_poll_cq(qp_id, &comp, 1);
            if (num_comps > 0) break;
            sleep(1);
        }
        
        if (num_comps <= 0) {
            printf("Host A: No completion received\n");
            exit(1);
        }
        
        printf("Host A: Completion received - status=%s\n",
               rdma_comp_status_str(comp.status));
        
        if (comp.status == RDMA_WC_SUCCESS) {
            printf("Host A: *** RDMA_WRITE SUCCESSFUL ***\n");
        }
        
        // Cleanup
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(mr_id);
        
    } else {
        // HOST B: Receiver
        printf("Host B: Preparing to receive data...\n");
        
        // Clear buffer
        for (int i = 0; i < TEST_SIZE; i++) {
            buf[i] = 0;
        }
        
        // Register memory region (must be MR ID 1 to match sender)
        int mr_id = rdma_reg_mr(buf, TEST_SIZE,
                               RDMA_ACCESS_LOCAL_WRITE | RDMA_ACCESS_REMOTE_WRITE);
        if (mr_id < 0) {
            printf("Failed to register MR\n");
            exit(1);
        }
        printf("Host B: Registered MR %d\n", mr_id);
        
        // Create queue pair (must be QP ID 0 to match sender)
        int qp_id = rdma_create_qp(64, 64);
        if (qp_id < 0) {
            printf("Failed to create QP\n");
            exit(1);
        }
        printf("Host B: Created QP %d\n", qp_id);
        
        // Connect to Host A
        unsigned char host_a_mac[] = HOST_A_MAC;
        if (rdma_connect(qp_id, host_a_mac, 0) < 0) {
            printf("Failed to connect QP\n");
            exit(1);
        }
        printf("Host B: Connected to Host A (QP 0)\n");
        printf("Host B: Waiting for RDMA_WRITE...\n");
        
        // Poll for completion (receive side)
        struct rdma_completion comp;
        int num_comps;
        for (int retry = 0; retry < 20; retry++) {
            num_comps = rdma_poll_cq(qp_id, &comp, 1);
            if (num_comps > 0) break;
            sleep(1);
        }
        
        if (num_comps <= 0) {
            printf("Host B: Timeout waiting for data\n");
            exit(1);
        }
        
        printf("Host B: Data received! Verifying...\n");
        
        // Verify data
        int errors = 0;
        for (int i = 0; i < TEST_SIZE; i++) {
            if ((unsigned char)buf[i] != (unsigned char)(i % 256)) {
                errors++;
            }
        }
        
        if (errors == 0) {
            printf("Host B: *** DATA VERIFICATION PASSED ***\n");
            printf("Host B: All %d bytes match!\n", TEST_SIZE);
        } else {
            printf("Host B: DATA VERIFICATION FAILED (%d errors)\n", errors);
        }
        
        // Cleanup
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(mr_id);
    }
    
    printf("\nTest complete!\n");
    exit(0);
}
```

#### 6.2 Update Makefile for Test Program
Add to UPROGS:
```makefile
$U/_rdmanet_test\
```

#### 6.3 Verification
- [ ] Program compiles
- [ ] Can run as both host_a and host_b
- [ ] Connects to remote peer

---

## Phase 7: Testing and Validation (1 hour)

### Goal
Test two-host RDMA communication in QEMU.

### Tasks

#### 7.1 Setup QEMU Network Configuration

Create `scripts/run_host_a.sh`:
```bash
#!/bin/bash
qemu-system-riscv64 \
    -machine virt -bios none -kernel kernel/kernel \
    -m 128M -smp 3 -nographic \
    -global virtio-mmio.force-legacy=false \
    -drive file=fs.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -netdev socket,id=net0,listen=127.0.0.1:1234
```

Create `scripts/run_host_b.sh`:
```bash
#!/bin/bash
sleep 2  # Wait for Host A to start listening
qemu-system-riscv64 \
    -machine virt -bios none -kernel kernel/kernel \
    -m 128M -smp 3 -nographic \
    -global virtio-mmio.force-legacy=false \
    -drive file=fs.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:57 \
    -netdev socket,id=net0,connect=127.0.0.1:1234
```

Make executable:
```bash
chmod +x scripts/run_host_a.sh scripts/run_host_b.sh
```

#### 7.2 Run Test Sequence
1. Terminal 1: `./scripts/run_host_a.sh`
2. Terminal 2: `./scripts/run_host_b.sh`
3. In Host B: `rdmanet_test host_b`
4. In Host A: `rdmanet_test host_a`
5. Observe output

#### 7.3 Expected Output

**Host B:**
```
=== RDMA Network Test ===
Running as: Host B (receiver)

Host B: Preparing to receive data...
Host B: Registered MR 1
Host B: Created QP 0
Host B: Connected to Host A (QP 0)
Host B: Waiting for RDMA_WRITE...
Host B: Data received! Verifying...
Host B: *** DATA VERIFICATION PASSED ***
Host B: All 256 bytes match!

Test complete!
```

**Host A:**
```
=== RDMA Network Test ===
Running as: Host A (sender)

Host A: Preparing to send data...
Host A: Registered MR 1
Host A: Created QP 0
Host A: Connected to Host B (QP 0)
Host A: Posting RDMA_WRITE (256 bytes)...
Host A: Waiting for completion...
Host A: Completion received - status=SUCCESS
Host A: *** RDMA_WRITE SUCCESSFUL ***

Test complete!
```

#### 7.4 Verification Checklist
- [ ] Both hosts boot successfully
- [ ] Network connection established (check with ping if needed)
- [ ] QPs connect to each other
- [ ] Packet is transmitted
- [ ] Data is received correctly
- [ ] ACK is sent and received
- [ ] Completions posted on both sides
- [ ] Data verification passes

---

## Phase 8: Documentation and Cleanup (30 minutes)

### Goal
Document the implementation and create usage guide.

### Tasks

#### 8.1 Update `docs/USER_SPACE_RDMA.md`
Add section on network RDMA:
```markdown
## Network RDMA Support

### Overview
xv6 RDMA now supports communication between two xv6 instances over Ethernet.

### Connection Setup
1. Both hosts create QPs
2. Call `rdma_connect(qp_id, remote_mac, remote_qp_num)` on each side
3. QPs transition to RTS state
4. Can now post RDMA operations

### Example Usage
See `user/rdmanet_test.c` for complete example.

### QEMU Configuration
Use socket networking to connect two instances:
- Host A: `-netdev socket,id=net0,listen=:1234`
- Host B: `-netdev socket,id=net0,connect=127.0.0.1:1234`

### Limitations
- Only RDMA_WRITE supported currently
- No reliability (packet loss not handled)
- No fragmentation (limited to MTU)
- Manual MAC address configuration
```

#### 8.2 Create README for Testing
Create `docs/NETWORK_RDMA_TESTING.md` with setup instructions.

#### 8.3 Verification
- [ ] Documentation complete
- [ ] Setup instructions clear
- [ ] Example code included

---

## Success Criteria

### Functional Requirements
- [ ] Two xv6 instances can communicate over RDMA
- [ ] RDMA_WRITE transfers data correctly
- [ ] Completions posted on both sender and receiver
- [ ] Data integrity verified

### Performance Requirements
- [ ] Latency < 10ms for 256-byte transfer (loopback network)
- [ ] No packet drops under normal conditions
- [ ] No memory leaks

### Quality Requirements
- [ ] Code compiles without warnings
- [ ] No race conditions in packet handling
- [ ] Proper error handling for all failure cases
- [ ] Documentation complete and accurate

---

## Troubleshooting Guide

### Problem: QPs don't connect
**Check:**
- Both hosts have network initialized
- MAC addresses are correct
- QP IDs match between connect calls

### Problem: No packets transmitted
**Check:**
- QP is in RTS state
- `e1000_transmit()` is being called
- mbuf allocation succeeding

### Problem: Packets not received
**Check:**
- RDMA ethertype registered in `net_rx()`
- `rdma_net_rx()` is being called
- Destination QP exists and is valid

### Problem: Data corruption
**Check:**
- MR boundaries not exceeded
- Endianness conversion correct
- Memory alignment proper

### Problem: No completions
**Check:**
- ACKs are being sent
- ACKs are being received
- pending_acks tracking correct
- wr_id matches

---

## Timeline

| Phase | Duration | Cumulative |
|-------|----------|------------|
| 1. Packet Format | 30 min | 0:30 |
| 2. Connection | 30 min | 1:00 |
| 3. TX Path | 1 hour | 2:00 |
| 4. RX Path | 1 hour | 3:00 |
| 5. Build System | 15 min | 3:15 |
| 6. Test Program | 45 min | 4:00 |
| 7. Testing | 1 hour | 5:00 |
| 8. Documentation | 30 min | 5:30 |

**Total: 5.5 hours**

---

## Next Steps After Implementation

1. **Add RDMA_READ support**
   - Implement request/response protocol
   - Handle larger data transfers

2. **Add reliability**
   - Implement timeout and retransmission
   - Add sequence number checking
   - Handle out-of-order packets

3. **Optimize performance**
   - Batch multiple operations
   - Reduce packet overhead
   - Implement zero-copy paths

4. **Add more tests**
   - Concurrent QPs
   - Large transfers (fragmentation)
   - Error injection
   - Performance benchmarks

---

## Getting Help

If you encounter issues during implementation:
1. Check the troubleshooting guide above
2. Review `/docs/NETWORK_RDMA_DESIGN.md` for architecture details
3. Look at existing network code in `kernel/net.c` and `kernel/e1000.c`
4. Use `printf()` debugging liberally
5. Test each phase independently before moving to next

Good luck with the implementation! 🚀
