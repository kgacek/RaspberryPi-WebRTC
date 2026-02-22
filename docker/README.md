# Docker Build System for ArcadeRally Integration

This directory contains Docker configuration for building the ArcadeRally-integrated RaspberryPi-WebRTC application.

## Overview

The build system now uses **pre-built libwebrtc.a** from [Native-WebRTC-Build](https://github.com/TzuHuanTai/Native-WebRTC-Build/releases) for ultra-fast builds:

- **Build time**: ~5 minutes (down from 12 hours!)
- **No base image needed**: Downloads pre-compiled library
- **Same author**: TzuHuanTai maintains both repos, guaranteed compatibility
- **Version**: 5790 (M115 - same as GitHub Actions workflow)

### Legacy Build (from source)

If you need custom WebRTC flags, `Dockerfile.webrtc-base` is still available for building from source (~12h).

## Quick Start

### Build ArcadeRally (~5 minutes)

```bash
# Build application (downloads pre-built libwebrtc.a automatically)
../scripts/docker-build.sh app

# Extract binary
../scripts/docker-build.sh extract

# Binary is now at: ./pi-webrtc
```

That's it! No 12-hour wait. ⚡

## File Structure

```
docker/
├── Dockerfile.arcaderally    # Main: Build app with pre-built libwebrtc.a
├── Dockerfile.webrtc-base   # Legacy: Build libwebrtc from source (optional)
├── .dockerignore             # Files to exclude from build context
└── README.md                 # This file
```

## Usage Examples

### Build Commands

```bash
# Build application (fast!)
./scripts/docker-build.sh app

# Build without cache (force clean build)
./scripts/docker-build.sh app --no-cache

# Extract binary to current directory
./scripts/docker-build.sh extract

# Legacy: Build libwebrtc from source (12h, only if needed)
# docker buildx build --platform linux/arm64 -t webrtc-base:m115 -f docker/Dockerfile.webrtc-base .
```

### Running the Container

```bash
# Run with environment variables
docker run --rm \
  --platform linux/arm64 \
  --device /dev/video0 \
  --device /dev/ttyS0 \
  -e CF_REALTIME_APP_ID="your-app-id" \
  -e CF_REALTIME_TOKEN="your-token" \
  -e CAR_ID="car-123" \
  -e CAR_API_KEY="car_xxxxxxxxxxxxx" \
  arcaderally-rpi:latest \
    --camera=libcamera:0 \
    --fps=25 \
    --width=1280 \
    --height=720 \
    --use-cloudflare \
    --cf-app-id="$CF_REALTIME_APP_ID" \
    --cf-token="$CF_REALTIME_TOKEN" \
    --car-id="$CAR_ID" \
    --car-api-key="$CAR_API_KEY" \
    --enable-uart-control
```

### Deploy to Registry

```bash
# Push to GitHub Container Registry
export DOCKER_REGISTRY="ghcr.io/kgacek"
./scripts/docker-build.sh push

# Or manually
docker tag arcaderally-rpi:latest ghcr.io/kgacek/arcaderally-rpi:latest
docker push ghcr.io/kgacek/arcaderally-rpi:latest
```

### Pull and Run on Raspberry Pi

```bash
# On the Raspberry Pi (not macOS)
docker pull ghcr.io/kgacek/arcaderally-rpi:latest

docker run -d \
  --name arcaderally \
  --restart unless-stopped \
  --device /dev/video0 \
  --device /dev/ttyS0 \
  -e CF_REALTIME_APP_ID="..." \
  -e CF_REALTIME_TOKEN="..." \
  -e CAR_ID="..." \
  -e CAR_API_KEY="..." \
  ghcr.io/kgacek/arcaderally-rpi:latest \
    --camera=libcamera:0 \
    --fps=25 \
    --width=1280 \
    --height=720 \
    --use-cloudflare \
    --cf-app-id="$CF_REALTIME_APP_ID" \
    --cf-token="$CF_REALTIME_TOKEN" \
    --car-id="$CAR_ID" \
    --car-api-key="$CAR_API_KEY" \
    --enable-uart-control
```

## Build Times

| Method | First Build | Rebuild | Notes |
|--------|------------|---------|-------|
| **arcaderally-rpi (pre-built)** ⚡ | 5-7 min | 3-5 min | Downloads libwebrtc.a from GitHub |
| **webrtc-base (legacy)** | 10-16h | - | Only if custom WebRTC flags needed |

**Hardware**: MacBook Pro M2/M3 with 16GB RAM

### Breakdown (pre-built approach):
- Download libwebrtc.a: ~1 min
- Install dependencies: ~1 min  
- CMake configure: ~1 min
- Compile ArcadeRally: ~2-3 min
- **Total**: ~5 minutes ✅

## Troubleshooting

### Download fails from Native-WebRTC-Builder

```bash
# Manually download and provide locally
wget https://github.com/TzuHuanTai/Native-WebRTC-Builder/releases/download/m125/libwebrtc-m125-arm64-release.tar.gz

# Edit Dockerfile.arcaderally to use local file:
# COPY libwebrtc-m125-arm64-release.tar.gz /tmp/
# RUN tar -xzf /tmp/libwebrtc-m125-arm64-release.tar.gz && ...
```

### Want to use different WebRTC version

```dockerfile
# In Dockerfile.arcaderally, change release tag:
RUN wget -q https://github.com/TzuHuanTai/Native-WebRTC-Build/releases/download/OTHER_TAG/libwebrtc-arm64.tar.gz
# Check available releases: https://github.com/TzuHuanTai/Native-WebRTC-Build/releases
# Note: Use 5790 (M115) for compatibility with RaspberryPi-WebRTC main repo
```
```bash
./scripts/docker-build.sh base
```

### Out of Memory During Build

Reduce parallelism in Dockerfile.webrtc-base:
```dockerfile
RUN ninja -C out/Release64 -j1  # Use only 1 core
```

### Docker Buildx Not Available

```bash
# Install buildx plugin
brew install docker-buildx
docker buildx create --use
```

### Slow ARM64 Emulation

This is expected on macOS (using QEMU). Options:
- Build on actual Raspberry Pi (faster)
- Use GitHub Actions with ARM64 runners
- Be patient (12h is normal for webrtc-base)

### Binary Doesn't Run on Raspberry Pi

Make sure you're building for the correct platform:
```bash
docker buildx build --platform linux/arm64 ...
```

Not linux/amd64 or darwin/arm64!

## Optimization Tips

### Multi-Stage Caching

The Dockerfiles are optimized for layer caching:
- Copy `CMakeLists.txt` before source code
- Copy dependencies before application code
- Build in separate stages

### Incremental Builds

Only rebuild what changed:
```bash
# Change only C++ code → ~2 min rebuild
vim ../src/signaling/cloudflare_service.cpp
./scripts/docker-build.sh app

# Change CMakeLists.txt → ~5 min rebuild (CMake reconfigure)
vim ../CMakeLists.txt
./scripts/docker-build.sh app

# Change protocol buffer → ~3 min rebuild (protoc + recompile)
vim ../external/protocol/protos/packet.proto
./scripts/docker-build.sh app
```

### Registry Caching

Use GitHub Actions cache or registry cache:
```bash
docker buildx build \
  --cache-from type=registry,ref=ghcr.io/kgacek/webrtc-base:buildcache \
  --cache-to type=registry,ref=ghcr.io/kgacek/webrtc-base:buildcache,mode=max \
  -t webrtc-base:m115 \
  -f docker/Dockerfile.webrtc-base \
  ..
```

## Development Workflow

### Typical Development Cycle

```bash
# 1. One-time setup (12h)
./scripts/docker-build.sh base

# 2. Edit code
vim ../src/signaling/cloudflare_service.cpp

# 3. Rebuild (5 min)
./scripts/docker-build.sh app

# 4. Extract binary
./scripts/docker-build.sh extract

# 5. Test on Raspberry Pi
scp pi-webrtc pi@raspberrypi.local:/home/pi/
ssh pi@raspberrypi.local './pi-webrtc --help'

# 6. Repeat steps 2-5
```

### CI/CD Integration

See `.github/workflows/` for example GitHub Actions workflows that:
- Build base image once (cached)
- Build app on every push (fast)
- Push to registry automatically
- Create releases with artifacts

## Clean Up

```bash
# Remove all images
./scripts/docker-build.sh clean

# Remove dangling images
docker image prune -f

# Remove build cache
docker buildx prune -f
```

## See Also

- [ARCADERALLY_INTEGRATION.md](../ARCADERALLY_INTEGRATION.md) - Integration documentation
- [BUILD_WEBRTC.md](../doc/BUILD_WEBRTC.md) - Native build instructions
- [Docker Buildx Documentation](https://docs.docker.com/buildx/working-with-buildx/)
