# Frontend Setup

VertexSDR does not ship the WebSDR frontend files (JavaScript, HTML, images).
They are copyright Pieter-Tjerk de Boer (PA3FWM) and are not redistributed
by this project. You must download them before running VertexSDR.

## Quick Start

```bash
bash scripts/fetch-frontend.sh
```

This downloads the frontend files from community WebSDR operator repositories
and applies VertexSDR-specific patches (AM sync display, band plan overlay,
guard functions). The result is placed in `pub2/`.

## What the Script Does

1. Downloads the upstream WebSDR frontend files from
   [WeirdNewbie2/websdr](https://github.com/WeirdNewbie2/websdr)
   (primary source - visual baseline for VertexSDR's frontend appearance)
2. Falls back to [reynico/raspberry-websdr](https://github.com/reynico/raspberry-websdr),
   [FarnhamSDR/websdr](https://github.com/FarnhamSDR/websdr), or
   [ON5HB/websdr](https://github.com/ON5HB/websdr) if the primary is unreachable
3. `websdr-waterfall.js` is always fetched from reynico - that copy retains
   PA3FWM's original copyright header (WeirdNewbie2 strips it)
4. Applies patches from `patches/` to add VertexSDR-specific features

## Requirements

- `curl` or `wget`
- `patch`

## Files Downloaded vs. Files Owned by VertexSDR

### Downloaded (PA3FWM copyright, not in this repo)

| File | Notes |
|---|---|
| `websdr-base.js` | Core JS - patched with AM sync, band plan, vtx_guard |
| `websdr-waterfall.js` | HTML5 waterfall client |
| `websdr-sound.js` | HTML5 audio client - patched |
| `websdr-controls.html` | Control panel HTML - patched |
| `websdr-head.html` | Page header HTML |
| `index.html` | Main page - patched |
| `m.html` | Mobile page |
| `mobile-controls.html` | Mobile controls - patched |
| `sysop.html` | Sysop admin page |
| `websdr-1405020937.jar` | Java applet (legacy, not used by modern browsers) |
| `carrier.png`, `smeter1.png`, etc. | UI images |

### Owned by VertexSDR (generated or stored in this repo, under LGPL 3.0)

| File | Notes |
|---|---|
| `patches/*.patch` | VertexSDR modifications to upstream files |

## Frontend Parity

All WebSDR-compatible server implementations share the same PA3FWM-authored
frontend. The frontend files are identical across WebSDR operator setups
(modulo local configuration patches). VertexSDR's frontend is based on the
WeirdNewbie2 community operator version, with patches to add:

- AM synchronous detector status display
- Band plan overlay
- `__vtx_guard()` guard function

Maintaining parity with upstream WebSDR frontend versions ensures compatibility
with the full WebSDR protocol and client expectations.

---

## Legal Analysis

### What PA3FWM's License Actually Says

The WebSDR frontend files carry this notice in `websdr-base.js`:

> Copyright 2007-2014, Pieter-Tjerk de Boer, pa3fwm@websdr.org; all rights reserved.
> Permission is given to use, distribute, and modify this software only
> as part of the WebSDR package, provided proper credit is given.

And in `websdr-waterfall.js` and `websdr-sound.js`:

> Copyright 2013-2014, pa3fwm@websdr.org - all rights reserved.
> Since the intended use of this code involves sending a copy to the client
> computer, I (PA3FWM) hereby allow making it available unmodified, via my
> original WebSDR server software, to original WebSDR clients. Other use,
> including distribution in part or entirety or as part of other software,
> or reverse engineering, is not allowed without my explicit prior permission.

**Plain reading:** Strictly speaking, these files may only be distributed by the
original WebSDR server software to original WebSDR clients. Use in any other
software - including VertexSDR - is not allowed without PA3FWM's explicit
prior permission.

### The Practical Reality in Amateur Radio

Dozens of amateur radio operators have published these files publicly on GitHub,
in blog posts, and in WebSDR setup guides without issue. PA3FWM has not pursued
any enforcement actions against the amateur radio community for sharing these
files. The amateur radio community operates on a cooperative, good-faith basis,
and PA3FWM himself has made the WebSDR software freely available for amateur
radio use.

Technically, everyone distributing these files on GitHub (reynico, FarnhamSDR,
ON5HB, WeirdNewbie2, and dozens of others) is operating in a legally gray area
under the strict letter of the license. Nobody cares - it is universally accepted
within the amateur radio community as standard practice for setting up WebSDR
instances.

**This does not mean there is no legal risk.** If PA3FWM or a successor ever
chose to enforce the license terms, those redistributors could be liable. But in
practice, this has never happened, and the amateur radio community's cooperative
norms make it unlikely.

### VertexSDR's Approach: The Most Defensible Position

VertexSDR takes the most legally cautious approach available:

1. **Zero files shipped.** The PA3FWM-authored files are not present in this
   repository, are not in any release archive, and are not in git history.
   VertexSDR does not distribute them.

2. **Users download independently.** The `fetch-frontend.sh` script is a
   convenience tool. It fetches files from third-party repositories. Legal
   liability for redistribution of those files lies with those third-party
   repositories, not with VertexSDR.

3. **Patches are VertexSDR's own work.** The `patches/*.patch` files contain
   only the VertexSDR-specific additions (unified diff format). They do not
   contain the upstream files' content and do not constitute redistribution.
   Patches are unambiguously VertexSDR's original work, licensed under LGPL 3.0.

4. **Copyright notice preserved.** The `websdr-waterfall.js` fetch uses the
   reynico source rather than WeirdNewbie2 because reynico's copy retains
   PA3FWM's in-file copyright header. VertexSDR does not assist in stripping
   attribution.

This approach is analogous to how many open-source projects handle proprietary
firmware, binary blobs, or licensed assets - provide a script to fetch them
from their legitimate sources rather than bundling them.

### Why This Is Better Than Alternatives

| Approach | Legal Risk |
|---|---|
| Ship PA3FWM files in repo | High - direct redistribution contrary to license |
| Ship modified PA3FWM files | High - derivative work without permission |
| Write replacement from scratch | Zero - but difficult and time-consuming |
| Fetch + patch at install time (VertexSDR's approach) | Low - no redistribution by VertexSDR |

### Source Repositories Used

These are the community WebSDR operator repos used as download sources.
None claim to hold the original copyright - they are operator configuration
shares of the WebSDR software package.

| Repo | Stars | Description |
|---|---|---|
| [WeirdNewbie2/websdr](https://github.com/WeirdNewbie2/websdr) | - | WebSDR operator config (visual baseline) |
| [reynico/raspberry-websdr](https://github.com/reynico/raspberry-websdr) | 139+ | Raspberry Pi WebSDR setup guide |
| [FarnhamSDR/websdr](https://github.com/FarnhamSDR/websdr) | 36+ | Farnham Amateur Radio Club WebSDR |
| [ON5HB/websdr](https://github.com/ON5HB/websdr) | 11+ | ON5HB WebSDR config files |

### Summary

- VertexSDR is as legally clean as it can be while using PA3FWM's frontend
- No PA3FWM files are shipped, distributed, or present in git history
- The community WebSDR ecosystem operates on good-faith norms that tolerate
  the widespread sharing of these files
- Users who want a fully license-compliant setup should obtain permission
  directly from PA3FWM (pa3fwm@websdr.org)

---

## Updating the Patches

If the upstream files change and the patches no longer apply cleanly,
regenerate them against the WeirdNewbie2 source:

```bash
# Download fresh WeirdNewbie2 versions
WN2_BASE="https://raw.githubusercontent.com/WeirdNewbie2/websdr/main/dist/pub2"
TMPDIR=$(mktemp -d)
for f in websdr-base.js websdr-sound.js websdr-controls.html websdr-head.html \
          index.html m.html mobile-controls.html; do
  curl -fsSL "$WN2_BASE/$f" -o "$TMPDIR/$f"
done

# Regenerate patches against your modified pub2/ versions
for f in websdr-base.js websdr-sound.js websdr-controls.html websdr-head.html \
          index.html m.html mobile-controls.html; do
  diff -u "$TMPDIR/$f" "pub2/$f" > "patches/${f}.patch" || true
done
```
