# Band Switching and bandinfo.js

The frontend switches bands using per-band state from `pub*/tmp/bandinfo.js`.

## Required Fields

Each band entry in `bandinfo` includes:
- `centerfreq` (kHz)
- `samplerate` (kHz)
- `tuningstep` (kHz/bin)
- `maxlinbw` (kHz)
- `vfo` (per-band startup frequency in kHz)
- `maxzoom`
- `name`
- `scaleimgs`

## VFO Behavior

`vfo` is per band, not global.

At startup:
- Every band starts with `vfo = centerfreq`
- If `initial` is inside one band's span (with a 4 kHz margin), that band's `vfo` is set to `initial`

This keeps band switching stable. Switching to another band uses that band's stored VFO.

## Runtime Switch

When a client switches band through `~~param`/`~~waterparam`:
- Client is moved between per-band audio counters
- `band_ptr` and `band_idx_req` update immediately
- Per-client audio codec/demod state resets to avoid stale filter state from the previous band
