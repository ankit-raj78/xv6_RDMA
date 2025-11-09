# Use Ubuntu 22.04 as base
FROM ubuntu:22.04

# Avoid prompts from apt
ENV DEBIAN_FRONTEND=noninteractive

# Install base packages
RUN apt-get update && apt-get install -y \
    git \
    build-essential \
    gdb-multiarch \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    python3 \
    python3-pip \
    vim \
    curl \
    wget \
    tmux \
    bc \
    perl \
    software-properties-common \
    && rm -rf /var/lib/apt/lists/*

# Add QEMU PPA for newer version and install QEMU 8.x
RUN add-apt-repository -y ppa:canonical-server/server-backports && \
    apt-get update && \
    apt-get install -y qemu-system-misc && \
    rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace/xv6_RDMA

# Set environment variables
ENV TOOLPREFIX=riscv64-linux-gnu-

# Default command
CMD ["/bin/bash"]
