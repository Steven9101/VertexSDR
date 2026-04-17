# stdin Pipelines

VertexSDR can read from stdin instead of opening hardware directly. This lets you use any external SDR tool as the frontend.

## Limits

- Not all sample rates work. Use rates from the supported list.
- Match `stdinformat` to the actual bytes from the external tool.
- Use `noniq` only for real-valued streams.

See [Sample Rates and Hardware](sample-rates-and-hardware.md).

## Supported Formats

Real: `s8`, `u8`, `s16le`, `u16le`, `f32le`

IQ: `cs8`, `cu8`, `cs16le`, `cu16le`, `cf32le`

## RTL-SDR via rtl_sdr

`rtl_sdr` outputs unsigned 8-bit IQ, so use `stdinformat cu8`.

Config:

```
band HF
device !stdin
stdinformat cu8
samplerate 2048000
centerfreq 10100
gain 30
```

Run:

```bash
rtl_sdr -f 10100000 -s 2048000 -g 30 - | ./vertexsdr
```

## rx_tools (rx_sdr)

`rx_sdr` from the rx_tools package emits raw IQ samples.

Config:

```
band Wide
device !stdin
stdinformat cu8
samplerate 2048000
centerfreq 10100
gain 0
```

Run:

```bash
rx_sdr -f 10100000 -s 2048000 - | ./vertexsdr
```

Adjust rx_sdr flags for your hardware.

Note: `rx_fm` and `rx_power` are demodulator/spectrum tools and are not a good fit for raw RF ingest into VertexSDR. Use `rx_sdr` instead.

## Signed 16-bit IQ

Config:

```
band IQ16
device !stdin
stdinformat cs16le
samplerate 8000000
centerfreq 10000
fftbackend vkfft
```

Run:

```bash
some_iq_source | ./vertexsdr
```

## Real-Valued Input

Enable `noniq` for real-valued streams.

Config:

```
band RealIn
device !stdin
stdinformat s16le
samplerate 192000
centerfreq 1000
noniq
```

Run:

```bash
some_real_source | ./vertexsdr
```
