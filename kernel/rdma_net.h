// kernel/rdma_net.h - RDMA Network Protocol Definitions

#ifndef _RDMA_NET_H_
#define _RDMA_NET_H_

#include "types.h"
#include "net.h"

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
void rdma_net_rx(struct mbuf *m, uint8 *src_mac);
int  rdma_net_tx_write(struct rdma_qp *qp, struct rdma_work_request *wr);
void rdma_net_tx_ack(struct rdma_qp *qp, uint16 remote_qp, uint32 seq_num, uint8 *dst_mac);

#endif // _RDMA_NET_H_
