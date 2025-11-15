// kernel/sysrdma.c - RDMA system calls for user space

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "rdma.h"

// Register memory region
// args: addr (uint64), len (uint64), flags (int)
// returns: mr_id on success, -1 on failure
uint64
sys_rdma_reg_mr(void)
{
    uint64 addr;
    uint64 len;
    int flags;
    
    argaddr(0, &addr);
    argaddr(1, &len);
    argint(2, &flags);
    
    // Validate parameters
    if (addr == 0 || len == 0) {
        return -1;
    }
    
    // Call kernel RDMA function
    int mr_id = rdma_mr_register(addr, len, flags);
    return mr_id;
}

// Deregister memory region
// args: mr_id (int)
// returns: 0 on success, -1 on failure
uint64
sys_rdma_dereg_mr(void)
{
    int mr_id;
    
    argint(0, &mr_id);
    
    // Validate mr_id
    if (mr_id < 0 || mr_id >= MAX_MRS) {
        return -1;
    }
    
    // Call kernel RDMA function
    int ret = rdma_mr_deregister(mr_id);
    return ret;
}

// Create queue pair
// args: sq_size (uint32), cq_size (uint32)
// returns: qp_id on success, -1 on failure
uint64
sys_rdma_create_qp(void)
{
    int sq_size;
    int cq_size;
    
    argint(0, &sq_size);
    argint(1, &cq_size);
    
    // Validate queue sizes (must be positive and reasonable)
    if (sq_size <= 0 || sq_size > 1024 || cq_size <= 0 || cq_size > 1024) {
        return -1;
    }
    
    // Call kernel RDMA function
    int qp_id = rdma_qp_create((uint32)sq_size, (uint32)cq_size);
    return qp_id;
}

// Destroy queue pair
// args: qp_id (int)
// returns: 0 on success, -1 on failure
uint64
sys_rdma_destroy_qp(void)
{
    int qp_id;
    
    argint(0, &qp_id);
    
    // Validate qp_id
    if (qp_id < 0 || qp_id >= MAX_QPS) {
        return -1;
    }
    
    // Call kernel RDMA function
    int ret = rdma_qp_destroy(qp_id);
    return ret;
}

// Post send work request
// args: qp_id (int), wr (struct rdma_work_request *)
// returns: 0 on success, -1 on failure
uint64
sys_rdma_post_send(void)
{
    int qp_id;
    uint64 wr_ptr;
    struct rdma_work_request wr;
    struct proc *p = myproc();
    
    argint(0, &qp_id);
    argaddr(1, &wr_ptr);
    
    // Validate qp_id
    if (qp_id < 0 || qp_id >= MAX_QPS) {
        return -1;
    }
    
    // Copy work request from user space
    if (copyin(p->pagetable, (char*)&wr, wr_ptr, sizeof(wr)) < 0) {
        return -1;
    }
    
    // Call kernel RDMA function
    int ret = rdma_qp_post_send(qp_id, &wr);
    return ret;
}

// Poll completion queue
// args: qp_id (int), comps (struct rdma_completion *), max_comps (int)
// returns: number of completions polled, -1 on failure
uint64
sys_rdma_poll_cq(void)
{
    int qp_id;
    uint64 comps_ptr;
    int max_comps;
    struct rdma_completion comps[16];  // Stack buffer for completions
    struct proc *p = myproc();
    
    argint(0, &qp_id);
    argaddr(1, &comps_ptr);
    argint(2, &max_comps);
    
    // Validate parameters
    if (qp_id < 0 || qp_id >= MAX_QPS) {
        return -1;
    }
    
    if (max_comps <= 0 || max_comps > 16) {
        return -1;  // Limit to 16 completions per poll
    }
    
    // Poll completions from kernel
    int num_comps = rdma_qp_poll_cq(qp_id, comps, max_comps);
    
    if (num_comps < 0) {
        return -1;
    }
    
    // Copy completions to user space
    if (num_comps > 0) {
        if (copyout(p->pagetable, comps_ptr, (char*)comps, 
                    num_comps * sizeof(struct rdma_completion)) < 0) {
            return -1;
        }
    }
    
    return num_comps;
}

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
