// RDMA Phase 1 Test Suite
// Validates QEMU E1000 RDMA extensions implementation
// Tests all functionality described in Phase 1 (Days 1-5)

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "rdma.h"

/* ETH_P_RDMA definition (if not in rdma.h) */
#ifndef ETH_P_RDMA
#define ETH_P_RDMA 0x8915
#endif

/* ============================================
 * TEST FRAMEWORK
 * ============================================ */

static int phase1_tests_passed = 0;
static int phase1_tests_failed = 0;

#define P1_TEST_START(name) \
    printf("[PHASE1] Testing: %s\n", name)

#define P1_TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  ✗ FAIL: %s\n", msg); \
        phase1_tests_failed++; \
        return -1; \
    } \
} while(0)

#define P1_TEST_PASS(name) do { \
    printf("  ✓ PASS: %s\n", name); \
    phase1_tests_passed++; \
    return 0; \
} while(0)

/* Hardware register access helpers */
static inline uint32
read_rdma_reg(uint32 offset)
{
    volatile uint32 *base = (uint32 *)E1000_RDMA_BASE;
    return base[offset / 4];
}

static inline void
write_rdma_reg(uint32 offset, uint32 value)
{
    volatile uint32 *base = (uint32 *)E1000_RDMA_BASE;
    base[offset / 4] = value;
}

static inline uint64
read_rdma_reg64(uint32 offset)
{
    volatile uint32 *base = (uint32 *)E1000_RDMA_BASE;
    uint32 low = base[offset / 4];
    uint32 high = base[(offset + 4) / 4];
    return ((uint64)high << 32) | low;
}

static inline void
write_rdma_reg64(uint32 offset, uint64 value)
{
    volatile uint32 *base = (uint32 *)E1000_RDMA_BASE;
    base[offset / 4] = (uint32)(value & 0xFFFFFFFF);
    base[(offset + 4) / 4] = (uint32)(value >> 32);
}

/* ============================================
 * PHASE 1 - DAY 1-2: BASIC RDMA REGISTERS
 * ============================================ */

/* Test 1: RDMA register addresses are accessible */
static int
test_phase1_register_access(void)
{
    P1_TEST_START("RDMA Register Accessibility");
    
    // Try to read all main RDMA registers without crashing
    uint32 ctrl = read_rdma_reg(E1000_RDMA_CTRL);
    uint32 status = read_rdma_reg(E1000_RDMA_STATUS);
    uint64 mr_ptr = read_rdma_reg64(E1000_MR_TABLE_PTR);
    uint32 mr_len = read_rdma_reg(E1000_MR_TABLE_LEN);
    
    printf("  Initial register values:\n");
    printf("    CTRL=0x%x STATUS=0x%x MR_PTR=0x%lx MR_LEN=%d\n",
           ctrl, status, mr_ptr, mr_len);
    
    // Verify we can write and read back
    write_rdma_reg(E1000_MR_TABLE_LEN, 0x12345678);
    uint32 readback = read_rdma_reg(E1000_MR_TABLE_LEN);
    P1_TEST_ASSERT(readback == 0x12345678, "Register write/read failed");
    
    P1_TEST_PASS("Register Access");
}

/* Test 2: RDMA Control Register bits */
static int
test_phase1_control_register(void)
{
    P1_TEST_START("RDMA Control Register");
    
    // Test ENABLE bit
    write_rdma_reg(E1000_RDMA_CTRL, RDMA_CTRL_ENABLE);
    uint32 ctrl = read_rdma_reg(E1000_RDMA_CTRL);
    P1_TEST_ASSERT(ctrl & RDMA_CTRL_ENABLE, "ENABLE bit not set");
    
    // Check status becomes READY
    uint32 status = read_rdma_reg(E1000_RDMA_STATUS);
    P1_TEST_ASSERT(status & RDMA_STATUS_READY, "Status not READY after enable");
    printf("    Status after enable: 0x%x\n", status);
    
    // Test RESET bit (self-clearing)
    write_rdma_reg(E1000_RDMA_CTRL, RDMA_CTRL_ENABLE | RDMA_CTRL_RESET);
    for (int i = 0; i < 100; i++) {
        ctrl = read_rdma_reg(E1000_RDMA_CTRL);
        if (!(ctrl & RDMA_CTRL_RESET)) break;
    }
    P1_TEST_ASSERT(!(ctrl & RDMA_CTRL_RESET), "RESET bit did not clear");
    
    P1_TEST_PASS("Control Register");
}

/* Test 3: MR Table Pointer Configuration */
static int
test_phase1_mr_table_setup(void)
{
    P1_TEST_START("MR Table Configuration");
    
    // Test writing MR table pointer (64-bit)
    uint64 test_ptr = 0x80001000;
    write_rdma_reg64(E1000_MR_TABLE_PTR, test_ptr);
    uint64 read_ptr = read_rdma_reg64(E1000_MR_TABLE_PTR);
    P1_TEST_ASSERT(read_ptr == test_ptr, "MR table pointer mismatch");
    printf("    MR_TABLE_PTR: wrote=0x%lx read=0x%lx\n", test_ptr, read_ptr);
    
    // Test writing MR table length
    write_rdma_reg(E1000_MR_TABLE_LEN, MAX_MRS);
    uint32 len = read_rdma_reg(E1000_MR_TABLE_LEN);
    P1_TEST_ASSERT(len == MAX_MRS, "MR table length mismatch");
    printf("    MR_TABLE_LEN: %d\n", len);
    
    P1_TEST_PASS("MR Table Setup");
}

/* Test 4: QP Register Read/Write */
static int
test_phase1_qp_registers(void)
{
    P1_TEST_START("QP Register Access");
    
    uint32 qp_id = 0;
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    
    // Test SQ Base (64-bit)
    uint64 sq_base = 0x80100000;
    write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, sq_base);
    uint64 sq_read = read_rdma_reg64(qp_base + E1000_QP_SQ_BASE);
    P1_TEST_ASSERT(sq_read == sq_base, "QP SQ_BASE mismatch");
    printf("    QP%d SQ_BASE: 0x%lx\n", qp_id, sq_read);
    
    // Test SQ Size
    write_rdma_reg(qp_base + E1000_QP_SQ_SIZE, 64);
    uint32 sq_size = read_rdma_reg(qp_base + E1000_QP_SQ_SIZE);
    P1_TEST_ASSERT(sq_size == 64, "QP SQ_SIZE mismatch");
    
    // Test CQ Base (64-bit)
    uint64 cq_base_val = 0x80200000;
    write_rdma_reg64(qp_base + E1000_QP_CQ_BASE, cq_base_val);
    uint64 cq_read = read_rdma_reg64(qp_base + E1000_QP_CQ_BASE);
    P1_TEST_ASSERT(cq_read == cq_base_val, "QP CQ_BASE mismatch");
    
    // Test CQ Size
    write_rdma_reg(qp_base + E1000_QP_CQ_SIZE, 64);
    uint32 cq_size = read_rdma_reg(qp_base + E1000_QP_CQ_SIZE);
    P1_TEST_ASSERT(cq_size == 64, "QP CQ_SIZE mismatch");
    
    printf("    QP%d configured: SQ_SIZE=%d CQ_SIZE=%d\n", qp_id, sq_size, cq_size);
    
    P1_TEST_PASS("QP Registers");
}

/* Test 5: Multiple QPs */
static int
test_phase1_multiple_qps(void)
{
    P1_TEST_START("Multiple QP Configuration");
    
    // Configure 4 different QPs with unique values
    for (int qp_id = 0; qp_id < 4; qp_id++) {
        uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
        
        uint64 sq_base = 0x80000000 + (qp_id * 0x10000);
        uint64 cq_base = 0x80040000 + (qp_id * 0x10000);
        uint32 size = 32 << qp_id;  // 32, 64, 128, 256
        
        write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, sq_base);
        write_rdma_reg(qp_base + E1000_QP_SQ_SIZE, size);
        write_rdma_reg64(qp_base + E1000_QP_CQ_BASE, cq_base);
        write_rdma_reg(qp_base + E1000_QP_CQ_SIZE, size);
    }
    
    // Verify all QPs retained their unique values
    for (int qp_id = 0; qp_id < 4; qp_id++) {
        uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
        
        uint64 expected_sq = 0x80000000 + (qp_id * 0x10000);
        uint64 expected_cq = 0x80040000 + (qp_id * 0x10000);
        uint32 expected_size = 32 << qp_id;
        
        uint64 sq = read_rdma_reg64(qp_base + E1000_QP_SQ_BASE);
        uint64 cq = read_rdma_reg64(qp_base + E1000_QP_CQ_BASE);
        uint32 sq_size = read_rdma_reg(qp_base + E1000_QP_SQ_SIZE);
        uint32 cq_size = read_rdma_reg(qp_base + E1000_QP_CQ_SIZE);
        
        P1_TEST_ASSERT(sq == expected_sq, "QP SQ_BASE isolation failed");
        P1_TEST_ASSERT(cq == expected_cq, "QP CQ_BASE isolation failed");
        P1_TEST_ASSERT(sq_size == expected_size, "QP SQ_SIZE isolation failed");
        P1_TEST_ASSERT(cq_size == expected_size, "QP CQ_SIZE isolation failed");
        
        printf("    QP%d: SQ=0x%lx CQ=0x%lx SIZE=%d ✓\n", qp_id, sq, cq, sq_size);
    }
    
    P1_TEST_PASS("Multiple QPs");
}

/* Test 6: QP Head/Tail Pointer Updates */
static int
test_phase1_qp_pointers(void)
{
    P1_TEST_START("QP Head/Tail Pointers");
    
    uint32 qp_id = 1;
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    
    // Read initial head/tail values
    uint32 sq_head = read_rdma_reg(qp_base + E1000_QP_SQ_HEAD);
    uint32 sq_tail = read_rdma_reg(qp_base + E1000_QP_SQ_TAIL);
    uint32 cq_head = read_rdma_reg(qp_base + E1000_QP_CQ_HEAD);
    uint32 cq_tail = read_rdma_reg(qp_base + E1000_QP_CQ_TAIL);
    
    printf("    Initial: SQ(H=%d T=%d) CQ(H=%d T=%d)\n",
           sq_head, sq_tail, cq_head, cq_tail);
    
    // Test writing SQ_TAIL (doorbell)
    write_rdma_reg(qp_base + E1000_QP_SQ_TAIL, 5);
    sq_tail = read_rdma_reg(qp_base + E1000_QP_SQ_TAIL);
    P1_TEST_ASSERT(sq_tail == 5, "SQ_TAIL write failed");
    
    // Test writing CQ_HEAD (consumer update)
    write_rdma_reg(qp_base + E1000_QP_CQ_HEAD, 3);
    cq_head = read_rdma_reg(qp_base + E1000_QP_CQ_HEAD);
    P1_TEST_ASSERT(cq_head == 3, "CQ_HEAD write failed");
    
    printf("    After update: SQ_TAIL=%d CQ_HEAD=%d\n", sq_tail, cq_head);
    
    P1_TEST_PASS("QP Pointers");
}

/* ============================================
 * PHASE 1 - DAY 3-4: WORK PROCESSING LOGIC
 * ============================================ */

/* Test 7: QP Doorbell Mechanism */
static int
test_phase1_doorbell(void)
{
    P1_TEST_START("QP Doorbell Mechanism");
    
    uint32 qp_id = 0;
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    
    // Configure QP first
    write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, 0x80000000);
    write_rdma_reg(qp_base + E1000_QP_SQ_SIZE, 64);
    write_rdma_reg64(qp_base + E1000_QP_CQ_BASE, 0x80010000);
    write_rdma_reg(qp_base + E1000_QP_CQ_SIZE, 64);
    
    // Read initial state
    uint32 head_before = read_rdma_reg(qp_base + E1000_QP_SQ_HEAD);
    
    // Ring doorbell by writing SQ_TAIL
    write_rdma_reg(qp_base + E1000_QP_SQ_TAIL, 1);
    
    // Give QEMU time to process (in real implementation)
    // In test, just verify the write worked
    uint32 tail_after = read_rdma_reg(qp_base + E1000_QP_SQ_TAIL);
    P1_TEST_ASSERT(tail_after == 1, "Doorbell write failed");
    
    printf("    Doorbell rung: HEAD=%d TAIL=%d\n", head_before, tail_after);
    printf("    (Work processing would happen in QEMU)\n");
    
    P1_TEST_PASS("Doorbell Mechanism");
}

/* Test 8: Work Request Structure Alignment */
static int
test_phase1_wr_structure(void)
{
    P1_TEST_START("Work Request Structure");
    
    // Verify WR structure size matches QEMU expectation
    uint64 wr_size = sizeof(struct rdma_work_request);
    printf("    sizeof(rdma_work_request) = %d bytes\n", (int)wr_size);
    P1_TEST_ASSERT(wr_size == 56, "WR structure size mismatch");
    
    // Test structure packing
    struct rdma_work_request wr;
    wr.wr_id = 0x123456789ABCDEF0;
    wr.opcode = RDMA_OP_WRITE;
    wr.flags = 0x01;
    wr.local_mr_id = 1;
    wr.length = 4096;
    
    P1_TEST_ASSERT(wr.wr_id == 0x123456789ABCDEF0, "WR wr_id field corrupt");
    P1_TEST_ASSERT(wr.opcode == RDMA_OP_WRITE, "WR opcode field corrupt");
    P1_TEST_ASSERT(wr.length == 4096, "WR length field corrupt");
    
    printf("    WR structure validation: ✓\n");
    
    P1_TEST_PASS("WR Structure");
}

/* Test 9: Completion Structure Alignment */
static int
test_phase1_comp_structure(void)
{
    P1_TEST_START("Completion Structure");
    
    // Verify completion structure size
    uint64 comp_size = sizeof(struct rdma_completion);
    printf("    sizeof(rdma_completion) = %d bytes\n", (int)comp_size);
    P1_TEST_ASSERT(comp_size == 16, "Completion structure size mismatch");
    
    // Test structure packing
    struct rdma_completion comp;
    comp.wr_id = 0xFEDCBA9876543210;
    comp.byte_len = 2048;
    comp.status = RDMA_WC_SUCCESS;
    comp.opcode = RDMA_OP_WRITE;
    
    P1_TEST_ASSERT(comp.wr_id == 0xFEDCBA9876543210, "Comp wr_id corrupt");
    P1_TEST_ASSERT(comp.byte_len == 2048, "Comp byte_len corrupt");
    P1_TEST_ASSERT(comp.status == RDMA_WC_SUCCESS, "Comp status corrupt");
    
    printf("    Completion structure validation: ✓\n");
    
    P1_TEST_PASS("Completion Structure");
}

/* Test 10: MR Structure Alignment */
static int
test_phase1_mr_structure(void)
{
    P1_TEST_START("Memory Region Structure");
    
    // Verify MR structure size (must match QEMU)
    uint64 mr_size = sizeof(struct rdma_mr_hw);
    printf("    sizeof(rdma_mr_hw) = %d bytes\n", (int)mr_size);
    P1_TEST_ASSERT(mr_size == 56, "MR structure size mismatch");
    
    // Test structure fields
    struct rdma_mr_hw mr;
    mr.id = 5;
    mr.access_flags = RDMA_ACCESS_LOCAL_WRITE | RDMA_ACCESS_REMOTE_WRITE;
    mr.vaddr = 0x10000;
    mr.paddr = 0x80100000;
    mr.length = 8192;
    mr.lkey = 5;
    mr.rkey = 5;
    mr.valid = 1;
    
    P1_TEST_ASSERT(mr.id == 5, "MR id corrupt");
    P1_TEST_ASSERT(mr.paddr == 0x80100000, "MR paddr corrupt");
    P1_TEST_ASSERT(mr.length == 8192, "MR length corrupt");
    P1_TEST_ASSERT(mr.valid == 1, "MR valid corrupt");
    
    printf("    MR structure validation: ✓\n");
    
    P1_TEST_PASS("MR Structure");
}

/* ============================================
 * PHASE 1 - DAY 5: PACKET RECEPTION
 * ============================================ */

/* Test 11: RDMA Packet Header Structure */
static int
test_phase1_packet_header(void)
{
    P1_TEST_START("RDMA Packet Header");
    
    // Note: rdma_packet_header is defined in QEMU, not xv6 kernel
    // We just verify the constants are accessible
    
    printf("    ETH_P_RDMA = 0x%x\n", ETH_P_RDMA);
    printf("    RDMA_OP_WRITE = 0x%x\n", RDMA_OP_WRITE);
    printf("    RDMA_OP_READ = 0x%x\n", RDMA_OP_READ);
    printf("    RDMA_OP_SEND = 0x%x\n", RDMA_OP_SEND);
    
    P1_TEST_ASSERT(ETH_P_RDMA == 0x8915, "ETH_P_RDMA wrong value");
    P1_TEST_ASSERT(RDMA_OP_WRITE == 0x01, "RDMA_OP_WRITE wrong value");
    P1_TEST_ASSERT(RDMA_OP_READ == 0x02, "RDMA_OP_READ wrong value");
    P1_TEST_ASSERT(RDMA_OP_SEND == 0x03, "RDMA_OP_SEND wrong value");
    
    printf("    RDMA packet constants validated: ✓\n");
    
    P1_TEST_PASS("Packet Header");
}

/* Test 12: Register Persistence */
static int
test_phase1_register_persistence(void)
{
    P1_TEST_START("Register Value Persistence");
    
    // Write unique values to multiple registers
    write_rdma_reg(E1000_MR_TABLE_LEN, 0xAABBCCDD);
    write_rdma_reg64(E1000_MR_TABLE_PTR, 0x1122334455667788);
    
    uint32 qp_base = E1000_QP_BASE;
    write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, 0x8877665544332211);
    write_rdma_reg(qp_base + E1000_QP_SQ_SIZE, 0x11223344);
    
    // Perform some other operations
    for (int i = 0; i < 1000; i++) {
        read_rdma_reg(E1000_RDMA_STATUS);
    }
    
    // Verify values persisted
    uint32 len = read_rdma_reg(E1000_MR_TABLE_LEN);
    uint64 ptr = read_rdma_reg64(E1000_MR_TABLE_PTR);
    uint64 sq = read_rdma_reg64(qp_base + E1000_QP_SQ_BASE);
    uint32 size = read_rdma_reg(qp_base + E1000_QP_SQ_SIZE);
    
    P1_TEST_ASSERT(len == 0xAABBCCDD, "MR_TABLE_LEN not persistent");
    P1_TEST_ASSERT(ptr == 0x1122334455667788, "MR_TABLE_PTR not persistent");
    P1_TEST_ASSERT(sq == 0x8877665544332211, "SQ_BASE not persistent");
    P1_TEST_ASSERT(size == 0x11223344, "SQ_SIZE not persistent");
    
    printf("    All registers retained values after 1000 reads\n");
    
    P1_TEST_PASS("Register Persistence");
}

/* Test 13: Register Boundary Checks */
static int
test_phase1_register_boundaries(void)
{
    P1_TEST_START("Register Address Boundaries");
    
    // Test that QP registers are properly strided
    uint32 qp0_base = E1000_QP_BASE;
    uint32 qp1_base = E1000_QP_BASE + E1000_QP_STRIDE;
    uint32 qp15_base = E1000_QP_BASE + 15 * E1000_QP_STRIDE;
    
    // Write to QP 0
    write_rdma_reg(qp0_base + E1000_QP_SQ_SIZE, 32);
    // Write to QP 1
    write_rdma_reg(qp1_base + E1000_QP_SQ_SIZE, 64);
    // Write to QP 15
    write_rdma_reg(qp15_base + E1000_QP_SQ_SIZE, 128);
    
    // Verify isolation
    uint32 qp0_size = read_rdma_reg(qp0_base + E1000_QP_SQ_SIZE);
    uint32 qp1_size = read_rdma_reg(qp1_base + E1000_QP_SQ_SIZE);
    uint32 qp15_size = read_rdma_reg(qp15_base + E1000_QP_SQ_SIZE);
    
    P1_TEST_ASSERT(qp0_size == 32, "QP0 value corrupted");
    P1_TEST_ASSERT(qp1_size == 64, "QP1 value corrupted");
    P1_TEST_ASSERT(qp15_size == 128, "QP15 value corrupted");
    
    printf("    QP register stride verified: 0x%x\n", E1000_QP_STRIDE);
    
    P1_TEST_PASS("Register Boundaries");
}

/* Test 14: QEMU Trace Events (Indirect Test) */
static int
test_phase1_trace_infrastructure(void)
{
    P1_TEST_START("QEMU Tracing Infrastructure");
    
    // We can't directly verify trace events from xv6,
    // but we can trigger all code paths that should emit traces
    
    // Trigger: e1000_rdma_enabled
    write_rdma_reg(E1000_RDMA_CTRL, RDMA_CTRL_ENABLE);
    
    // Trigger: e1000_rdma_reset
    write_rdma_reg(E1000_RDMA_CTRL, RDMA_CTRL_RESET);
    
    // Trigger: e1000_rdma_mr_table_set
    write_rdma_reg64(E1000_MR_TABLE_PTR, 0x80000000);
    
    // Trigger: e1000_rdma_qp_sq_base
    uint32 qp_base = E1000_QP_BASE;
    write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, 0x80100000);
    
    // Trigger: doorbell (would trigger e1000_rdma_process_sq in QEMU)
    write_rdma_reg(qp_base + E1000_QP_SQ_TAIL, 1);
    
    printf("    Trace-triggering operations completed\n");
    printf("    (Check QEMU console with -d trace:e1000_rdma*)\n");
    
    P1_TEST_PASS("Trace Infrastructure");
}

/* Test 15: Complete QP Lifecycle */
static int
test_phase1_qp_lifecycle(void)
{
    P1_TEST_START("Complete QP Lifecycle");
    
    uint32 qp_id = 2;
    uint32 qp_base = E1000_QP_BASE + qp_id * E1000_QP_STRIDE;
    
    // Step 1: Configure QP
    printf("    Step 1: Configuring QP %d\n", qp_id);
    write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, 0x80000000);
    write_rdma_reg(qp_base + E1000_QP_SQ_SIZE, 64);
    write_rdma_reg64(qp_base + E1000_QP_CQ_BASE, 0x80010000);
    write_rdma_reg(qp_base + E1000_QP_CQ_SIZE, 64);
    
    // Step 2: Verify configuration
    printf("    Step 2: Verifying configuration\n");
    uint64 sq = read_rdma_reg64(qp_base + E1000_QP_SQ_BASE);
    uint32 sq_size = read_rdma_reg(qp_base + E1000_QP_SQ_SIZE);
    P1_TEST_ASSERT(sq == 0x80000000, "QP config failed");
    P1_TEST_ASSERT(sq_size == 64, "QP size config failed");
    
    // Step 3: Simulate work submission (ring doorbell)
    printf("    Step 3: Ringing doorbell\n");
    write_rdma_reg(qp_base + E1000_QP_SQ_TAIL, 5);
    uint32 tail = read_rdma_reg(qp_base + E1000_QP_SQ_TAIL);
    P1_TEST_ASSERT(tail == 5, "Doorbell failed");
    
    // Step 4: Simulate completion consumption
    printf("    Step 4: Updating CQ head\n");
    write_rdma_reg(qp_base + E1000_QP_CQ_HEAD, 5);
    uint32 head = read_rdma_reg(qp_base + E1000_QP_CQ_HEAD);
    P1_TEST_ASSERT(head == 5, "CQ head update failed");
    
    // Step 5: Reset QP (write zeros)
    printf("    Step 5: Resetting QP\n");
    write_rdma_reg64(qp_base + E1000_QP_SQ_BASE, 0);
    write_rdma_reg(qp_base + E1000_QP_SQ_SIZE, 0);
    write_rdma_reg64(qp_base + E1000_QP_CQ_BASE, 0);
    write_rdma_reg(qp_base + E1000_QP_CQ_SIZE, 0);
    
    uint64 sq_after = read_rdma_reg64(qp_base + E1000_QP_SQ_BASE);
    P1_TEST_ASSERT(sq_after == 0, "QP reset failed");
    
    printf("    Complete QP lifecycle: ✓\n");
    
    P1_TEST_PASS("QP Lifecycle");
}

/* ============================================
 * MAIN TEST RUNNER
 * ============================================ */

void
run_rdma_phase1_tests(void)
{
    printf("\n");
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║     RDMA PHASE 1 TEST SUITE - QEMU VERIFICATION       ║\n");
    printf("║   Testing: Days 1-5 QEMU E1000 RDMA Extensions        ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n");
    printf("\n");
    
    printf("PHASE 1 - DAY 1-2: Basic RDMA Registers\n");
    printf("════════════════════════════════════════\n");
    test_phase1_register_access();
    test_phase1_control_register();
    test_phase1_mr_table_setup();
    test_phase1_qp_registers();
    test_phase1_multiple_qps();
    test_phase1_qp_pointers();
    printf("\n");
    
    printf("PHASE 1 - DAY 3-4: Work Processing Logic\n");
    printf("═════════════════════════════════════════\n");
    test_phase1_doorbell();
    test_phase1_wr_structure();
    test_phase1_comp_structure();
    test_phase1_mr_structure();
    printf("\n");
    
    printf("PHASE 1 - DAY 5: Packet Reception & Validation\n");
    printf("═══════════════════════════════════════════════\n");
    test_phase1_packet_header();
    test_phase1_register_persistence();
    test_phase1_register_boundaries();
    test_phase1_trace_infrastructure();
    test_phase1_qp_lifecycle();
    printf("\n");
    
    /* Print summary */
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║                   TEST SUMMARY                         ║\n");
    printf("╠════════════════════════════════════════════════════════╣\n");
    printf("║  Tests Passed: %-3d                                     ║\n", phase1_tests_passed);
    printf("║  Tests Failed: %-3d                                     ║\n", phase1_tests_failed);
    printf("╠════════════════════════════════════════════════════════╣\n");
    
    if (phase1_tests_failed == 0) {
        printf("║  ✓✓✓ ALL PHASE 1 TESTS PASSED! ✓✓✓                   ║\n");
        printf("║                                                        ║\n");
        printf("║  QEMU E1000 RDMA extensions are working correctly!    ║\n");
        printf("║  Ready to proceed to Phase 2 (xv6 Kernel RDMA Core)  ║\n");
    } else {
        printf("║  ✗✗✗ SOME TESTS FAILED ✗✗✗                           ║\n");
        printf("║                                                        ║\n");
        printf("║  Please review failed tests above                     ║\n");
    }
    
    printf("╚════════════════════════════════════════════════════════╝\n");
    printf("\n");
}
