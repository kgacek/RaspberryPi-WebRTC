#!/bin/bash
# Use pre-built Raspberry Pi OS development image from GitHub CI
# Much faster than building locally!

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Image from GitHub Container Registry
IMAGE_NAME="ghcr.io/${GITHUB_REPOSITORY_OWNER:-your-username}/raspberrypi-webrtc-dev:latest"

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

print_header() {
    echo ""
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo ""
}

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

print_info() {
    echo -e "${BLUE}ℹ️  $1${NC}"
}

show_usage() {
    cat << EOF
Usage: $0 <command>

Commands:
    pull        Download pre-built RaspiOS dev image from GitHub CI
    
    build       Compile project using the CI image (fast!)
    
    shell       Open bash shell in the dev environment
    
    info        Show image information

Examples:
    # First time - pull the image:
    $0 pull
    
    # Then build your project:
    $0 build
    
    # Or open a shell for debugging:
    $0 shell

Configuration:
    Set GITHUB_REPOSITORY_OWNER env variable if not using default repo:
    export GITHUB_REPOSITORY_OWNER="yourusername"

Current image: $IMAGE_NAME

EOF
}

cmd_pull() {
    print_header "Pulling Pre-built RaspiOS Dev Image"
    
    print_info "Pulling from: $IMAGE_NAME"
    print_info "This may take a few minutes (image is ~1-2GB)..."
    echo ""
    
    if docker pull "$IMAGE_NAME"; then
        print_success "Image pulled successfully!"
        echo ""
        print_info "You can now use: $0 build"
    else
        echo ""
        echo "❌ Failed to pull image"
        echo ""
        echo "Possible reasons:"
        echo "  1. Image not published yet (run GitHub Actions workflow first)"
        echo "  2. Need to set correct repository owner:"
        echo "     export GITHUB_REPOSITORY_OWNER=yourusername"
        echo "  3. Private repo: docker login ghcr.io first"
        exit 1
    fi
}

cmd_build() {
    print_header "Building RaspberryPi-WebRTC"
    
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "❌ Image not found: $IMAGE_NAME"
        echo ""
        print_info "Run: $0 pull"
        exit 1
    fi
    
    cd "$PROJECT_DIR"
    
    print_info "Compiling with real Raspberry Pi OS environment..."
    echo ""
    
    docker run --rm \
        --platform=linux/arm64 \
        -v "$PROJECT_DIR:/app" \
        "$IMAGE_NAME" \
        /bin/bash -c "
            cd /app &&
            rm -rf build &&
            mkdir -p build &&
            cd build &&
            cmake .. \
                -DCMAKE_CXX_COMPILER=clang++ \
                -DCMAKE_BUILD_TYPE=Release \
                -DPLATFORM=raspberrypi &&
            make -j\$(nproc)
        "
    
    echo ""
    if [ -f "$PROJECT_DIR/build/pi-webrtc" ]; then
        print_success "Build successful!"
        echo ""
        print_info "Binary: $PROJECT_DIR/build/pi-webrtc"
        print_info "Size: $(du -h "$PROJECT_DIR/build/pi-webrtc" | cut -f1)"
        echo ""
        print_info "Test: ./build/pi-webrtc --help"
    else
        echo "❌ Build failed"
        exit 1
    fi
}

cmd_shell() {
    print_header "Opening Shell in RaspiOS Environment"
    
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "❌ Image not found: $IMAGE_NAME"
        echo ""
        print_info "Run: $0 pull"
        exit 1
    fi
    
    cd "$PROJECT_DIR"
    
    print_info "Starting interactive shell..."
    echo ""
    
    docker run --rm -it \
        --platform=linux/arm64 \
        -v "$PROJECT_DIR:/app" \
        "$IMAGE_NAME" \
        /bin/bash
}

cmd_info() {
    print_header "Image Information"
    
    echo "Image: $IMAGE_NAME"
    echo ""
    
    if docker image inspect "$IMAGE_NAME" &>/dev/null; then
        print_success "Image is available locally"
        
        SIZE=$(docker image inspect "$IMAGE_NAME" --format='{{.Size}}' | awk '{print int($1/1024/1024)"MB"}')
        CREATED=$(docker image inspect "$IMAGE_NAME" --format='{{.Created}}' | cut -d'.' -f1)
        
        echo "  Size: $SIZE"
        echo "  Created: $CREATED"
        echo ""
        
        print_info "Run: $0 build"
    else
        echo "⚠️  Image not found locally"
        echo ""
        print_info "Run: $0 pull"
    fi
}

# Main
case "${1:-}" in
    pull)
        cmd_pull
        ;;
    build)
        cmd_build
        ;;
    shell)
        cmd_shell
        ;;
    info)
        cmd_info
        ;;
    help|--help|-h)
        show_usage
        ;;
    *)
        if [ -z "${1:-}" ]; then
            show_usage
        else
            echo "❌ Unknown command: $1"
            echo ""
            show_usage
            exit 1
        fi
        ;;
esac
