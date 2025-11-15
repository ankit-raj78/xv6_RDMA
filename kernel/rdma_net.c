// kernel/rdma_net.c - RDMA Network Protocol Implementation

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "rdma.h"
#include "rdma_net.h"

// Local MAC address (should match QEMU configuration)
static uint8 local_mac[6];

void
rdma_net_init(void)
{
    // Get local MAC address from E1000
    e1000_get_mac(local_mac);
    
    printf("rdma_net: initialized (MAC: %x:%x:%x:%x:%x:%x)\n",
           local_mac[0], local_mac[1], local_mac[2], 
           local_mac[3], local_mac[4], local_mac[5]);
}

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
    printf("rdma_net_tx: sending WRITE packet (seq=%d, len=%d)\n", qp->tx_seq_num - 1, wr->length);
    e1000_transmit(m);
    
    return 0;
}

/* Send ACK packet to remote peer */
void
rdma_net_tx_ack(struct rdma_qp *qp, uint16 remote_qp, uint32 seq_num, uint8 *dst_mac)
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

/* Receive and process RDMA packet
 * 
 * Called from net_rx() when ETHTYPE_RDMA packet arrives.
 */
void
rdma_net_rx(struct mbuf *m, uint8 *src_mac)
{
    printf("rdma_net_rx: received packet\n");
    
    // Parse RDMA header
    struct rdma_pkt_hdr *hdr = mbufpullhdr(m, *hdr);
    if (!hdr) {
        printf("rdma_net_rx: failed to parse header\n");
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
    
    printf("rdma_net_rx: opcode=%d dst_qp=%d seq=%d len=%d\n", opcode, dst_qp_num, seq_num, length);
    
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
