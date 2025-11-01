#!/bin/bash

# xv6 RDMA Docker Quick Start Script

set -e

CONTAINER_NAME="xv6-rdma-dev"

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== xv6 RDMA Docker Environment ===${NC}\n"

# Function to check if Docker is installed
check_docker() {
    if ! command -v docker &> /dev/null; then
        echo -e "${YELLOW}Docker is not installed. Please install Docker first:${NC}"
        echo "  macOS: https://docs.docker.com/desktop/mac/install/"
        echo "  Linux: https://docs.docker.com/engine/install/"
        echo "  Windows: https://docs.docker.com/desktop/windows/install/"
        exit 1
    fi
}

# Function to check if docker-compose is installed
check_docker_compose() {
    if ! command -v docker-compose &> /dev/null && ! docker compose version &> /dev/null; then
        echo -e "${YELLOW}docker-compose is not installed.${NC}"
        echo "It usually comes with Docker Desktop."
        exit 1
    fi
}

# Build the Docker image
build() {
    echo -e "${GREEN}Building Docker image...${NC}"
    docker-compose build
    echo -e "${GREEN}✓ Build complete!${NC}\n"
}

# Start the container
start() {
    echo -e "${GREEN}Starting xv6 development environment...${NC}"
    docker-compose up -d
    echo -e "${GREEN}✓ Container started!${NC}\n"
    echo -e "Run ${BLUE}./docker.sh shell${NC} to enter the container"
}

# Stop the container
stop() {
    echo -e "${YELLOW}Stopping container...${NC}"
    docker-compose down
    echo -e "${GREEN}✓ Container stopped${NC}"
}

# Enter the container shell
shell() {
    if ! docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
        echo -e "${YELLOW}Container not running. Starting it...${NC}"
        start
    fi
    echo -e "${GREEN}Entering container shell...${NC}\n"
    docker exec -it $CONTAINER_NAME /bin/bash
}

# Build xv6 inside container
build_xv6() {
    echo -e "${GREEN}Building xv6...${NC}"
    docker exec -it $CONTAINER_NAME make clean
    docker exec -it $CONTAINER_NAME make
    echo -e "${GREEN}✓ xv6 build complete!${NC}"
}

# Run xv6 in QEMU inside container
run_xv6() {
    echo -e "${GREEN}Starting xv6 in QEMU...${NC}"
    echo -e "${YELLOW}Note: Press Ctrl-A then X to exit QEMU${NC}\n"
    docker exec -it $CONTAINER_NAME make qemu
}

# Clean build artifacts
clean() {
    echo -e "${YELLOW}Cleaning build artifacts...${NC}"
    docker exec -it $CONTAINER_NAME make clean
    echo -e "${GREEN}✓ Clean complete${NC}"
}

# Show usage
usage() {
    echo "Usage: ./docker.sh [command]"
    echo ""
    echo "Commands:"
    echo "  build       - Build the Docker image"
    echo "  start       - Start the container"
    echo "  stop        - Stop the container"
    echo "  shell       - Enter container shell"
    echo "  build-xv6   - Build xv6 inside container"
    echo "  run         - Run xv6 in QEMU"
    echo "  clean       - Clean xv6 build artifacts"
    echo "  restart     - Restart the container"
    echo "  logs        - Show container logs"
    echo "  status      - Show container status"
    echo ""
    echo "Quick start:"
    echo "  ./docker.sh build"
    echo "  ./docker.sh start"
    echo "  ./docker.sh run"
}

# Restart container
restart() {
    stop
    start
}

# Show logs
logs() {
    docker-compose logs -f
}

# Show status
status() {
    echo -e "${BLUE}Container Status:${NC}"
    docker ps -a --filter "name=$CONTAINER_NAME" --format "table {{.Names}}\t{{.Status}}\t{{.Ports}}"
}

# Main script logic
check_docker
check_docker_compose

case "${1:-}" in
    build)
        build
        ;;
    start)
        start
        ;;
    stop)
        stop
        ;;
    shell)
        shell
        ;;
    build-xv6)
        build_xv6
        ;;
    run)
        run_xv6
        ;;
    clean)
        clean
        ;;
    restart)
        restart
        ;;
    logs)
        logs
        ;;
    status)
        status
        ;;
    *)
        usage
        ;;
esac
