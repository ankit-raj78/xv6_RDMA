// kernel/rdma_test.h - RDMA Kernel-Space Unit Tests
// This file is included by rdma.c when RDMA_TESTING is defined

#ifndef _RDMA_TEST_H_
#define _RDMA_TEST_H_

/* ============================================
 * TEST FRAMEWORK
 * ============================================ */

static int rdma_tests_passed = 0;
static int rdma_tests_failed = 0;

#define RDMA_TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  ✗ FAILED: %s\n", msg); \
        rdma_tests_failed++; \
        return -1; \
    } \
} while(0)

#define RDMA_TEST_PASS(name) do { \
    printf("  ✓ PASSED: %s\n", name); \
    rdma_tests_passed++; \
    return 0; \
} while(0)

/* ============================================
 * TEST 1: MR Table Initialization
 * ============================================ */
static int rdma_test_mr_init(void)
{
    printf("TEST 1: MR Table Initialization\n");
    
    acquire(&mr_lock);
    
    // Check that all MR slots are initially invalid
    int all_invalid = 1;
    for (int i = 0; i < MAX_MRS; i++) {
        if (mr_table[i].hw.valid != 0) {
            all_invalid = 0;
            printf("  MR slot %d has valid=%d (expected 0)\n", i, mr_table[i].hw.valid);
            break;
        }
        if (mr_table[i].hw.id != 0) {
            all_invalid = 0;
            printf("  MR slot %d has id=%d (expected 0)\n", i, mr_table[i].hw.id);
            break;
        }
        if (mr_table[i].refcount != 0) {
            all_invalid = 0;
            printf("  MR slot %d has refcount=%d (expected 0)\n", i, mr_table[i].refcount);
            break;
        }
    }
    
    release(&mr_lock);
    
    RDMA_TEST_ASSERT(all_invalid, "MR table not properly initialized");
    RDMA_TEST_PASS("MR Table Initialization");
}

/* ============================================
 * TEST 2: QP Table Initialization
 * ============================================ */
static int rdma_test_qp_init(void)
{
    printf("TEST 2: QP Table Initialization\n");
    
    acquire(&qp_lock);
    
    // Check that all QP slots are initially invalid and in RESET state
    int all_valid = 1;
    for (int i = 0; i < MAX_QPS; i++) {
        if (qp_table[i].valid != 0) {
            all_valid = 0;
            printf("  QP slot %d has valid=%d (expected 0)\n", i, qp_table[i].valid);
            break;
        }
        if (qp_table[i].state != QP_STATE_RESET) {
            all_valid = 0;
            printf("  QP slot %d has state=%d (expected RESET=%d)\n", 
                   i, qp_table[i].state, QP_STATE_RESET);
            break;
        }
        if (qp_table[i].outstanding_ops != 0) {
            all_valid = 0;
            printf("  QP slot %d has outstanding_ops=%d (expected 0)\n", 
                   i, qp_table[i].outstanding_ops);
            break;
        }
    }
    
    release(&qp_lock);
    
    RDMA_TEST_ASSERT(all_valid, "QP table not properly initialized");
    RDMA_TEST_PASS("QP Table Initialization");
}

/* ============================================
 * TEST 3: Hardware Register Access
 * ============================================ */
static int rdma_test_hw_regs(void)
{
    printf("TEST 3: Hardware Register Access\n");
    
    // Test that we can read RDMA registers (reading should not crash)
    uint32 ctrl_val = rdma_readreg(E1000_RDMA_CTRL);
    uint32 status_val = rdma_readreg(E1000_RDMA_STATUS);
    
    printf("  RDMA_CTRL = 0x%x\n", ctrl_val);
    printf("  RDMA_STATUS = 0x%x\n", status_val);
    
    // Verify status register shows READY (since we called rdma_hw_enable())
    RDMA_TEST_ASSERT(status_val & RDMA_STATUS_READY, "Hardware not ready");
    
    RDMA_TEST_PASS("Hardware Register Access");
}

/* ============================================
 * TEST 4: Lock Functionality
 * ============================================ */
static int rdma_test_locks(void)
{
    printf("TEST 4: Lock Functionality\n");
    
    // Test that locks are initialized (not locked)
    RDMA_TEST_ASSERT(mr_lock.locked == 0, "MR lock initially locked");
    RDMA_TEST_ASSERT(qp_lock.locked == 0, "QP lock initially locked");
    
    // Test MR lock acquire/release
    acquire(&mr_lock);
    RDMA_TEST_ASSERT(mr_lock.locked == 1, "MR lock acquire failed");
    release(&mr_lock);
    RDMA_TEST_ASSERT(mr_lock.locked == 0, "MR lock release failed");
    
    // Test QP lock acquire/release
    acquire(&qp_lock);
    RDMA_TEST_ASSERT(qp_lock.locked == 1, "QP lock acquire failed");
    release(&qp_lock);
    RDMA_TEST_ASSERT(qp_lock.locked == 0, "QP lock release failed");
    
    RDMA_TEST_PASS("Lock Functionality");
}

/* ============================================
 * TEST 5: Ring Buffer Logic
 * ============================================ */
static int rdma_test_ring_buffer(void)
{
    printf("TEST 5: Ring Buffer Logic\n");
    
    // Test 1: Ring buffer wraparound calculation
    uint32 size = 64;
    uint32 head = 63;
    uint32 next = (head + 1) % size;
    RDMA_TEST_ASSERT(next == 0, "Ring buffer wraparound failed");
    
    // Test 2: Empty queue detection (head == tail)
    head = 10;
    uint32 tail = 10;
    int is_empty = (head == tail);
    RDMA_TEST_ASSERT(is_empty, "Failed to detect empty queue");
    
    // Test 3: Not full detection
    head = 10;
    tail = 11;
    uint32 next_tail = (tail + 1) % size;
    int is_full = (next_tail == head);
    RDMA_TEST_ASSERT(!is_full, "False positive on queue full");
    
    // Test 4: Actually full detection
    tail = 9;
    next_tail = (tail + 1) % size;
    is_full = (next_tail == head);
    RDMA_TEST_ASSERT(is_full, "Failed to detect full queue");
    
    // Test 5: Multiple wraparounds
    head = 0;
    for (int i = 0; i < 200; i++) {
        head = (head + 1) % size;
    }
    RDMA_TEST_ASSERT(head == 8, "Multiple wraparounds incorrect");
    
    RDMA_TEST_PASS("Ring Buffer Logic");
}

/* ============================================
 * TEST 6: Physical Address Conversion
 * ============================================ */
static int rdma_test_phys_addr(void)
{
    printf("TEST 6: Physical Address Conversion\n");
    
    // Allocate kernel memory
    char *kbuf = kalloc();
    RDMA_TEST_ASSERT(kbuf != 0, "kalloc failed");
    
    uint64 va = (uint64)kbuf;
    RDMA_TEST_ASSERT(va >= KERNBASE, "Kernel buffer not in kernel space");
    printf("  VA = 0x%lx, KERNBASE = 0x%lx\n", va, KERNBASE);
    
    // Convert to physical
    uint64 pa = va - KERNBASE;
    printf("  PA = 0x%lx, PHYSTOP = 0x%lx\n", pa, PHYSTOP);
    RDMA_TEST_ASSERT(pa < PHYSTOP, "Physical address out of range");
    
    // Convert back
    uint64 va2 = pa + KERNBASE;
    RDMA_TEST_ASSERT(va == va2, "VA to PA conversion inconsistent");
    
    kfree(kbuf);
    RDMA_TEST_PASS("Physical Address Conversion");
}

/* ============================================
 * TEST 7: Page Boundary Check Logic
 * ============================================ */
static int rdma_test_page_boundary(void)
{
    printf("TEST 7: Page Boundary Check\n");
    
    // Test 1: Buffer within single page
    uint64 addr1 = PGSIZE;
    uint64 len1 = 1024;
    uint64 start1 = PGROUNDDOWN(addr1);
    uint64 end1 = PGROUNDDOWN(addr1 + len1 - 1);
    printf("  Test 1: addr=0x%lx len=%ld -> start=0x%lx end=0x%lx\n", addr1, len1, start1, end1);
    RDMA_TEST_ASSERT(start1 == end1, "Single-page buffer marked as crossing");
    
    // Test 2: Buffer crossing page boundary
    uint64 addr2 = PGSIZE - 100;
    uint64 len2 = 200;
    uint64 start2 = PGROUNDDOWN(addr2);
    uint64 end2 = PGROUNDDOWN(addr2 + len2 - 1);
    printf("  Test 2: addr=0x%lx len=%ld -> start=0x%lx end=0x%lx\n", 
           addr2, len2, start2, end2);
    RDMA_TEST_ASSERT(start2 != end2, "Cross-page buffer not detected");
    
    // Test 3: Buffer exactly at page boundary
    uint64 addr3 = PGSIZE * 2;
    uint64 len3 = 512;
    uint64 start3 = PGROUNDDOWN(addr3);
    uint64 end3 = PGROUNDDOWN(addr3 + len3 - 1);
    printf("  Test 3: addr=0x%lx len=%ld -> start=0x%lx end=0x%lx\n", 
           addr3, len3, start3, end3);
    RDMA_TEST_ASSERT(start3 == end3, "Page-aligned buffer marked as crossing");
    
    // Test 4: Maximum single-page buffer
    uint64 addr4 = PGSIZE;
    uint64 len4 = PGSIZE;
    uint64 start4 = PGROUNDDOWN(addr4);
    uint64 end4 = PGROUNDDOWN(addr4 + len4 - 1);
    printf("  Test 4: addr=0x%lx len=%ld -> start=0x%lx end=0x%lx\n", 
           addr4, len4, start4, end4);
    RDMA_TEST_ASSERT(start4 != end4, "Full-page buffer crosses boundary");
    
    RDMA_TEST_PASS("Page Boundary Check");
}

/* ============================================
 * TEST 8: MR Table Manipulation
 * ============================================ */
static int rdma_test_mr_table(void)
{
    printf("TEST 8: MR Table Manipulation\n");
    
    acquire(&mr_lock);
    
    // Test 1: Find free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_MRS; i++) {
        if (!mr_table[i].hw.valid) {
            free_slot = i;
            break;
        }
    }
    RDMA_TEST_ASSERT(free_slot >= 0, "No free MR slots found");
    printf("  Found free slot: %d\n", free_slot);
    
    // Test 2: Simulate marking slot as used
    struct rdma_mr *mr = &mr_table[free_slot];
    mr->hw.id = free_slot + 1;  // 1-based IDs
    mr->hw.valid = 1;
    mr->hw.vaddr = 0x10000;
    mr->hw.paddr = 0x80000;
    mr->hw.length = 2048;
    mr->hw.access_flags = RDMA_ACCESS_LOCAL_WRITE;
    mr->refcount = 0;
    
    // Test 3: Verify the slot is now marked valid
    RDMA_TEST_ASSERT(mr_table[free_slot].hw.valid == 1, "Slot not marked valid");
    RDMA_TEST_ASSERT(mr_table[free_slot].hw.id == free_slot + 1, "ID not set correctly");
    
    // Test 4: Clean up - mark slot as free again
    mr->hw.valid = 0;
    mr->hw.id = 0;
    
    release(&mr_lock);
    
    RDMA_TEST_PASS("MR Table Manipulation");
}

/* ============================================
 * TEST 9: QP Memory Allocation
 * ============================================ */
static int rdma_test_qp_alloc(void)
{
    printf("TEST 9: QP Memory Allocation\n");
    
    // Test allocating memory for SQ and CQ
    struct rdma_work_request *sq = (struct rdma_work_request *)kalloc();
    RDMA_TEST_ASSERT(sq != 0, "Failed to allocate SQ");
    
    struct rdma_completion *cq = (struct rdma_completion *)kalloc();
    RDMA_TEST_ASSERT(cq != 0, "Failed to allocate CQ");
    
    // Test that we can write to these buffers
    sq[0].wr_id = 12345;
    sq[0].opcode = RDMA_OP_WRITE;
    RDMA_TEST_ASSERT(sq[0].wr_id == 12345, "SQ write failed");
    
    cq[0].wr_id = 67890;
    cq[0].status = RDMA_WC_SUCCESS;
    RDMA_TEST_ASSERT(cq[0].wr_id == 67890, "CQ write failed");
    
    // Convert to physical addresses
    uint64 sq_va = (uint64)sq;
    uint64 sq_pa = (sq_va >= KERNBASE) ? (sq_va - KERNBASE) : sq_va;
    RDMA_TEST_ASSERT(sq_pa < PHYSTOP, "SQ physical address invalid");
    
    uint64 cq_va = (uint64)cq;
    uint64 cq_pa = (cq_va >= KERNBASE) ? (cq_va - KERNBASE) : cq_va;
    RDMA_TEST_ASSERT(cq_pa < PHYSTOP, "CQ physical address invalid");
    
    printf("  SQ: VA=0x%lx PA=0x%lx\n", sq_va, sq_pa);
    printf("  CQ: VA=0x%lx PA=0x%lx\n", cq_va, cq_pa);
    
    // Clean up
    kfree((void *)sq);
    kfree((void *)cq);
    
    RDMA_TEST_PASS("QP Memory Allocation");
}

/* ============================================
 * TEST 10: Power-of-2 Validation
 * ============================================ */
static int rdma_test_power_of_2(void)
{
    printf("TEST 10: Power-of-2 Validation\n");
    
    // Test valid power-of-2 sizes
    uint32 valid_sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    for (int i = 0; i < 9; i++) {
        uint32 size = valid_sizes[i];
        int is_pow2 = (size != 0) && ((size & (size - 1)) == 0);
        if (!is_pow2) {
            printf("  Size %d incorrectly marked as non-power-of-2\n", size);
        }
        RDMA_TEST_ASSERT(is_pow2, "Valid power-of-2 rejected");
    }
    
    // Test invalid non-power-of-2 sizes
    uint32 invalid_sizes[] = {3, 5, 7, 9, 15, 31, 63, 127};
    for (int i = 0; i < 8; i++) {
        uint32 size = invalid_sizes[i];
        int is_pow2 = (size != 0) && ((size & (size - 1)) == 0);
        if (is_pow2) {
            printf("  Size %d incorrectly marked as power-of-2\n", size);
        }
        RDMA_TEST_ASSERT(!is_pow2, "Invalid non-power-of-2 accepted");
    }
    
    // Test zero
    int is_zero_pow2 = (0 != 0) && ((0 & (0 - 1)) == 0);
    RDMA_TEST_ASSERT(!is_zero_pow2, "Zero incorrectly marked as power-of-2");
    
    RDMA_TEST_PASS("Power-of-2 Validation");
}

/* ============================================
 * MAIN TEST RUNNER
 * ============================================ */
static void rdma_run_kernel_tests(void)
{
    printf("\n");
    printf("========================================\n");
    printf("  RDMA KERNEL-SPACE UNIT TESTS\n");
    printf("========================================\n");
    
    // Run all tests
    rdma_test_mr_init();
    rdma_test_qp_init();
    rdma_test_hw_regs();
    rdma_test_locks();
    rdma_test_ring_buffer();
    rdma_test_phys_addr();
    rdma_test_page_boundary();
    rdma_test_mr_table();
    rdma_test_qp_alloc();
    rdma_test_power_of_2();
    
    // Print summary
    printf("========================================\n");
    printf("  Tests Passed: %d\n", rdma_tests_passed);
    printf("  Tests Failed: %d\n", rdma_tests_failed);
    
    if (rdma_tests_failed == 0) {
        printf("  ✓ ALL KERNEL TESTS PASSED!\n");
    } else {
        printf("  ✗ SOME TESTS FAILED\n");
        panic("RDMA kernel tests failed");
    }
    printf("========================================\n\n");
}

#endif /* _RDMA_TEST_H_ */

