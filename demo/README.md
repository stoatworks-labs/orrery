# demo/ — the browser demo

Live at **https://orrery-demo.stoatworks-labs.com**, linked from the
[project page](https://stoatworks-labs.com/software/orrery/) and from the
[video plugins page](https://stoatworks-labs.com/video-plugins/).

**This is not the plugin.** It is the GLSL from [`source/Shaders.cpp`](../source/Shaders.cpp),
copied across unedited — `#ifdef ORRERY_EFFECT` branches included — and run in
WebGL2 with the parameters the plugin's constructor declares. Both bundles are
here: the picker in the transport bar compiles the shaders twice, once each way,
exactly as the two plugins do.

## What is a port rather than a copy

`Motion.cpp` runs on the CPU in the plugin and it runs on the CPU here, so it is
ported into `plugin.js` in full, with `Hash.h`, `Shapes.cpp`'s bounds and
`Controls.cpp`'s 0..1 conversions. That is the plugin's whole design and it
survives the trip intact: **an instance's placement is a pure function of
(index, phase)**, so any frame renders on its own and Step on this page is exact
rather than approximate.

The integer hash matters as much here as there. `fract(sin(x))` differs between
machines; `lowbias32` does not — which is why the same Seed scatters the same
shapes in this page as it does in Resolume.

## Editing it

- `plugin.js` — this plugin's parameters, its shaders and the motion port.
  **When a shader in `source/Shaders.cpp` changes, change it here too**, and when
  `Motion.cpp`, `Controls.cpp` or `Shapes.cpp` changes, mirror that as well.
- `tools/check_shaders.py` — proves the four shader strings are still character
  for character what `Shaders.cpp` holds. Run by `tools/verify.sh`. Nothing
  checks the motion port; that one is on you.
- `vendor/` — the shared kit, vendored from `stoatworks-backend/resolume-demo/`.
  **Do not edit these.** Fix the master and re-run `./sync.sh`; `./sync.sh
  --check` reports drift.

## What the page cannot have

- **No host FFT.** The Audio group's spectrum is zeros, which is what Resolume
  sends with nothing routed, so Audio Size and Audio Bright do nothing.
- **No transport.** Beat and Bar run off a 120 BPM clock generated in the page —
  the tempo the plugin itself falls back to when a host reports none. Manual
  ignores the clock and is driven by the Phase slider, as in the host.
- **The clip picker is Orrery Mask's only.** The source takes no input at all.

## Deploying

From the repo root:

```bash
cf-run npx wrangler deploy
```

There is no build to run first — but check `git status` before deploying,
because a parallel session sharing this checkout can have staged its own work
into `demo/`.

Verify **by content, never by status code**. A wrong page returns a cheerful
200; only the title and the banner tell you which page is live:

```bash
curl -s 'https://orrery-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```
