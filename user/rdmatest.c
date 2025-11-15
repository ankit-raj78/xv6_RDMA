// user/rdmatest.c - User-space RDMA test program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "user/rdma.h"

#define TEST_SIZE 512   // 512 bytes - small enough to fit in one page
#define PGSIZE 4096     // Page size

// Helper to print test result
void print_result(char *test_name, int passed)
{
    if (passed) {
        printf("[PASS] %s\n", test_name);
    } else {
        printf("[FAIL] %s\n", test_name);
    }
}

// Allocate a page-aligned buffer using sbrk
void* alloc_page_aligned(int size)
{
    char *p = sbrk(size + PGSIZE);
    if (p == (char*)-1) {
        return 0;
    }
    // Align to page boundary
    uint64 addr = (uint64)p;
    uint64 aligned = (addr + PGSIZE - 1) & ~(PGSIZE - 1);
    return (void*)aligned;
}

// Test 1: Memory region registration and deregistration
int test_mr_registration(void)
{
    char buffer[TEST_SIZE];
    int mr_id;
    
    // Register memory region with read/write access
    mr_id = rdma_reg_mr(buffer, TEST_SIZE, 
                       RDMA_ACCESS_LOCAL_READ | RDMA_ACCESS_LOCAL_WRITE |
                       RDMA_ACCESS_REMOTE_READ | RDMA_ACCESS_REMOTE_WRITE);
    
    if (mr_id < 0) {
        printf("  ERROR: Failed to register memory region\n");
        return 0;
    }
    
    printf("  Registered MR %d at %p, size %d bytes\n", mr_id, buffer, TEST_SIZE);
    
    // Deregister memory region
    if (rdma_dereg_mr(mr_id) < 0) {
        printf("  ERROR: Failed to deregister memory region\n");
        return 0;
    }
    
    printf("  Deregistered MR %d\n", mr_id);
    return 1;
}

// Test 2: Queue pair creation and destruction
int test_qp_creation(void)
{
    int qp_id;
    
    // Create queue pair
    qp_id = rdma_create_qp(64, 64);  // 64 entry SQ and CQ
    
    if (qp_id < 0) {
        printf("  ERROR: Failed to create queue pair\n");
        return 0;
    }
    
    printf("  Created QP %d (SQ=64, CQ=64)\n", qp_id);
    
    // Destroy queue pair
    if (rdma_destroy_qp(qp_id) < 0) {
        printf("  ERROR: Failed to destroy queue pair\n");
        return 0;
    }
    
    printf("  Destroyed QP %d\n", qp_id);
    return 1;
}

// Test 3: RDMA_WRITE operation
int test_rdma_write(void)
{
    char *src_buffer;
    char *dst_buffer;
    int src_mr_id, dst_mr_id;
    int qp_id;
    struct rdma_work_request wr;
    struct rdma_completion comp;
    int i, num_comps;
    
    // Allocate page-aligned buffers to avoid crossing page boundaries
    src_buffer = alloc_page_aligned(TEST_SIZE);
    dst_buffer = alloc_page_aligned(TEST_SIZE);
    
    if (!src_buffer || !dst_buffer) {
        printf("  ERROR: Failed to allocate page-aligned buffers\n");
        return 0;
    }
    
    printf("  Allocated page-aligned buffers (src=%p, dst=%p)\n", src_buffer, dst_buffer);
    
    // Initialize buffers
    for (i = 0; i < TEST_SIZE; i++) {
        src_buffer[i] = (char)(i % 256);  // Pattern: 0-255 repeating
        dst_buffer[i] = 0;                // Clear destination
    }
    
    printf("  Initialized buffers (src with pattern, dst cleared)\n");
    
    // Register memory regions
    src_mr_id = rdma_reg_mr(src_buffer, TEST_SIZE,
                           RDMA_ACCESS_LOCAL_READ | RDMA_ACCESS_REMOTE_READ);
    if (src_mr_id < 0) {
        printf("  ERROR: Failed to register source MR\n");
        return 0;
    }
    
    dst_mr_id = rdma_reg_mr(dst_buffer, TEST_SIZE,
                           RDMA_ACCESS_LOCAL_WRITE | RDMA_ACCESS_REMOTE_WRITE);
    if (dst_mr_id < 0) {
        printf("  ERROR: Failed to register destination MR\n");
        rdma_dereg_mr(src_mr_id);
        return 0;
    }
    
    printf("  Registered MRs: src=%d, dst=%d\n", src_mr_id, dst_mr_id);
    
    // Create queue pair
    qp_id = rdma_create_qp(64, 64);
    if (qp_id < 0) {
        printf("  ERROR: Failed to create QP\n");
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    printf("  Created QP %d\n", qp_id);
    
    // Build RDMA_WRITE work request
    rdma_build_write_wr(&wr, 
                       123,                    // wr_id
                       src_mr_id,              // local MR
                       0,                      // local offset
                       dst_mr_id,              // remote MR
                       (unsigned long)dst_buffer,  // remote addr
                       dst_mr_id,              // remote key
                       TEST_SIZE);             // length
    
    printf("  Built RDMA_WRITE work request (wr_id=123, len=%d)\n", TEST_SIZE);
    
    // Post send
    if (rdma_post_send(qp_id, &wr) < 0) {
        printf("  ERROR: Failed to post send\n");
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    printf("  Posted RDMA_WRITE operation\n");
    
    // Poll for completion
    num_comps = rdma_poll_cq(qp_id, &comp, 1);
    if (num_comps < 0) {
        printf("  ERROR: Failed to poll CQ\n");
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    if (num_comps == 0) {
        printf("  ERROR: No completion received\n");
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    printf("  Polled completion: wr_id=%d, status=%s, byte_len=%d\n",
           (int)comp.wr_id, rdma_comp_status_str(comp.status), comp.byte_len);
    
    // Verify completion
    if (!rdma_comp_is_success(&comp)) {
        printf("  ERROR: Completion status is not SUCCESS\n");
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    if (comp.wr_id != 123) {
        printf("  ERROR: Completion wr_id mismatch (expected 123, got %d)\n", 
               (int)comp.wr_id);
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    // Verify data
    int mismatches = 0;
    for (i = 0; i < TEST_SIZE; i++) {
        if (src_buffer[i] != dst_buffer[i]) {
            mismatches++;
            if (mismatches <= 5) {  // Print first 5 mismatches
                printf("  Data mismatch at offset %d: expected %d, got %d\n",
                       i, src_buffer[i] & 0xFF, dst_buffer[i] & 0xFF);
            }
        }
    }
    
    if (mismatches > 0) {
        printf("  ERROR: Data verification failed (%d mismatches)\n", mismatches);
        rdma_destroy_qp(qp_id);
        rdma_dereg_mr(src_mr_id);
        rdma_dereg_mr(dst_mr_id);
        return 0;
    }
    
    printf("  Data verification passed (all %d bytes match)\n", TEST_SIZE);
    
    // Cleanup
    rdma_destroy_qp(qp_id);
    rdma_dereg_mr(src_mr_id);
    rdma_dereg_mr(dst_mr_id);
    
    return 1;
}

// Main test runner
int main(int argc, char *argv[])
{
    int passed = 0;
    int total = 0;
    
    printf("=== xv6 RDMA User-Space Test Suite ===\n\n");
    
    // Test 1: MR registration
    printf("Test 1: Memory Region Registration\n");
    total++;
    if (test_mr_registration()) {
        passed++;
        print_result("MR Registration", 1);
    } else {
        print_result("MR Registration", 0);
    }
    printf("\n");
    
    // Test 2: QP creation
    printf("Test 2: Queue Pair Creation\n");
    total++;
    if (test_qp_creation()) {
        passed++;
        print_result("QP Creation", 1);
    } else {
        print_result("QP Creation", 0);
    }
    printf("\n");
    
    // Test 3: RDMA_WRITE
    printf("Test 3: RDMA_WRITE Operation\n");
    total++;
    if (test_rdma_write()) {
        passed++;
        print_result("RDMA_WRITE", 1);
    } else {
        print_result("RDMA_WRITE", 0);
    }
    printf("\n");
    
    // Summary
    printf("=== Test Summary ===\n");
    printf("Passed: %d/%d\n", passed, total);
    
    if (passed == total) {
        printf("All tests PASSED!\n");
        exit(0);
    } else {
        printf("Some tests FAILED!\n");
        exit(1);
    }
}
