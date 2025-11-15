# Network RDMA Testing Guide

## Overview
This guide explains how to test two-host RDMA communication in xv6.

## Quick Start

### Terminal 1: Start Host A (Listener - starts first)
```bash
cd /Users/ankitraj2/510/xv6_RDMA
./scripts/run_host_a.sh
```

Wait for xv6 to boot. **DO NOT run rdmanet_test yet!**

### Terminal 2: Start Host B (Connector - connects to Host A)  
```bash
cd /Users/ankitraj2/510/xv6_RDMA
./scripts/run_host_b.sh
```

Wait for Host B to boot.

### Run Tests in Order

**IMPORTANT: Start the receiver (Host B) before the sender (Host A)!**

1. **In Terminal 2 (Host B):** Run receiver first:
   ```
   rdmanet_test host_b
   ```
   You should see: "Host B: Ready! Waiting for RDMA_WRITE from Host A..."

2. **In Terminal 1 (Host A):** Then run sender:
   ```
   rdmanet_test host_a
   ```
   Host A will wait 3 seconds, then send data.

**Note:** The socket connection happens when both QEMU instances are running, but the RDMA test requires Host B's receiver to be waiting before Host A sends data.

## What to Expect

### Host B Output (Receiver):
```
=== RDMA Network Test ===
Running as: Host B (receiver)

Host B: Preparing to receive data...
Host B: Cleared buffer (all zeros)
Host B: Registered MR 1 (addr=0x5000, size=256)
Host B: Created QP 0
Host B: Connected to Host A (QP 0, MAC: 52:54:00:12:34:56)
Host B: Ready! Waiting for RDMA_WRITE from Host A...
Host B: Data received! Completion posted.
Host B:   byte_len=256, status=SUCCESS
Host B: Verifying data...

*** Host B: DATA VERIFICATION PASSED! ***
*** All 256 bytes match expected pattern ***
*** Network RDMA working correctly! ***

Test complete!
```

### Host A Output (Sender):
```
=== RDMA Network Test ===
Running as: Host A (sender)

Host A: Preparing to send data...
Host A: Filled buffer with test pattern
Host A: Registered MR 1 (addr=0x5000, size=256)
Host A: Created QP 0
Host A: Connected to Host B (QP 0, MAC: 52:54:00:12:34:57)
Host A: Waiting for Host B to be ready...
Host A: Posting RDMA_WRITE (256 bytes)...
Host A: Waiting for completion (ACK from Host B)...
Host A: Completion received!
Host A:   wr_id=1, status=SUCCESS, byte_len=0

*** Host A: RDMA_WRITE SUCCESSFUL! ***
*** Data sent to Host B over network RDMA ***

Test complete!
```

## Architecture

### Network Configuration
- **Host A**: MAC 52:54:00:12:34:56, listens on socket :1234
- **Host B**: MAC 52:54:00:12:34:57, connects to 127.0.0.1:1234
- **Protocol**: Ethernet type 0x8915 (RDMA)

### Packet Format
```
[Ethernet Header (14B)] [RDMA Header (40B)] [Payload (256B)]
```

### Flow
1. Host B starts first, waits for data
2. Host A connects and sends 256-byte RDMA_WRITE
3. Host B receives data, writes to memory, sends ACK
4. Host A receives ACK, posts completion
5. Host B verifies data integrity

## Troubleshooting

### Problem: "No completion received"
- **Cause**: Hosts not connected properly
- **Fix**: Ensure Host B starts within 2 seconds of Host A

### Problem: "Timeout waiting for data"  
- **Cause**: Host A not running or network issue
- **Fix**: Check both terminals, restart both hosts

### Problem: "Data verification failed"
- **Cause**: Memory corruption or packet error
- **Fix**: Check kernel logs, rebuild clean

## Implementation Details

### Files Created
- `kernel/rdma_net.h` - Protocol definitions
- `kernel/rdma_net.c` - TX/RX implementation (270 lines)
- `user/rdmanet_test.c` - Test program (273 lines)
- `scripts/run_host_a.sh` - Launch script for sender
- `scripts/run_host_b.sh` - Launch script for receiver

### Key Functions
- `rdma_net_tx_write()` - Builds and sends RDMA packet
- `rdma_net_rx()` - Receives and processes RDMA packet  
- `rdma_net_tx_ack()` - Sends ACK back to sender
- `rdma_qp_connect()` - Establishes network connection

### State Machine
```
QP_STATE_INIT → (connect) → QP_STATE_RTR → (first packet) → QP_STATE_RTS
```

## Performance

- **Latency**: ~10-50ms for 256-byte transfer (loopback network)
- **Throughput**: Limited by software processing in xv6
- **Packet Overhead**: 54 bytes (14B Ethernet + 40B RDMA header)

## Next Steps

1. Test with larger data sizes (up to MTU ~1500 bytes)
2. Add RDMA_READ support
3. Implement reliability (timeout/retransmission)
4. Add fragmentation for large transfers
5. Performance benchmarking

## Success Criteria

✅ Both hosts boot successfully  
✅ QPs connect to each other  
✅ RDMA packet transmitted over network  
✅ Data written to correct memory location  
✅ ACK sent and received  
✅ Completions posted on both sides  
✅ Data verification passes (all 256 bytes match)
