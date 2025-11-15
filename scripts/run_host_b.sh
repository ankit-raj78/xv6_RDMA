#!/bin/bash
# Start Host B (receiver) - connects to Host A

echo "==================================="
echo "Starting Host B (receiver/target)"
echo "MAC: 52:54:00:12:34:57"
echo "Connecting to 127.0.0.1:1234"
echo "==================================="
echo ""
echo "Waiting 2 seconds for Host A to start listening..."
sleep 2
echo ""
echo "After connecting, run: rdmanet_test host_b"
echo ""

/opt/homebrew/bin/qemu-system-riscv64 \
    -machine virt -bios none -kernel kernel/kernel \
    -m 128M -smp 3 -nographic \
    -global virtio-mmio.force-legacy=false \
    -drive file=fs_host_b.img,if=none,format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:57 \
    -netdev socket,id=net0,connect=127.0.0.1:1234
