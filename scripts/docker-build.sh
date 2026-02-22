#!/bin/bash
# Docker build helper script for ArcadeRally integration
# This script manages the two-stage build process:
# 1. webrtc-base: Heavy WebRTC build (once, ~12h)
# 2. arcaderally-rpi: Fast app rebuilds (~5 min)

set -e

# Configuration
REGISTRY="${DOCKER_REGISTRY:-ghcr.io/kgacek}"
BASE_TAG="${WEBRTC_VERSION:-m115}"
VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo "dev")
PLATFORM="linux/arm64"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Functions
print_info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

usage() {
    cat << EOF
Usage: $0 <command> [options]

Commands:
  base        Build WebRTC base image (once, ~12h with ARM64 emulation)
  app         Build ArcadeRally application (fast, ~5-10 min)
  extract     Extract binary from built image
  push        Push images to registry
  clean       Remove local images
  help        Show this help message

Options:
  --registry  Docker registry URL (default: ghcr.io/kgacek)
  --version   Version tag for app image (default: git describe)
  --no-cache  Build without using cache
  --platform  Target platform (default: linux/arm64)

Examples:
  # First time: Build WebRTC base (leave overnight)
  $0 base

  # During development: Rebuild app after code changes
  $0 app

  # Extract binary to current directory
  $0 extract

  # Push to registry
  $0 push

Environment Variables:
  DOCKER_REGISTRY  Override default registry
  WEBRTC_VERSION   WebRTC version tag (default: m115)
EOF
}

check_docker() {
    if ! command -v docker &> /dev/null; then
        print_error "Docker is not installed"
        exit 1
    fi
    
    if ! docker buildx version &> /dev/null; then
        print_error "Docker Buildx is not available"
        exit 1
    fi
    
    print_success "Docker and Buildx are available"
}

build_base() {
    print_warning "⚠️  NOTE: Building libwebrtc from source is no longer required!"
    echo ""
    print_info "The Dockerfile.arcaderally now uses pre-built libwebrtc.a from:"
    print_info "https://github.com/TzuHuanTai/Native-WebRTC-Builder/releases"
    echo ""
    print_info "This reduces build time from 12h to ~5 minutes!"
    echo ""
    print_warning "If you still want to build from source (for custom flags):"
    echo ""
    echo "  docker buildx build \\"
    echo "    --platform $PLATFORM \\"
    echo "    -t webrtc-base:${BASE_TAG} \\"
    echo "    -f docker/Dockerfile.webrtc-base \\"
    echo "    ."
    echo ""
    print_info "Otherwise, just run: $0 app"
    exit 0
}

build_app() {
    print_info "Building ArcadeRally application..."
    print_info "Using pre-built libwebrtc.a (release 5790/M115) from Native-WebRTC-Build"
    echo ""
    
    local cache_flag=""
    if [[ "$1" == "--no-cache" ]]; then
        cache_flag="--no-cache"
        print_warning "Building without cache"
    fi
    
    print_info "Building version: $VERSION"
    
    docker buildx build \
        --platform "$PLATFORM" \
        -t "arcaderally-rpi:${VERSION}" \
        -t "arcaderally-rpi:latest" \
        -f docker/Dockerfile.arcaderally \
        $cache_flag \
        .
    
    print_success "ArcadeRally application built successfully!"
    print_info "Image tagged as: arcaderally-rpi:${VERSION}"
}

extract_binary() {
    print_info "Extracting binary from image..."
    
    if ! docker image inspect "arcaderally-rpi:latest" &> /dev/null; then
        print_error "Application image 'arcaderally-rpi:latest' not found"
        print_info "Run '$0 app' first to build the application"
        exit 1
    fi
    
    # Create temporary container
    local container_id=$(docker create --platform "$PLATFORM" arcaderally-rpi:latest)
    
    # Copy binary
    docker cp "${container_id}:/usr/local/bin/pi-webrtc" ./pi-webrtc
    
    # Remove temporary container
    docker rm "$container_id" > /dev/null
    
    # Make executable
    chmod +x ./pi-webrtc
    
    print_success "Binary extracted to: ./pi-webrtc"
    ls -lh ./pi-webrtc
}

push_images() {
    print_info "Pushing images to registry: $REGISTRY"
    
    # Tag and push base image
    if docker image inspect "webrtc-base:${BASE_TAG}" &> /dev/null; then
        print_info "Pushing webrtc-base:${BASE_TAG}..."
        docker tag "webrtc-base:${BASE_TAG}" "${REGISTRY}/webrtc-base:${BASE_TAG}"
        docker push "${REGISTRY}/webrtc-base:${BASE_TAG}"
        print_success "Pushed webrtc-base:${BASE_TAG}"
    fi
    
    # Tag and push app image
    if docker image inspect "arcaderally-rpi:${VERSION}" &> /dev/null; then
        print_info "Pushing arcaderally-rpi:${VERSION}..."
        docker tag "arcaderally-rpi:${VERSION}" "${REGISTRY}/arcaderally-rpi:${VERSION}"
        docker tag "arcaderally-rpi:latest" "${REGISTRY}/arcaderally-rpi:latest"
        docker push "${REGISTRY}/arcaderally-rpi:${VERSION}"
        docker push "${REGISTRY}/arcaderally-rpi:latest"
        print_success "Pushed arcaderally-rpi:${VERSION}"
    fi
}

clean_images() {
    print_warning "Removing local images..."
    
    docker rmi -f "webrtc-base:${BASE_TAG}" "webrtc-base:latest" 2>/dev/null || true
    docker rmi -f "arcaderally-rpi:${VERSION}" "arcaderally-rpi:latest" 2>/dev/null || true
    
    print_success "Local images removed"
}

# Parse arguments
NO_CACHE=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --registry)
            REGISTRY="$2"
            shift 2
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        --no-cache)
            NO_CACHE="--no-cache"
            shift
            ;;
        --platform)
            PLATFORM="$2"
            shift 2
            ;;
        base|app|extract|push|clean|help)
            COMMAND="$1"
            shift
            ;;
        *)
            print_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Main execution
if [[ -z "$COMMAND" ]]; then
    usage
    exit 1
fi

check_docker

case "$COMMAND" in
    base)
        build_base $NO_CACHE
        ;;
    app)
        build_app $NO_CACHE
        ;;
    extract)
        extract_binary
        ;;
    push)
        push_images
        ;;
    clean)
        clean_images
        ;;
    help)
        usage
        ;;
    *)
        print_error "Unknown command: $COMMAND"
        usage
        exit 1
        ;;
esac

print_success "Done!"
