# Use Ubuntu as base image
FROM ubuntu:22.04

# Avoid prompts from apt
ENV DEBIAN_FRONTEND=noninteractive

# Install required packages
RUN apt-get update && apt-get install -y \
    git \
    build-essential \
    gdb-multiarch \
    qemu-system-misc \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    python3 \
    python3-pip \
    vim \
    curl \
    wget \
    tmux \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace/xv6_RDMA

# Set environment variables
ENV TOOLPREFIX=riscv64-linux-gnu-

# Default command
CMD ["/bin/bash"]
