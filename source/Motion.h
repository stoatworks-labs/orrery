#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Shapes.h"

/**
    Where every shape is, at a given phase.

    ## The one idea

    **An instance's placement is a pure function of (index, phase).**

    There is no simulation anywhere in Orrery: no velocity that gets integrated,
    no "previous position", no feedback texture. A bounce is a triangle wave, not
    a collision test. A drift is a wrap, not an accumulation. Ask for the frame
    at phase 91.7 and you get it without having played the preceding 91.7.

    Four things follow, and all four are why it is written this way:

    - **It cannot drift.** Integrate a velocity a frame at a time and the motion's
      speed is whatever the host's frame rate happened to be -- and Resolume's
      frame rate drops when the show gets heavy. For a generator whose whole
      purpose is driving a lighting rig, a chase that slows down when the
      projection load goes up is not a cosmetic problem; it is the thing coming
      apart from the music.
    - **Beat sync is free rather than bolted on.** Phase is just a number. Give it
      the host's clock and the shapes free-run; give it the host's bar phase
      (FFGL hands it to us in `SetBeatInfo`) and they lock to the track, with no
      second code path and nothing to resynchronise.
    - **Any frame can be rendered on its own.** `ortest` renders phase 3.25 cold.
      Nearly every test depends on that.
    - **Resolution independence is free rather than fought for.** Nothing here
      knows a pixel from a parsec.

    ## Why this is C++ and not GLSL

    Downpour mirrors its rain maths into the shader because a cell is a function
    of *every pixel* and the arithmetic has to happen per fragment. Orrery's
    maths is per *instance*, and there are at most 64 of those, so it runs once
    each on the CPU and the results are uploaded as two uniform arrays.

    That is not a micro-optimisation -- it deletes a whole class of bug. Downpour
    carries the same equations twice, marks every mirrored line, and needs a test
    whose only job is to catch the two copies drifting apart. Orrery has one
    copy, and the harness tests the real one.

    ## Two coordinate conventions, on purpose

    - **Paths are placed in frame space**: 0..1 across the raster in each axis,
      y down. So an orbit at full size sweeps the whole of a 16:9 frame rather
      than a circle in the middle with dead bars at the sides -- which is what
      you want when the frame *is* the LED rig.
    - **Shapes are sized in short-edge fractions.** So a circle is a circle.

    Mixing these up is the one geometric mistake available here, and it is
    invisible at 1:1: everything looks right on a square test render and turns
    into ellipses the moment it reaches a real output.
*/
namespace orrery
{
enum class Path
{
	Orbit = 0,
	Lissajous,
	Drift,
	Bounce,
	Grid,

	Count
};

const char* PathName( Path path );

enum class ColourMode
{
	White = 0,
	Solid,
	HueSpread,
	HueCycle,

	Count
};

const char* ColourModeName( ColourMode mode );

/**
    The most shapes that can be on screen at once.

    Bounded because the whole set is uploaded as two `vec4` uniform arrays, and
    a uniform array has to be declared at a fixed size in GLSL. 64 instances is
    128 vec4s; the GL 4.1 floor for fragment uniform components is 1024 (256
    vec4s), so this fits with room to spare on the least capable machine the
    plugin can load on at all.
*/
constexpr int kMaxInstances = 64;

/// One shape, placed. Everything the renderer needs and nothing it does not.
struct Instance
{
	float x = 0.5f;  ///< Centre in frame space, 0..1, y down.
	float y = 0.5f;
	float scale    = 0.1f;  ///< Radius as a fraction of the SHORT edge.
	float rotation = 0.0f;  ///< Radians, clockwise on screen.

	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float a = 1.0f;
};

/// The motion parameters in physical units. Controls.cpp turns the host's 0..1
/// sliders into one of these; nothing in here is a 0..1 host value except where
/// the quantity genuinely is a fraction.
struct MotionParams
{
	Path path  = Path::Orbit;
	Shape shape = Shape::Circle;
	int count  = 6;

	/// Cycles. Unbounded and monotonic -- Orrery.cpp builds it from the host
	/// clock or the host's bar phase.
	float phase = 0.0f;

	/// How far apart in phase consecutive instances sit, in cycles. 0 moves
	/// them as one body; 1 spreads them evenly around a whole cycle, which is
	/// what makes a chase.
	float spread = 1.0f;

	/// Blends the even spread towards a hashed one. 0 is a metronome, 1 is a
	/// swarm.
	float scatter = 0.0f;

	uint32_t seed = 1;

	/// Extent of the path in frame space, 0..1. 1 reaches the frame edges.
	float pathSize = 0.8f;
	float centreX  = 0.5f;
	float centreY  = 0.5f;

	/// Lissajous frequency ratios. 1,1 degenerates to a circle, which is why
	/// the phase offset between the axes is fixed at a quarter turn.
	float ratioX = 3.0f;
	float ratioY = 2.0f;

	/// Drift heading in radians. 0 travels right, increasing turns clockwise on
	/// screen because y runs down.
	float direction = 0.0f;

	int gridCols = 4;
	int gridRows = 3;

	/// Frame width / height.
	///
	/// The one thing in here that comes from the output rather than from a
	/// slider, and it is needed for exactly one job: a bounce has to turn round
	/// when the shape's *edge* reaches the frame edge, and how far a shape's
	/// edge reaches depends on which axis you measure it in. Without this a
	/// bounce on a 16:9 output turns round early at the sides and late at the
	/// top -- which reads as sloppy easing rather than as a bug.
	float aspect = 16.0f / 9.0f;

	/// Shape radius as a fraction of the short edge.
	float size     = 0.08f;
	float sizeVary = 0.0f;

	/// Revolutions **per cycle**, not per second. Everything in Orrery hangs off
	/// phase; a spin with its own independent clock would be the one thing that
	/// slid out of time when the rest is beat-locked.
	float spin      = 0.0f;
	float spinPhase = 0.0f;

	/// Pulse envelope. `pulse` scales the shape, `pulseBright` fades it, and
	/// `pulseWidth` is the fraction of the cycle the flash occupies.
	float pulse       = 0.0f;
	float pulseBright = 0.0f;
	float pulseWidth  = 0.5f;

	/// Audio. `audio[i]` is instance i's level, 0..1, already smoothed -- the
	/// FFGL side fills it from the host's FFT, one broad band per instance,
	/// low frequencies first; the OFX side leaves it dark. `audioSize` grows a
	/// shape with its band, `audioBright` fades a quiet shape towards nothing
	/// -- which is what turns a Grid of shapes into a spectrum analyser.
	float audioSize   = 0.0f;
	float audioBright = 0.0f;
	std::array< float, kMaxInstances > audio = {};

	ColourMode colourMode = ColourMode::White;
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;
	float hueSpread = 1.0f;
	float opacity   = 1.0f;
};

/// Place every instance. Clears and fills `out`.
void Solve( const MotionParams& p, std::vector< Instance >& out );

/// One instance, for the harness and for reasoning about a single shape without
/// building the whole set. `index` must be < p.count.
Instance SolveOne( const MotionParams& p, int index );

/// The phase of instance `index`, in cycles. Exposed because it is the quantity
/// the pulse and the colour both key off, and a test that wants to predict
/// either needs it.
float InstancePhase( const MotionParams& p, int index );

/// The pulse envelope: a raised cosine occupying the first `width` of each
/// cycle, and zero for the rest.
///
/// A raised cosine rather than a saw because a saw's discontinuity at the top of
/// the cycle is audible as a click on a lighting rig -- a fixture jumping from
/// black to full in one frame reads as a glitch, not a beat.
float PulseEnvelope( float cyclePhase, float width );

/// HSV to RGB, all components 0..1, hue wrapping.
void HsvToRgb( float h, float s, float v, float& r, float& g, float& b );

/// RGB to HSV. Used to take the saturation and value off the operator's colour
/// swatch so the hue modes can replace only the hue.
void RgbToHsv( float r, float g, float b, float& h, float& s, float& v );

} // namespace orrery
