// user/rdmanet_test.c - Two-host RDMA network test

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/rdma.h"

#define TEST_SIZE 256
#define PGSIZE 4096

// Host A MAC: 52:54:00:12:34:56 (QEMU default)
#define HOST_A_MAC_0 0x52
#define HOST_A_MAC_1 0x54
#define HOST_A_MAC_2 0x00
#define HOST_A_MAC_3 0x12
#define HOST_A_MAC_4 0x34
#define HOST_A_MAC_5 0x56

// Host B MAC: 52:54:00:12:34:57 (configured in QEMU)
#define HOST_B_MAC_0 0x52
#define HOST_B_MAC_1 0x54
#define HOST_B_MAC_2 0x00
#define HOST_B_MAC_3 0x12
#define HOST_B_MAC_4 0x34
#define HOST_B_MAC_5 0x57

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
        printf("  host_a: sender (initiator)\n");
        printf("  host_b: receiver (target)\n");
        exit(1);
    }
    
    int is_host_a = (strcmp(argv[1], "host_a") == 0);
    
    printf("\n=== RDMA Network Test ===\n");
    printf("DEBUG: argc=%d argv[1]='%s' is_host_a=%d\n", argc, argv[1], is_host_a);
    printf("Running as: %s\n\n", is_host_a ? "Host A (sender)" : "Host B (receiver)");
    
    // Allocate buffer
    char *buf = alloc_page_aligned(TEST_SIZE);
    if (!buf) {
        printf("ERROR: Failed to allocate buffer\n");
        exit(1);
    }
    
    if (is_host_a) {
        // ========================================
        // HOST A: Sender
        // ========================================
        printf("Host A: Preparing to send data...\n");
        
        // Fill buffer with test pattern
        for (int i = 0; i < TEST_SIZE; i++) {
            buf[i] = (char)(i % 256);
        }
        printf("Host A: Filled buffer with test pattern\n");
        
        // Register memory region
        int mr_id = rdma_reg_mr(buf, TEST_SIZE, 
                               RDMA_ACCESS_LOCAL_READ | RDMA_ACCESS_REMOTE_READ);
        if (mr_id < 0) {
            printf("ERROR: Failed to register MR\n");
            exit(1);
        }
        printf("Host A: Registered MR %d (addr=%p, size=%d)\n", mr_id, buf, TEST_SIZE);
        
        // Create queue pair
        int qp_id = rdma_create_qp(64, 64);
        if (qp_id < 0) {
            printf("ERROR: Failed to create QP\n");
            exit(1);
        }
        printf("Host A: Created QP %d\n", qp_id);
        
        // Connect to Host B
        unsigned char host_b_mac[6] = {
            HOST_B_MAC_0, HOST_B_MAC_1, HOST_B_MAC_2,
            HOST_B_MAC_3, HOST_B_MAC_4, HOST_B_MAC_5
        };
        if (rdma_connect(qp_id, host_b_mac, 0) < 0) {
            printf("ERROR: Failed to connect QP\n");
            exit(1);
        }
        printf("Host A: Connected to Host B (QP 0, MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
               host_b_mac[0], host_b_mac[1], host_b_mac[2],
               host_b_mac[3], host_b_mac[4], host_b_mac[5]);
        
        // Sleep to let Host B set up
        printf("Host A: Waiting for Host B to be ready...\n");
        pause(3);
        
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
        
        printf("Host A: Posting RDMA_WRITE (%d bytes)...\n", TEST_SIZE);
        if (rdma_post_send(qp_id, &wr) < 0) {
            printf("ERROR: Failed to post send\n");
            exit(1);
        }
        
        // Poll for completion
        printf("Host A: Waiting for completion (ACK from Host B)...\n");
        struct rdma_completion comp;
        int num_comps = 0;
        for (int retry = 0; retry < 10; retry++) {
            num_comps = rdma_poll_cq(qp_id, &comp, 1);
            if (num_comps > 0) break;
            pause(1);
            printf("Host A:   polling... (attempt %d/10)\n", retry + 1);
        }
        
        if (num_comps <= 0) {
            printf("ERROR: No completion received (timeout)\n");
            rdma_destroy_qp(qp_id);
            rdma_dereg_mr(mr_id);
            exit(1);
        }
        
        printf("Host A: Completion received!\n");
        printf("Host A:   wr_id=%d, status=%s, byte_len=%d\n",
               (int)comp.wr_id, rdma_comp_status_str(comp.status), comp.byte_len);
        
        if (comp.status == RDMA_WC_SUCCESS) {
            printf("\n*** Host A: RDMA_WRITE SUCCESSFUL! ***\n");
            printf("*** Data sent to Host B over network RDMA ***\n\n");
        } else {
            printf("\nERROR: RDMA_WRITE failed with status %s\n",
                   rdma_comp_status_str(comp.status));
        }
        
        // Cleanup
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(mr_id);
        
    } else {
        // ========================================
        // HOST B: Receiver
        // ========================================
        printf("Host B: Preparing to receive data...\n");
        
        // Clear buffer
        for (int i = 0; i < TEST_SIZE; i++) {
            buf[i] = 0;
        }
        printf("Host B: Cleared buffer (all zeros)\n");
        
        // Register memory region (must be MR ID 1 to match sender)
        int mr_id = rdma_reg_mr(buf, TEST_SIZE,
                               RDMA_ACCESS_LOCAL_WRITE | RDMA_ACCESS_REMOTE_WRITE);
        if (mr_id < 0) {
            printf("ERROR: Failed to register MR\n");
            exit(1);
        }
        printf("Host B: Registered MR %d (addr=%p, size=%d)\n", mr_id, buf, TEST_SIZE);
        
        // Create queue pair (must be QP ID 0 to match sender)
        int qp_id = rdma_create_qp(64, 64);
        if (qp_id < 0) {
            printf("ERROR: Failed to create QP\n");
            exit(1);
        }
        printf("Host B: Created QP %d\n", qp_id);
        
        // Connect to Host A
        unsigned char host_a_mac[6] = {
            HOST_A_MAC_0, HOST_A_MAC_1, HOST_A_MAC_2,
            HOST_A_MAC_3, HOST_A_MAC_4, HOST_A_MAC_5
        };
        if (rdma_connect(qp_id, host_a_mac, 0) < 0) {
            printf("ERROR: Failed to connect QP\n");
            exit(1);
        }
        printf("Host B: Connected to Host A (QP 0, MAC: %02x:%02x:%02x:%02x:%02x:%02x)\n",
               host_a_mac[0], host_a_mac[1], host_a_mac[2],
               host_a_mac[3], host_a_mac[4], host_a_mac[5]);
        
        printf("Host B: Ready! Waiting for RDMA_WRITE from Host A...\n");
        
        // Poll for completion (receive side)
        struct rdma_completion comp;
        int num_comps = 0;
        for (int retry = 0; retry < 20; retry++) {
            num_comps = rdma_poll_cq(qp_id, &comp, 1);
            if (num_comps > 0) break;
            pause(1);
            if (retry % 3 == 0) {
                printf("Host B:   waiting... (%d seconds)\n", retry + 1);
            }
        }
        
        if (num_comps <= 0) {
            printf("ERROR: Timeout waiting for data (no completion received)\n");
            rdma_destroy_qp(qp_id);
            rdma_dereg_mr(mr_id);
            exit(1);
        }
        
        printf("Host B: Data received! Completion posted.\n");
        printf("Host B:   byte_len=%d, status=%s\n",
               comp.byte_len, rdma_comp_status_str(comp.status));
        
        // Verify data
        printf("Host B: Verifying data...\n");
        int errors = 0;
        int first_error = -1;
        for (int i = 0; i < TEST_SIZE; i++) {
            unsigned char expected = (unsigned char)(i % 256);
            unsigned char actual = (unsigned char)buf[i];
            if (actual != expected) {
                if (first_error < 0) {
                    first_error = i;
                    printf("Host B:   ERROR at byte %d: expected 0x%02x, got 0x%02x\n",
                           i, expected, actual);
                }
                errors++;
            }
        }
        
        if (errors == 0) {
            printf("\n*** Host B: DATA VERIFICATION PASSED! ***\n");
            printf("*** All %d bytes match expected pattern ***\n", TEST_SIZE);
            printf("*** Network RDMA working correctly! ***\n\n");
        } else {
            printf("\nERROR: DATA VERIFICATION FAILED!\n");
            printf("  %d/%d bytes corrupted\n", errors, TEST_SIZE);
            printf("  First error at byte %d\n", first_error);
        }
        
        // Cleanup
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(mr_id);
    }
    
    printf("Test complete!\n");
    exit(0);
}
