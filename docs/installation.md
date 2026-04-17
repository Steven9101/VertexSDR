# Installation and Build

Tested on Debian and Ubuntu. Other Linux distributions should work but are not regularly tested.

## All-in-One Setup

The quickest way to get started -- installs dependencies, builds, and fetches the frontend:

```bash
bash scripts/setup.sh
```

GPU build (Vulkan):

```bash
bash scripts/setup.sh --vulkan
```

This single script handles everything below. Read on if you want to do the steps manually.

## Manual Setup

### Dependencies

Core (required):

```bash
sudo apt install build-essential libfftw3-dev libpng-dev libasound2-dev libssl-dev xxd patch curl
```

Vulkan (optional, for GPU FFT):

```bash
sudo apt install libvulkan-dev glslang-tools spirv-tools
```

`xxd` is only needed if you edit shader files and run `make shaders`.

### Build

```bash
make
./vertexsdr
```

GPU build:

```bash
make USE_VULKAN=1
./vertexsdr
```

Profiling build:

```bash
make profile
make USE_VULKAN=1 profile
```

Clean:

```bash
make clean
```

The build produces a single binary: `vertexsdr`.

### Frontend Files

The WebSDR frontend files are not shipped in this repository (see `COPYING`
and [docs/frontend-setup.md](frontend-setup.md) for details). Download and
patch them before first run:

```bash
bash scripts/fetch-frontend.sh
```

This requires `curl` or `wget` and `patch`. See [docs/frontend-setup.md](frontend-setup.md)
for full details including legal notes.

### First Run

Default reads `websdr.cfg` from the current directory:

```bash
./vertexsdr
```

With explicit config path:

```bash
./vertexsdr /etc/websdr.cfg
```

Background:

```bash
nohup ./vertexsdr > vertexsdr.log 2>&1 &
```

## Checklist Before First Start

1. `websdr.cfg` exists and has at least one band configured
2. Frontend files downloaded: `bash scripts/fetch-frontend.sh`
3. The `public` directory (default: `pub2/`) exists
4. The configured `tcpport` is available
5. The SDR input (ALSA device, rtl_tcp server, etc.) is reachable

## After Starting

Check in browser:
- Main page loads at http://localhost:8901
- Waterfall appears
- Audio connects
- Band switching and tuning work

Check server-side:
- Process stays up
- Bands initialize without errors
- `pub2/tmp/bandinfo.js` was generated

## Common Issues

**"fftbackend=vkfft requested but Vulkan not enabled"**

Rebuild with `make USE_VULKAN=1` or change config to `fftbackend fftw`.

**Config file not found**

Make sure `websdr.cfg` is in the working directory, or pass the path as argument.

**Waterfall loads but no audio**

Check band device config, make sure the SDR source is actually running, check `pub2/tmp/` files were generated.

**Port 80 binding**

Port 80 requires root. Use `setcap cap_net_bind_service+ep ./vertexsdr` or run behind a reverse proxy, or use a high port like 8901.
