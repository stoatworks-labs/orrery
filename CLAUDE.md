# orrery

Primitive shapes moving on deterministic paths — as **two** FFGL plugins for
Resolume Arena/Avenue: a source (`Orrery`) and an effect that masks the clip
(`Orrery Mask`). For quick animated masks, and for chroma animations driving a
pixel map. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`.
Public MIT repo.

Read `AGENTS.md` before changing the motion, the coordinate conventions or the
blend state.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install both bundles to Resolume: `cmake --install build`
- Render a frame offline: `./build/ortest --out /tmp/frame.png --phase 1.25`
- The effect over a test clip: `./build/ortest --effect --out /tmp/mask.png`
- Drive the real clock instead of pinning: `--time 2.0`
- List parameters: `./build/ortest --list`
- Contact sheets: `./build/ortest --shapes /tmp/shapes.png --paths /tmp/paths.png`
- Set anything by name: `--set "Path=1" --set "Count=0.5"`

## OpenFX build
- `source/ofx/OrreryOFX.cpp` → `build/Orrery.ofx.bundle` (target `OrreryOFX`,
  `-DBUILD_OFX=OFF` to skip): **both** plugins in one bundle —
  `com.stoatworks.orrery` (generator) and `com.stoatworks.orrerymask` (filter).
- Motion.cpp is linked straight from source (still one home). The SDFs and the
  blend rules are mirrored from the fragment shader — including the sdStar5
  negation. Change the shader's distance functions or blends, change this too.
- Sync offers Free and Manual only: OFX hosts carry no tempo. Manual is the
  mode for keyframing Phase against the edit.
- Smoke test (ofxprobe drives the Filter context; the generator's render runs
  only in a real host):
  `../resolume-ofx-bridge/build/ofxprobe --dir build --render com.stoatworks.orrerymask --size 640x360 --out /tmp/o.bmp`
- OFX SDK subset (BSD-3) vendored under `external/openfx`.
- Install for Resolve: copy the bundle into `/Library/OFX/Plugins`.

## Verify
- Everything: `tools/verify.sh`
- Where every shape landed, against `Motion.cpp`: `./build/ortest --motion`
- Circles stay round off 1:1: `./build/ortest --round`
- The four effect mask modes: `./build/ortest --mask`
- No dead controls: `python3 tools/sweep.py`

## Notes
- **An instance's placement is a pure function of (index, phase).** No simulation
  state, no feedback buffer. That is what makes it frame-rate independent,
  beat-syncable for free, resolution independent and testable a frame at a time.
- **The motion is C++ only — there is no GLSL mirror.** It runs once per
  instance, not per pixel, so there is one copy of the maths and the harness
  tests the real one.
- **Two coordinate conventions.** Paths are placed in frame space (0..1 per axis,
  y down); shapes are sized in short-edge fractions. Same number only at 1:1 —
  `--round` is the test for exactly this.
- All host parameters are 0..1 and mapped in `Controls.cpp`. `SetParamInfo`
  clamps a standard default into 0..1 before `SetParamRange` can widen it.
  **Option parameters are the exception** — they hold the element value.
- **No FBO anywhere**, including for the effect's Reveal and Hide. Sidesteps two
  SDK bugs; see `AGENTS.md`.
- `orrery_core` is an **OBJECT** library, and each plugin's registration is
  listed directly in its own target — see `AGENTS.md`.
- The GLSL declares `Xform[64]` as a literal; a `static_assert` keeps
  `kMaxInstances` in step.
- macOS build must be universal (arm64 + x86_64). Verify with `lipo`, never the
  build log.
- `flat`, `active`, `filter`, `input`, `output`, `sample`, `common` are GLSL
  reserved words. Shader errors surface only at runtime, in the diagnostics log.
- Public repo. "Commit" = commit **and** push.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume), no bundle command. It covers the failures that all look identical from
outside ("it does nothing"): a shader that will not compile, and instance uniform
arrays that did not resolve.

    ~/Library/Logs/orrery/orrery.YYYY-MM-DD.log
