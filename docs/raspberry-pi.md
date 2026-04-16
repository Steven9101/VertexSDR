# VertexSDR on Raspberry Pi and ARM

Runs on ARM Linux including Raspberry Pi, but stability depends on CPU, USB, and thermals.

## Basics

- Use a 64-bit OS image
- Add cooling to avoid throttling during long sessions
- Keep background services minimal
- Scale up gradually: one band, low rate, then increase

## SDR Input

Prefer stdin ingest from an external SDR pipeline, or USB devices with stable drivers on your kernel.

Avoid high sample rates on saturated USB buses, especially on older Pi models.

## Performance Tuning

1. Start with one band at a conservative rate
2. Add users in steps
3. Increase bandwidth only after stable operation
4. Watch thermal throttling under sustained load

## Vulkan on ARM

Only use `vkfft` if your device has a stable Vulkan stack.

```bash
make USE_VULKAN=1
```

Set `fftbackend vkfft` in config. Run extended tests before putting it in production.

## Rule

Prioritize stability over throughput. A stable lower-rate service is better than an intermittent high-rate one.
