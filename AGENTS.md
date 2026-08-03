# orrery — orientation for another LLM (or a newcomer)

**What it is:** primitive shapes moving on deterministic paths, as **two** FFGL
2.1 plugins for Resolume Arena/Avenue. `Orrery` is a source that draws shapes
over its own background; `Orrery Mask` is an effect that draws them over — or
cuts them into — the incoming clip. C++17 + GLSL 4.1, CMake, universal macOS
`.bundle` and a Windows `.dll`. Public, MIT, `github.com/stoatworks-labs/orrery`.

It exists for two jobs, and nearly every design decision below is downstream of
the second one:

- **Quick animated masks.** Shapes moving over black, used as a luma mask or
  layered with a blend mode.
- **Chroma animations for pixel mapping.** The composition *is* the fixture
  layout, Resolume's pixel mapper samples it, and the shapes are what the lights
  do. That is why a chase must not drift, why Grid exists, and why a hard edge is
  reachable.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the motion, the coordinate conventions, or the
blend state.

---

## The one idea

**An instance's placement is a pure function of (index, phase).**

There is no simulation state anywhere: no velocity that gets integrated, no
"previous position", no feedback texture. A bounce is a triangle wave, not a
collision test. A drift is a wrap, not an accumulation.

Four things follow, and all four are the reason it is written this way:

- **It cannot drift.** Integrate a velocity a frame at a time and the speed of
  the motion is whatever the host's frame rate happened to be — and Resolume's
  frame rate drops when the show gets heavy. For a generator whose output is
  driving a lighting rig, a chase that slows down when the projection load goes
  up is not a cosmetic problem, it is the lights coming apart from the music.
- **Beat sync is free rather than bolted on.** Phase is just a number. Give it
  the host clock and the shapes free-run; give it the host's bar phase and they
  lock, with no second code path and nothing to resynchronise.
- **Any frame renders on its own.** `ortest` renders phase 3.25 cold. Nearly
  every test depends on it.
- **Resolution independence is free rather than fought for.**

### What falls out of it

**The motion is C++ only — there is no GLSL mirror, and that is the point.**
Downpour has to carry its rain maths twice, marked `//= mirrored` on both sides,
with a test whose only job is catching the two copies drifting apart. It has to,
because a cell is a function of *every pixel*. Orrery's maths is per *instance*
and there are at most 64 of those, so it is solved once each on the CPU and
uploaded as two `vec4` uniform arrays. One copy, no mirror, no drift, and a
harness that tests the real thing.

If the instance count ever needs to reach into the thousands, *that* is when to
reconsider — and the answer would be a buffer texture, not a second copy of the
maths in GLSL.

---

## The traps

Ordered by how much time they will cost you.

**Two coordinate conventions, and mixing them up is invisible at 1:1.** Paths are
placed in **frame space** — 0..1 across the raster on each axis, y down — so a
full-size orbit sweeps the whole of a 16:9 frame instead of leaving dead bars at
the sides, which is what you want when the frame *is* the LED rig. Shapes are
sized in **short-edge fractions**, so a circle is round. Those are the same
number only on a square render. Get it wrong and a square test looks perfect
while every real output draws ellipses. `ortest --round` exists solely for this,
and it checks both the roundness *and* the size — because sizing off the wrong
edge gives a perfectly round circle of entirely the wrong diameter, which the
roundness test alone would pass.

**`sdStar5` comes out positive inside.** Every other distance function here is
negative inside, and everything downstream — the outline's `abs()`, the feather's
`smoothstep`, the coverage — assumes it. Left alone, the star renders as a solid
quad with a star-shaped hole in it: a striking picture, and completely wrong. It
is negated at the end of the function. This was caught by *looking at*
`ortest --shapes`, not by any assertion, which is why that contact sheet is
worth regenerating whenever a shape changes.

**An evenly spread chase is periodic in phase with period 1/N.** With Spread at 1
and 14 instances, advancing Phase by exactly 0.5 moves instance *i* onto where
instance *i+7* was — and since they are all the same colour, the frame comes back
**pixel-identical**. A sweep of 0, 0.5, 1 therefore reports a perfectly working
Phase slider as dead, at the two values anyone would reach for first. This cost
a genuine "is the plugin broken?" moment. `tools/sweep.py` uses 0, 0.137, 0.611,
1 and says why.

**Most parameters are supposed to do nothing in the default configuration.**
`Ratio X` is Lissajous only, `Direction` is Drift only, `Grid Columns` is Grid
only. `Roundness` and `Spin` are invisible on a **circle** — a rotated circle is
the same circle. `Seed` feeds hashes nothing consumes until `Scatter` or `Size
Variation` is up. `Colour` is ignored on Colour Mode White, and White and Solid
are identical while the swatch is still white. `Blend` needs shapes that overlap
at partial opacity — over black, three non-overlapping white shapes are white in
all three modes. Every one of these is a false failure waiting to happen, which
is what `sweep.py`'s `CONTEXT` table is for.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. There is no `SetParamDefault`. So every host parameter
here is 0..1 and the conversions live in `Controls.cpp`. A default heading of 90
degrees would silently become 1.

**Option parameters do NOT hold 0..1.** They hold the element value the operator
chose — 0, 1, 2… — so they are read through `Option()`, which rounds and clamps.
A stale composition naming an element that no longer exists is the reason for the
clamp.

**There is no FBO anywhere, on purpose.** Every mode — including the effect's
Reveal and Hide, which look like they need a mask buffer — is reachable with a
background pass and a blend function. Reveal samples the clip *inside the shape
fragment*; Hide draws the clip and punches the shapes out with
`glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_ALPHA )`. Reaching for an FBO is the
obvious move and walks straight into two SDK bugs: `FFGLFBO::Release` leaks its
colour texture, and `FFGLFBO::Initialise` allocates under a
`ScopedTextureBinding` whose destructor **clears the binding to 0 rather than
restoring it**, so allocating a buffer silently unbinds the input texture for
exactly the frames on which it was allocated. Orrery never allocates one.

**Every `ffglex::Scoped*` binding clears to 0 on scope exit — it does not
restore.** Which is why the render path uses plain `glUseProgram` and
`glBindTexture` and puts the state back by hand at the end.

**The quad is the only thing being rasterised, so `ShapeBound` is load-bearing.**
A shape that reaches past its own quad is not clipped in a way that looks like
clipping — it loses a corner, and a lost corner on a rotating square reads as the
shape *wobbling*. Erring high costs a few hundred pixels of overdraw per
instance, so the bounds are the true extents rounded up, plus margin for the
outline and the feather.

**The antialiasing width comes from the distance *before* the outline's
`abs()`.** `abs()` creases the field at d = 0, which is the centre line of the
stroke, and `fwidth()` of a crease spikes — putting a dark seam straight down the
middle of every outline.

**`FFGLShader::Set` has no integer-vector overload and reaching for the float one
is silent.** `Set( name, someInt, someInt )` against an `ivec2` resolves to the
`(float,float)` overload and issues a `glUniform2f` against an integer uniform: a
`GL_INVALID_OPERATION` that leaves the uniform at zero with nothing anywhere the
plugin can see. Every uniform here is `float`, `vec2`, `vec3`, `vec4` or a single
`int`. The instance arrays go through a raw `glUniform4fv` on a `FindUniform`
location.

**The GLSL declares `Xform[64]` as a literal**, because the shader is a plain
string. `Orrery.cpp` carries a `static_assert` that `kMaxInstances` is still 64,
so raising one without the other is a build error rather than a uniform-array
overrun.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit — giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. `orrery_core` is an **OBJECT** library, and
`SourcePlugin.cpp` / `EffectPlugin.cpp` are listed **directly** in their own
`MODULE` targets. Putting either in the shared library would register both
plugins into both bundles.

**Beat sync recovers a bar count without keeping one.** The host gives a tempo
and a position *within* the current bar, and never says which bar. A counter
would be state. So: the clock estimates how many bars have passed, `barPhase`
gives the exact position inside one, and `round( estimate − barPhase )` is the
integer that reconciles them. Continuous across the bar line, and exact even if
the clock estimate is off by up to half a bar. It can name the wrong *absolute*
bar if the transport did not start at zero, which is invisible because the
animation repeats.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that will not compile surfaces only at runtime, as
"the plugin does nothing". That is what `Diag` is for.

---

## Checking your work

`tools/verify.sh` runs the lot. The ones that matter check different things:

- **`--motion`** renders real frames and measures where every shape actually
  landed against what `Motion.cpp` said — 47 instances across all five paths and
  four aspect ratios, to within 1.5 px. It exercises the solver, the uniform
  upload, the vertex transform, the aspect correction, the distance function and
  the blend **at once**, and it catches things a mirror test structurally cannot,
  such as the instance array being uploaded off by one. It measures inside a
  window centred on the *prediction*, so a shape that is somewhere else registers
  as an empty window and fails — deliberately, because a nearest-blob search
  would assume the very thing under test.
- **`--round`** is the two-coordinate-conventions trap, above.
- **`--mask`** checks each of the four effect modes on the picture, inside a
  shape and outside it. Its reference clip is *captured* by rendering at zero
  opacity rather than predicted, because predicting it would mean
  reimplementing the UV flip in the test, and a test that reimplements what it
  tests agrees with its own mistakes. Colourise is swept against a **red** shape
  colour, because against white it is arithmetically identical to Reveal and
  would pass whether or not the multiply happened.
- **`sweep.py`** is the only thing that catches a dead control.

`ortest --shapes` and `--paths` write contact sheets. They assert nothing and are
still worth regenerating after any shape or path change — the inverted star was
found by looking at one.

**Host verification is Allan's, not an agent's.** Driving the Resolume GUI from a
session is unreliable. **Nothing in this repo has been loaded into Resolume
yet**, and the two things most worth checking there are how the parameter groups
land in the inspector and whether Bar sync actually locks against a real
transport.

---

## Things deliberately not done

- **No motion blur, no trails.** Both need history, and history is the one thing
  this design does not have. A trail would be an FBO and a feedback loop, and it
  would take the frame-rate independence with it.
- **No per-instance shape.** One shape kind per plugin instance. Mixing them is
  what a second layer is for, and it would double the branch in the fragment
  shader for something Resolume already composites better.
- **Ring is Circle plus Outline, made explicit.** It earns its place because
  `Roundness` drives its *thickness* — the one shape where that slider means
  something other than corner rounding, and the only reason it is not simply
  dead on a circle.
- **Bounce with Scatter at 0 is a coherent chase, not chaos.** All instances
  share one path and follow each other round it. That is a usable look, and
  Scatter detunes the axes per instance to get the independent wandering people
  picture when they hear "bounce".
- **`Mix` fades the shape layer rather than crossfading the whole effect.** Exact
  at both ends, slightly non-linear in between, because a true crossfade needs
  the framebuffer this plugin deliberately does not allocate.

Related: [downpour](https://github.com/stoatworks-labs/downpour) (the CMake,
harness and Diag patterns came from there), old-cathode, porthole,
resolume-luma-keyer, asciify.
