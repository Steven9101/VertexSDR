# Sample Rates, Hardware Support, and GPU Acceleration

VertexSDR has explicit handling for specific sample rates. Rates not in this list may still start but are not guaranteed to work correctly.

## Supported Sample Rates

| Rate | Notes |
|------|-------|
| 24 kHz | Low-rate |
| 48 kHz | Low-rate |
| 59.733 kHz | Legacy compatible |
| 96 kHz | Low/mid-rate |
| 101.25 kHz | Legacy compatible |
| 116 kHz | Legacy compatible |
| 192 kHz | Common narrowband |
| 192.308 kHz | Legacy compatible |
| 200 kHz | Common narrowband |
| 224 kHz | Common narrowband |
| 250 kHz | |
| 256 kHz | For `!rtlsdr` and `!tcpsdr` |
| 384 kHz | Mid-rate |
| 384.616 kHz | Legacy compatible |
| 400 kHz | Mid-rate |
| 448 kHz | |
| 512 kHz | For `!rtlsdr` and `!tcpsdr` |
| 768 kHz | |
| 769.232 kHz | Legacy compatible |
| 800 kHz | |
| 1.024 MHz | For `!rtlsdr` and `!tcpsdr` |
| 1.536 MHz | |
| 2.048 MHz | For `!rtlsdr` and `!tcpsdr` |
| 2.88 MHz | |
| 8 MHz | High-rate |
| 20 MHz | High-rate |
| 30 MHz | High-rate |
| 60 MHz | High-rate |

Rates above 400 kHz are rounded to the nearest kHz before matching.

## Recommended Day-to-Day Rates

- 48 kHz, 192 kHz, 250 kHz
- 256 kHz, 512 kHz, 1.024 MHz, 2.048 MHz (RTL-SDR and TCP relay)
- 384 kHz, 400 kHz, 768 kHz, 800 kHz, 1.536 MHz, 2.88 MHz
- 8 MHz, 20 MHz, 30 MHz, 60 MHz

## RX888 and Wideband

RX888 is a good fit for wideband use, especially through `!tcpsdr` with Vulkan/VkFFT enabled.

Rule of thumb:
- Under 4 MHz: CPU/FFTW is fine
- Above 4 MHz: use GPU/Vulkan
- Multi-band or high listener count: GPU recommended

## GPU Acceleration

```bash
make USE_VULKAN=1
```

Set `fftbackend vkfft` in config. If the binary was built without Vulkan, it falls back to FFTW.

## RTL-SDR Example

```
band RTL
device !rtlsdr 127.0.0.1:1234
samplerate 2048000
centerfreq 10000
```

Start rtl_tcp first:

```bash
rtl_tcp -a 127.0.0.1 -p 1234 -f 10000000 -s 2048000
```

## RX888 via TCP Relay

```
band RX888
device !tcpsdr 127.0.0.1:7777
samplerate 20000000
centerfreq 10000
fftbackend vkfft
```

## Wideband stdin Pipeline

```
band Wide
device !stdin
stdinformat cs16le
samplerate 8000000
centerfreq 10000
fftbackend vkfft
```
