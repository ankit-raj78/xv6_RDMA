# RDMA xv6 - Setup Complete! ✅

## Environment Verified

### ✅ Installed Software
- **QEMU**: version 10.1.2 (RISC-V support)
- **RISC-V Toolchain**: riscv64-unknown-elf-gcc 15.1.0
- **Python**: 3.x (for test scripts)
- **Git**: Repository initialized

### ✅ xv6 Base
- xv6-riscv cloned and integrated
- Successfully compiled kernel
- Project structure created

## Quick Start

### Build and Run
```bash
cd /Users/ankitraj2/510/xv6_RDMA
make clean
make qemu
```

To exit QEMU: Press `Ctrl-A`, then `X`

### Debug Mode
Terminal 1:
```bash
make qemu-gdb
```

Terminal 2:
```bash
riscv64-unknown-elf-gdb kernel/kernel
(gdb) target remote localhost:26000
(gdb) break main
(gdb) continue
```

## Project Structure
```
xv6_RDMA/
├── kernel/         # Kernel source code
│   ├── rdma.c      # [TO BE CREATED] RDMA core
│   ├── rdma.h      # [TO BE CREATED] RDMA kernel headers
│   └── ...         # Existing xv6 kernel files
├── user/           # User-space programs
│   ├── rdma.h      # [TO BE CREATED] User API
│   ├── rdmatest.c  # [TO BE CREATED] Test suite
│   └── ...         # Existing xv6 user programs
├── docs/           # Documentation
│   ├── DESIGN.md   # Architecture & design
│   ├── API.md      # API documentation
│   └── SETUP.md    # This file
├── tests/          # Test files
├── benchmarks/     # Performance benchmarks
└── scripts/        # Helper scripts
```

## Next Steps (Day 1 - Setup Day)

### 1. Create Git Branch for Development
```bash
cd /Users/ankitraj2/510/xv6_RDMA
git checkout -b rdma-dev
git add docs/
git commit -m "Day 1: Project setup and documentation structure"
git push origin rdma-dev
```

### 2. Review Documentation
- Read `docs/DESIGN.md` - Understand the architecture
- Read `docs/API.md` - Familiarize with planned API
- Discuss design decisions with your team

### 3. Setup Team Collaboration
Each team member should:
```bash
git clone https://github.com/ankit-raj78/xv6_RDMA.git
cd xv6_RDMA
git checkout rdma-dev
make clean && make qemu
```

### 4. Create Task Assignments
Suggested division:
- **Person 1**: Memory Registration (kernel/rdma.c - MR functions)
- **Person 2**: Queue Pairs (kernel/rdma.c - QP functions)
- **Person 3**: E1000 Extensions (kernel/e1000.c)
- **Person 4**: Testing & Documentation (user/rdmatest.c)

### 5. Setup Communication
- Create Slack/Discord channel: #xv6-rdma
- Daily standup time: [Choose time]
- Code review process: All PRs need 1 approval

## Timeline Reminder

**Week 1: Foundation & Core RDMA (Days 1-7)**
- ✅ Day 1: Setup & Design (COMPLETE)
- Day 2: Memory Registration - Part 1
- Day 3: Memory Registration - Part 2 (✓ Milestone 1)
- Day 4: Queue Pair Creation
- Day 5: Work Request Posting
- Day 6: Completion Polling (✓ Milestone 2)
- Day 7: Loopback RDMA Write (✓ Milestone 3)

## Useful Commands

### Build Commands
```bash
make clean          # Clean build artifacts
make qemu           # Build and run
make qemu-gdb       # Build and run with GDB
make fs.img         # Rebuild filesystem
```

### Testing Commands (after implementation)
```bash
# In xv6:
$ rdmatest          # Run test suite
$ rdmabench         # Run benchmarks
```

### Git Workflow
```bash
# Start new feature
git checkout rdma-dev
git pull origin rdma-dev
git checkout -b feature/my-feature

# Make changes, commit
git add .
git commit -m "Implement memory registration"
git push origin feature/my-feature

# Create PR on GitHub
# After review and approval, merge to rdma-dev
```

## Troubleshooting

### Issue: "Command not found: riscv64-unknown-elf-gcc"
```bash
# Check if it's in PATH
echo $PATH | grep homebrew

# Add to PATH if needed (add to ~/.zshrc)
export PATH="/opt/homebrew/bin:$PATH"
source ~/.zshrc
```

### Issue: QEMU won't start
```bash
# Verify QEMU installation
which qemu-system-riscv64
qemu-system-riscv64 --version

# Reinstall if needed
brew reinstall qemu
```

### Issue: Build fails
```bash
# Clean everything and rebuild
make clean
rm -f kernel/*.o user/*.o
make
```

## Resources

### Essential Reading
- [xv6 Book](https://pdos.csail.mit.edu/6.828/2023/xv6/book-riscv-rev3.pdf)
- [RDMA Introduction](https://blog.zhaw.ch/icclab/infiniband-an-introduction-simple-ib-verbs-program-with-rdma-write/)
- Project Proposal (your submitted PDF)

### xv6 Tips
- Use `printf()` in kernel for debugging
- Check `kernel/defs.h` for available kernel functions
- Study `kernel/vm.c` for memory management examples
- Study `kernel/proc.c` for per-process data structures

### RDMA Concepts
- Memory regions prevent page swapping
- Queue pairs enable kernel bypass
- Zero-copy means NIC accesses user memory directly
- Work requests are posted to send queue
- Completions are retrieved from completion queue

## Team Contact

**Repository**: https://github.com/ankit-raj78/xv6_RDMA
**Branch**: rdma-dev

## Success Checklist for Day 1

- [x] QEMU installed and verified
- [x] RISC-V toolchain installed and verified
- [x] xv6 cloned and builds successfully
- [x] xv6 runs in QEMU
- [x] Project structure created
- [x] Documentation written (DESIGN.md, API.md)
- [x] Git repository initialized
- [ ] Team members have cloned repo
- [ ] Team communication setup
- [ ] Task assignments decided

## Tomorrow (Day 2): Memory Registration

Tomorrow you'll start implementing memory registration:
1. Add system call definitions
2. Create `kernel/rdma.c` and `kernel/rdma.h`
3. Implement `sys_rdma_reg_mr()`
4. Test with simple user program

**Goal**: Successfully register and deregister a memory region

---

Good luck with your project! 🚀
