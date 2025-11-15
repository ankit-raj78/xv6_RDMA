#!/bin/bash
# Start Host A (sender) - listens on socket network

echo "==================================="
echo "Starting Host A (sender/initiator)"
echo "MAC: 52:54:00:12:34:56"
echo "Listening on 127.0.0.1:1234"
echo "==================================="
echo ""
echo "After both hosts start, run: rdmanet_test host_a"
echo ""

/opt/homebrew/bin/qemu-system-riscv64 \
    -machine virt -bios none -kernel kernel/kernel \
    -m 128M -smp 3 -nographic \
    -global virtio-mmio.force-legacy=false \
    -drive file=fs_host_a.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:56 \
    -netdev socket,id=net0,listen=127.0.0.1:1234
