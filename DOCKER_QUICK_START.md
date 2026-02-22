# Docker Quick Start Guide

**TL;DR**: Fastest way to build ArcadeRally for Raspberry Pi on your Mac.

## 🚀 Quick Commands

```bash
# 1️⃣ Build ArcadeRally app (5 min - uses pre-built libwebrtc.a!)
./scripts/docker-build.sh app

# 2️⃣ Extract binary
./scripts/docker-build.sh extract

# 3️⃣ Copy to Raspberry Pi
scp pi-webrtc pi@raspberrypi.local:/home/pi/
```

**⚡ That's it!** No 12-hour WebRTC build needed - we use pre-built binaries from [Native-WebRTC-Build](https://github.com/TzuHuanTai/Native-WebRTC-Build/releases) (release 5790/M115)!

## 📋 Prerequisites

### On macOS (your dev machine):

```bash
# Install Docker Desktop
brew install --cask docker

# Enable multi-architecture builds
docker buildx create --use
docker buildx inspect --bootstrap
```

### On Raspberry Pi (target device):

```bash
# Basic runtime dependencies (if not using Docker)
sudo apt update
sudo apt install libboost-system1.74.0 libboost-thread1.74.0 \
    libcurl4 libasound2 libv4l-0

# Or just use Docker on RPi too!
```

## 🏗️ Build Workflow

### Step 1: Build ArcadeRally App (Fast!)

The Dockerfile automatically downloads pre-built libwebrtc.a (m125) from [Native-WebRTC-Builder](https://github.com/TzuHuanTai/Native-WebRTC-Builder). **Takes only ~5 minutes!**

```bash
cd /path/to/RaspberryPi-WebRTC

# Build the application
./scripts/docker-build.sh app
```

**Expected output:**
```
🚀 Building ArcadeRally application...
ℹ️  Using pre-built libwebrtc.a (5790/M115) from Native-WebRTC-Build

Building version: abc123f
...
✅ ArcadeRally application built successfully!
ℹ️  Image tagged as: arcaderally-rpi:abc123f
✅ Done!
```

### Step 2: Extract Binary

Get the compiled executable:

```bash
./scripts/docker-build.sh extract
```

This creates `./pi-webrtc` file (ARM64 Linux binary).

### Step 4: Deploy to Raspberry Pi

#### Option A: Direct Copy (simplest)

```bash
# Copy binary
scp pi-webrtc pi@raspberrypi.local:/home/pi/

# SSH and run
ssh pi@raspberrypi.local
chmod +x /home/pi/pi-webrtc

./pi-webrtc \
  --camera=libcamera:0 \
  --fps=25 \
  --width=1280 \
  --height=720 \
  --use-cloudflare \
  --cf-app-id="your-app-id" \
  --cf-token="your-token" \
  --car-id="car-123" \
  --car-api-key="car_xxx" \
  --enable-uart-control
```

#### Option B: Docker on Raspberry Pi (recommended)

```bash
# On Mac: Push to registry
docker tag arcaderally-rpi:latest ghcr.io/your-username/arcaderally-rpi:latest
docker push ghcr.io/your-username/arcaderally-rpi:latest

# On RPi: Pull and run
ssh pi@raspberrypi.local
docker pull ghcr.io/your-username/arcaderally-rpi:latest

docker run -d \
  --name arcaderally \
  --device /dev/video0 \
  --device /dev/ttyS0 \
  -e CF_REALTIME_APP_ID="your-app-id" \
  -e CF_REALTIME_TOKEN="your-token" \
  ghcr.io/your-username/arcaderally-rpi:latest
```

#### Option C: Docker Compose (easiest)

```bash
# On Raspberry Pi:
# 1. Copy docker-compose.yml and .env.example
scp docker-compose.yml .env.example pi@raspberrypi.local:/home/pi/

# 2. Configure
ssh pi@raspberrypi.local
cd /home/pi
cp .env.example .env
nano .env  # Fill in your credentials

# 3. Run
docker-compose up -d

# 4. Check logs
docker-compose logs -f
```

## 🔄 Development Workflow

### Typical Day:

```bash
# Morning: Make some code changes
vim src/signaling/cloudflare_service.cpp

# Rebuild (5 min)
./scripts/docker-build.sh app

# Extract and test
./scripts/docker-build.sh extract
scp pi-webrtc pi@raspberrypi.local:/home/pi/

# See it work!
ssh pi@raspberrypi.local ./pi-webrtc --help
```

### Testing Multiple Changes:

```bash
# Change 1: Update UART protocol
vim src/common/uart_controller.cpp
./scripts/docker-build.sh app
./scripts/docker-build.sh extract
scp pi-webrtc pi@raspberrypi.local:/home/pi/test-v1

# Change 2: Update Cloudflare logic
vim src/signaling/cloudflare_service.cpp
./scripts/docker-build.sh app
./scripts/docker-build.sh extract
scp pi-webrtc pi@raspberrypi.local:/home/pi/test-v2

# Test both versions on RPi
```

## 📊 Build Time Comparison

| Method | First Build | Rebuild | Cache | Complexity |
|--------|------------|---------|-------|------------|
| **Native on RPi** | 6h | 6h | ❌ | Low |
| **Docker (old - build from source)** | 12h | 5 min | ✅ | Medium |
| **Docker (new - pre-built)** ✅ | **5 min** | **5 min** | ✅ | Low |
| **GitHub Actions** | 6h | 6h | ⚠️ | High |

**Winner**: Docker with pre-built libwebrtc.a ⚡ (no 12h wait!)

## 🐛 Troubleshooting

### Download fails during build

If GitHub releases download times out:
```bash
# Manually download and use local file
wget https://github.com/TzuHuanTai/Native-WebRTC-Builder/releases/download/m125/libwebrtc-m125-arm64-release.tar.gz

# Edit Dockerfile.arcaderally to use COPY instead of wget
# COPY libwebrtc-m125-arm64-release.tar.gz /tmp/
```

### Build is very slow

ARM64 emulation on macOS uses QEMU. Expected times:
- **Download libwebrtc.a**: ~1 min  
- **CMake configure**: ~1 min
- **Compile ArcadeRally code**: ~3-4 min
- **Total**: ~5 minutes

If it takes longer, check Docker Desktop resources (Settings → Resources).

### Out of memory

Reduce parallelism in [docker/Dockerfile.webrtc-base](docker/Dockerfile.webrtc-base):
```dockerfile
RUN ninja -C out/Release64 -j1  # Use only 1 core instead of 2
```

### Binary doesn't run on Raspberry Pi

Check the platform:
```bash
file ./pi-webrtc
# Should show: ELF 64-bit LSB executable, ARM aarch64, version 1 (GNU/Linux)
```

If it shows x86_64 or darwin, you built for wrong platform:
```bash
./scripts/docker-build.sh app --platform linux/arm64
```

### Docker Desktop not responding

```bash
# Restart Docker
killall Docker && open -a Docker

# Or restart from UI
```

## 🎯 Pro Tips

### 1. Fast iteration cycle

```bash
# Edit code, build, test - all in minutes!
vim src/signaling/cloudflare_service.cpp
./scripts/docker-build.sh app
./scripts/docker-build.sh extract
scp pi-webrtc pi@raspberrypi.local:/home/pi/
```

### 2. Use different libwebrtc versions

Change version in Dockerfile.arcaderally:
```dockerfile
# 5790 (M115) is recommended for compatibility
# Check releases: https://github.com/TzuHuanTai/Native-WebRTC-Build/releases
RUN wget -q https://github.com/TzuHuanTai/Native-WebRTC-Build/releases/download/5790/libwebrtc-arm64.tar.gz
```

### 3. Keep multiple versions

```bash
docker tag arcaderally-rpi:latest arcaderally-rpi:stable
# Now you have a backup before experiments
```

### 4. Script your deployment

Create `deploy.sh`:
```bash
#!/bin/bash
./scripts/docker-build.sh app
./scripts/docker-build.sh extract
scp pi-webrtc pi@raspberrypi.local:/home/pi/pi-webrtc-$(date +%Y%m%d-%H%M%S)
```

### 5. Test without Raspberry Pi

```bash
# Run in QEMU on Mac (slow but works)
docker run --rm --platform linux/arm64 arcaderally-rpi:latest --help
```

## 📚 More Information

- [docker/README.md](docker/README.md) - Detailed Docker documentation
- [ARCADERALLY_INTEGRATION.md](ARCADERALLY_INTEGRATION.md) - Integration guide
- [doc/BUILD_WEBRTC.md](doc/BUILD_WEBRTC.md) - Native build instructions

## 🆘 Need Help?

1. Check [docker/README.md](docker/README.md) for detailed troubleshooting
2. Read error messages carefully (they're usually accurate)
3. Check Docker Desktop is running and has enough resources:
   - Settings → Resources → Memory: ≥8GB recommended
   - Settings → Resources → Disk: ≥60GB free

## 🎉 Success Checklist

- ✅ Docker Desktop installed and running
- ✅ Buildx enabled (`docker buildx ls` shows builder)
- ✅ App image built (`docker images | grep arcaderally-rpi`)
- ✅ Binary extracted (`ls -lh pi-webrtc`)
- ✅ Binary copied to RPi (`ssh pi@raspberrypi.local ls -lh /home/pi/pi-webrtc`)
- ✅ App running on RPi! 🚗💨

---

**Remember**: With pre-built libwebrtc.a, every build is just ~5 minutes! ⚡ No more 12-hour waits!
