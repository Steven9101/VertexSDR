# Installation and Build

Tested on Debian and Ubuntu. Other Linux distributions should work but are not regularly tested.

## Dependencies

Core (required):

```bash
sudo apt install build-essential libfftw3-dev libpng-dev libasound2-dev libssl-dev xxd
```

Vulkan (optional, for GPU FFT):

```bash
sudo apt install libvulkan-dev glslang-tools spirv-tools
```

`xxd` is only needed if you edit shader files and run `make shaders`.

## Build

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

## First Run

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
2. The `public` directory (default: `pub2/`) exists
3. The configured `tcpport` is available
4. The SDR input (ALSA device, rtl_tcp server, etc.) is reachable

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
