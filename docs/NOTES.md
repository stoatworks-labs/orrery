# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

## orrery

*Orrery — animated primitive shapes as TWO FFGL plugins (source + mask effect) for Resolume; for quick animated masks and pixel-map chroma chases. PUBLIC MIT v0.1.0, all five release homes live*

**Orrery** — primitive shapes moving on deterministic paths, as **two** FFGL 2.1
plugins for Resolume: `Orrery` (FF_SOURCE, ID `OY01`) and `Orrery Mask`
(FF_EFFECT, ID `OY02`). C++17 + GLSL 4.1, CMake, `~/Projects/orrery`.

Built and released 2026-08-03. **PUBLIC MIT at `stoatworks-labs/orrery`,
now v1.0.5** (v0.2.0 Aug 3, v1.0.0 Aug 11, v1.0.1 Aug 18, v1.0.2 and v1.0.3 Aug 22,
v1.0.4 and v1.0.5 Aug 25 2026), built in CI (macOS universal .dmg + .zip, Windows .exe installer +
.zip). All five release homes agree: repo, website page
`stoatworks-labs.com/software/orrery/`, YouTube `4VJWYmGRwk8`, both embeds, and
the download block. Instagram Reel `DblB3hUDLm4`. Both bundles are also
installed into `~/Documents/Resolume Arena/Extra Effects/`.

**Two use cases drove every decision**: quick animated masks, and **chroma
animations for pixel mapping** (the composition *is* the fixture layout,
Resolume's pixel mapper samples it, the shapes are what the lights do).

**The one idea:** an instance's placement is a pure function of (index, phase) —
no simulation state at all. A bounce is a triangle wave, a drift is a wrap. So a
chase cannot drift when the frame rate drops, which matters because the output is
driving lights, and beat sync is free (phase is just a number; `Sync` swaps the
host clock for the host's bar position).

**The key divergence from [downpour](https://github.com/stoatworks-labs/downpour/blob/main/docs/NOTES.md) (`downpour`):** downpour mirrors its rain maths
into GLSL because a cell is a function of every *pixel*. Orrery's maths is per
*instance* (max 64), so it runs **once each on the CPU** and uploads two `vec4`
uniform arrays. **One copy of the maths, no mirror, no `//= mirrored` comments,
and no test whose only job is catching two copies drifting apart.** If instances
ever need to reach the thousands, the answer is a buffer texture — not a second
copy in GLSL.

8 shapes (SDFs), 5 paths (Orbit, Lissajous, Drift, Bounce, Grid), **47** parameters
(46 sweepable), 4 effect mask modes (Over / Reveal / Hide / Colourise).

**v1.0.2 (2026-08-22) closed both halves of the external bug report.** The clock
repair landed, and `phase = clock * speed` stopped teleporting every instance on a
Speed change — see [ffgl host time units](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_host_time_units.md) for the anchoring pattern
and why it is Free-sync only. It also added **Shade** and **Light** (issue #7):
the normal comes out of the distance field, so a Circle is exactly a sphere and a
Star comes out faceted with nothing special-cased. Off by default, so the null is
exact. `ortest --speed` and `--clock` both run in verify.sh now, ahead of the GL
context.

**The shading is the SECOND thing that exists three times** — the FFGL shader,
the OpenFX CPU rasteriser (`source/ofx/OrreryOFX.cpp`, which has always carried
its own `shapeDistance`) and `demo/plugin.js`. `check_shaders.py` enforces the
demo copy byte for byte; **the OpenFX copy is kept in step by hand and nothing
tests it** — verify.sh does not touch the OFX build at all.

⚠️ **New parameters must be APPENDED to `ParamId` in Controls.h**, never inserted
where they belong, or every id after them shifts in every saved composition. The
audio trio and now Shade/Light are all down at the bottom for that reason.

**No FBO anywhere, deliberately** — even Reveal and Hide, which look like they
need a mask buffer. Reveal samples the clip inside the shape fragment; Hide
punches with `glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_ALPHA )`. That sidesteps
both `FFGLFBO` bugs in [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md) entirely.

Verified by `tools/verify.sh` — universal build + `lipo` + `nm`, then
`ortest --motion` (47 instances measured off rendered frames against Motion.cpp,
5 paths × 4 aspect ratios, 1.5 px), `--round`, `--mask`, and `sweep.py`
(41 parameters, all live). **It HAS since been loaded into Resolume** — the
2026-08-04 clock commit `8784464` measured its `SetTime` live at 20.0 per frame
at 50 fps. Still unchecked there: how the parameter groups land in the inspector,
and whether Bar sync locks against a real transport.

**The harness only ever drives the clock in SECONDS**, so the milliseconds path
Resolume actually uses has no test coverage at all — which is how the v0.2.0
1000×-too-fast Speed bug ([ffgl host time units](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_host_time_units.md)) shipped and stayed
out until an external bug report in #6.

**The video is rendered, not filmed**, like porthole and old-cathode — but
unlike them nothing is stepped by `render.py`. `ortest --sequence` plays a cue
sheet (`tools/video.cues`) that lives in the plugin's own repo, so the piece is
edited next to the code, and the phase is NOT pinned so the plugin free-runs off
the host clock as it does in Resolume. One thing that cost a re-cut: **a
Lissajous is a curve and the shapes are point samples of it**, so the figure is
only legible while consecutive instances read as a line — at 3:2 even all 64
land as a scatter of dots that happen to be moving. 1:2 is the highest-order
figure that draws itself at this instance count.

Traps that cost real time are in the repo's `AGENTS.md`; the two worth knowing
outside it are in **orrery traps** (below), and the one that bit the *release*
is in [gen downloads](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_gen_downloads.md).

## orrery traps

*Two traps from building Orrery that generalise beyond it: an evenly-spread chase is phase-periodic with period 1/N (so a 0/0.5/1 sweep reports a working control dead), and iq's sdStar5 is positive inside*

Two things found building **orrery** (below) that would bite again anywhere else.

**An evenly spread chase is periodic in phase with period 1/N.** With N
instances distributed evenly around a path and all the same colour, advancing the
global phase by exactly `k/N` maps instance *i* onto where instance *i+k* was —
so the rendered frame comes back **pixel-identical**. With 14 instances, a phase
sweep of 0 → 0.5 → 1 is *two* exact symmetries, and a dead-control sweep duly
reported a perfectly working Phase slider as dead. It fails at precisely the two
values anyone reaches for first, which makes it far more convincing than a real
dead control. **Sweep cyclic parameters at irrational-looking values** (0.137,
0.611), never at 0 / ½ / 1.

The same shape of mistake applies to any test of a periodic system: a test point
that lands on a symmetry of the thing under test proves nothing, and looks like
a pass.

**Inigo Quilez's `sdStar5` returns POSITIVE inside**, unlike essentially every
other distance function on that page. Everything downstream — an outline's
`abs()`, a feather's `smoothstep`, a coverage test — assumes negative inside, so
transcribing it faithfully renders **a solid quad with a star-shaped hole in
it**. Striking, and completely wrong. Negate it. Nothing asserts this; it was
found by *looking at* a contact sheet of all the shapes, which is the argument
for generating one whenever a shape changes.

Also, from the same build: **the antialiasing width must be taken from the
distance field BEFORE an outline's `abs()`**. `abs()` creases the field at d = 0,
which is the centre line of the stroke, and `fwidth()` of a crease spikes —
putting a dark seam straight down the middle of every outline.

See also [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md).
