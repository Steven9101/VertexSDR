# Configuration Reference

The server reads a line-oriented `websdr.cfg` file. Empty lines are ignored. Lines starting with `#` are comments.

## File Structure

Put global settings first, then one or more band blocks.

```
maxusers 300
tcpport 8901
public pub2
fftbackend fftw
waterfallformat 9
audioformat 0
initial 14074 usb

band HF
device !stdin
samplerate 30000000
centerfreq 15000
gain -20
noniq
```

## Global Settings

### `maxusers <n>`

Maximum concurrent listeners.

### `tcpport <port>`

HTTP/WebSocket listen port.

### `public <dir>`

Primary web root directory. Usually `pub2`.

### `public2 <dir>`

Secondary public directory. Used for things like `stationinfo.txt`.

### `logdir <dir>`

Log directory.

### `chroot <dir>`

Chroot target after startup.

### `myhost <address>`

Local bind address.

### `hostname <name>`

Public hostname advertised externally. Set this if behind NAT or a reverse proxy.

### `orgserver <host>`

Directory server for registration. Defaults to `websdr.ewi.utwente.nl`. Set to `websdr.org` to register on the WebSDR directory.

VertexSDR also registers on **sdr-list.xyz** at the same time. This paired registration is automatic. You do not need separate configuration for sdr-list.xyz, and enabling the WebSDR directory path does not disable the sdr-list update path.

### `noorgserver`

Disables the legacy upstream orgserver/WebSDR socket registration path. It does not add a separate sdr-list mode; VertexSDR uses the same single registration configuration for both directories.

### `idletimeout <seconds>`

Disconnect timeout for inactive listeners.

### `logfileinterval <seconds>`

Log rotation interval. `0` means log to stderr/console only.

### `slowdownusers <n>`

Waterfall slowdown threshold. When user count exceeds this, waterfall dispatch skips frames.

### `slowdownusers2 <n>`

Stronger slowdown threshold.

### `fftplaneffort <0..3>`

FFTW planning effort:
- `0`: FFTW_ESTIMATE (fast startup)
- `1`: FFTW_MEASURE (recommended)
- `2`: FFTW_PATIENT
- `3`: FFTW_EXHAUSTIVE

Higher values slow startup but may reduce steady-state CPU.

### `fftbackend <fftw|vkfft>`

FFT backend. `fftw` for CPU, `vkfft` for Vulkan GPU. If vkfft is requested but the binary lacks Vulkan support, it falls back to FFTW.

### `waterfallformat <n>`

Waterfall stream format. `9` and `10` are known working.

### `audioformat <0..3>`

Audio compression. Higher values use less bandwidth but add more noise. `0` is best quality.

### `allowwide`

Enables wider AM/SSB bandwidth options.

### `donttrustlocalhost`

Disables localhost sysop trust.

### `dotrust <list>`

Adds trusted IP/range rules.

### `dotrustlocalnet`

Enables sysop trust for local private networks (10.x.x.x, 192.168.x.x).

### `chatboxlogfile <path>`

Chat log file path.

### `chatrejectipranges <value>`

Chat rejection range configuration.

### `rawpassword <value>`

Password for `/~~raw` IQ access. Unset means no password gate.

### `org <subkey> <value...>`

Informational metadata lines. Examples:

```
org qth JO21OC
org description Example site
org email ops@example.net
```

### `initial <freq_khz> <mode>`

Startup frequency and mode. Example: `initial 14074 usb`

If the frequency falls inside a configured band span, that band gets the startup VFO.

## Band Blocks

A band block starts with:

```
band <name>
```

All following band keys belong to that band until the next `band` line.

## Band Settings

### `device <value>`

ALSA single-device: `device hw:0,0`

ALSA multi-input: `device 0 hw:0,0` (first number is channel index)

RTL-SDR relay: `device !rtlsdr 127.0.0.1:1234`

TCP SDR relay: `device !tcpsdr 127.0.0.1:port`

stdin: `device !stdin`

### `samplerate <hz>`

Input sample rate in Hz. Not all rates are supported. See [Sample Rates and Hardware](sample-rates-and-hardware.md).

### `centerfreq <khz>`

Band center frequency in kHz.

### `audioformat <value>`

Band-local audio format string.

### `stdinformat <value>`

Input sample format for `device !stdin`.

Real formats: `s8`, `u8`, `s16le`, `u16le`, `f32le`

IQ formats: `cs8`, `cu8`, `cs16le`, `cu16le`, `cf32le`

Use real format with `noniq`. Use IQ format without `noniq`.

### `progfreq <khz>`

Programmable frequency offset for the DSP path.

### `swapiq`

Swap I/Q channels.

### `noniq`

Treat input as real-valued (not IQ).

### `gain <db>`

Input gain.

### `antenna <text>`

Antenna description.

### `noiseblanker <0|1>`

Enable/disable noise blanker.

### `extrazoom <n>`

Additional zoom scaling. Invalid values are reset to 0.

### `delay <seconds>`

Audio delay. Converted to samples at band init.

### `hpf <hz>`

High-pass / DC blocking frequency.

### `balance <file>`

Path to IQ balance correction table.

### `equalize <file>`

Path to per-frequency equalization table.

## SDR List Registration

VertexSDR registers your server on **two directories** at once: websdr.org and sdr-list.xyz. Both happen automatically from a single configuration. You do not need separate settings for each.

To enable, add:

```
orgserver websdr.org
myhost your.public.ip.or.hostname
hostname your.public.hostname.com
tcpport 8901
```

`myhost` is the address VertexSDR binds to or advertises. `hostname` is the public hostname clients and the directory servers will use. Set `hostname` if you are behind NAT or a reverse proxy.

You can also add metadata that appears in the listing:

```
org qth JO21OC
org description My HF receiver in central Europe
org email your@email.com
```

### Multi-Band Registration

When you configure multiple bands, VertexSDR registers each band as a separate receiver. Each registration includes `receiver_count` (total number of bands), `receiver_id` (band index and name), and `range_start_hz`/`range_end_hz` (the frequency span for that band). This is handled automatically. You do not need to configure anything extra per band beyond the normal `band`, `device`, `samplerate`, and `centerfreq` settings.

The `antenna` field per band also gets sent in the registration if set.

Example with two bands:

```
orgserver websdr.org
myhost your.public.hostname.com
hostname your.public.hostname.com
tcpport 8901

band HF-Low
device !rtlsdr 127.0.0.1:1234
samplerate 2048000
centerfreq 3700
gain 30
antenna Dipole

band HF-High
device !rtlsdr 127.0.0.1:1235
samplerate 2048000
centerfreq 14200
gain 30
antenna Dipole
```

This will register two receivers: one centered on 3700 kHz and one on 14200 kHz, both under the same server entry.

Make sure port forwarding is set up if behind NAT.

## Config Reload

Reload is triggered through:

```
/~~configreload?how=1   (quiet reload)
/~~configreload?how=2   (reload + force browser refresh)
/~~configreload?how=3   (clean shutdown)
```

Some changes (device type, sample rate, IQ mode) require a full restart.

## stationinfo.txt

DX/label lookup order:
1. `<public2>/stationinfo.txt`
2. `<public>/stationinfo.txt`
3. `./stationinfo.txt`

Format:

```
14100 CW Beacon
7074 FT8 Activity
17790 am Shortwave Broadcast
```
