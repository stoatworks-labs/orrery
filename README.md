# Orrery

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The motion is verified
> numerically by an offline harness that drives the real plugin class in a
> headless GL context: it renders frames and measures where every shape actually
> landed against an independent prediction, across five paths and four aspect
> ratios, and separately checks each mask mode on the rendered picture (see
> [Status](#status)). It has **never been loaded into Resolume** — only compiled,
> rendered and measured offline. Check it in your own rig before trusting it in a
> show.

Primitive shapes moving on deterministic paths, for [Resolume](https://resolume.com)
Arena and Avenue, as a pair of FFGL plugins. For quick animated masks, and for
chroma animations driving a pixel map.

![Shapes on a Lissajous path, hue spread across the set](docs/hero.png)

<sub>Twelve circles on a 3:2 Lissajous, hue spread across the set. Rendered by
`ortest`, the offline harness.</sub>

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/orrery/releases/tag/v0.1.0)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`orrery-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/orrery/releases/download/v0.1.0/orrery-0.1.0-macos-universal.dmg) | 577 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`orrery-macos-universal.zip`](https://github.com/stoatworks-labs/orrery/releases/latest/download/orrery-macos-universal.zip) | 300 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`orrery-0.1.0-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/orrery/releases/download/v0.1.0/orrery-0.1.0-windows-x86_64-setup.exe) | 212 KB |
| x64 · .zip archive | [`orrery-windows-x86_64.zip`](https://github.com/stoatworks-labs/orrery/releases/latest/download/orrery-windows-x86_64.zip) | 210 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/orrery/releases](https://github.com/stoatworks-labs/orrery/releases).

<!-- downloads:end -->

Both plugins are in every download — drop them into
`~/Documents/Resolume Arena/Extra Effects` (or the Avenue equivalent) and restart
Resolume. **The macOS build is not notarised**, so the first launch needs a
right-click → Open, or a trip through System Settings → Privacy & Security; see
[docs/UNSIGNED.md](docs/UNSIGNED.md).

## Two plugins

| | |
|---|---|
| **Orrery** | A generator. Shapes over their own background. |
| **Orrery Mask** | An effect. The same shapes over — or cut into — the incoming clip. |

FFGL resolves one `plugMain` per binary, so a source and an effect are two
bundles rather than one bundle with two entries. Both ship together.

## What it is for

- **Quick animated masks.** Shapes over black, used as a luma mask, or layered
  with a blend mode, or applied directly to a clip with `Orrery Mask`.
- **Chroma animations for pixel mapping.** Point Resolume's pixel mapper at a
  composition running Orrery and the shapes become the lights. `Grid` lays shapes
  out on a fixture grid, `Hue Spread` gives every one its own colour, and `Sync`
  locks the whole chase to the track.

## Why the motion never drifts

An instance's placement is a **pure function of (index, phase)**. Nothing is
integrated frame by frame, so nothing accumulates error and nothing slows down
when the show gets heavy — which matters when the output is driving a lighting
rig rather than a screen.

The same property is why beat sync costs nothing extra: phase is just a number,
so `Sync` swaps the host clock for the host's bar position and the shapes lock to
the track with no separate code path. Set it to `Manual` and the Phase parameter
becomes the only driver — hand it to Resolume's own BPM-synced animation, a
keyframe, or a MIDI fader.

## Shapes

![The eight primitives](docs/shapes.png)

Circle, Square, Triangle, Hexagon, Star, Cross, Ring, Bar — each with
**Roundness**, **Outline**, **Stretch** and **Softness**.

`Softness` at zero gives a hard edge, which is what you want for pixel mapping: a
feathered edge means a fixture sitting on the boundary reads a half-brightness
colour that was never in the design.

`Roundness` sets corner rounding on everything except **Ring**, where it sets the
ring's thickness.

## Paths

![The five paths](docs/paths.png)

| Path | What it does |
|---|---|
| **Orbit** | A circle. With Spread up, a chase around it. |
| **Lissajous** | Two frequency ratios. Whole-number ratios close and repeat; a ratio slightly off a whole number precesses, so the pattern stays interesting for hours. |
| **Drift** | Travel at a heading, wrapping at the edges. Wipes, bars crossing a rig. |
| **Bounce** | Reflection off the frame edges. Scatter detunes the axes per shape for independent wandering. |
| **Grid** | Fixed positions on a grid. The animation is the pulse — made for pixel mapping onto a fixture layout. |

**Spread** distributes the instances along the path in phase — that is what turns
a clump into a chase. **Scatter** blends that even spread towards a hashed one,
from metronome to swarm.

## Mask modes

`Orrery Mask` adds four ways of combining the shapes with the clip:

| Mode | Result |
|---|---|
| **Over** | Shapes drawn on top, in their own colours. |
| **Reveal** | The clip shows **only** where the shapes are. |
| **Hide** | The clip shows **everywhere except** the shapes. |
| **Colourise** | The clip, tinted by the shape colour, inside the shapes. |

Colourise needs a **Colour Mode** other than White to do anything visible —
against a white shape colour it is arithmetically identical to Reveal.

## Build

```bash
git clone --recursive https://github.com/stoatworks-labs/orrery
cd orrery
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build      # straight into Resolume's plugin folder
```

Needs CMake 3.15+ and a C++17 compiler. The Resolume FFGL SDK comes in as a
submodule; on Windows, GLEW comes from vcpkg.

## Status

**Verified offline, never run in Resolume.** `tools/verify.sh` builds universal,
checks both bundles with `lipo` and `nm`, then renders real frames and measures
them:

| Check | What it proves |
|---|---|
| `ortest --motion` | 47 instances landed within 1.5 px of an independent prediction, across 5 paths and 4 aspect ratios |
| `ortest --round` | circles stay round to 0.00% and correctly sized to 0.01%, at 1:1, 16:9, portrait and 2.39:1 |
| `ortest --mask` | each of the 4 mask modes does what it says, measured inside a shape and outside it |
| `tools/sweep.py` | all 41 parameters change the picture — no dead controls |

What that does **not** cover: how the parameter groups land in Resolume's
inspector, and whether `Bar` sync locks against a real transport. Both need the
host.

## Diagnostics

A shader that will not compile looks, from the outside, exactly like a plugin
that does nothing. If that happens:

    ~/Library/Logs/orrery/orrery.YYYY-MM-DD.log

## Licence

MIT — see [LICENSE](LICENSE).

The distance functions are the standard analytic forms derived and published by
[Inigo Quilez](https://iquilezles.org/articles/distfunctions2d/); the
normalisation constants, and everything else here, are ours.
