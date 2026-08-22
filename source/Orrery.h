#pragma once

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

#include <vector>

#include "Controls.h"
#include "Motion.h"
#include "Presets.h"
#include "Shapes.h"

/**
    Orrery -- primitive shapes moving on deterministic paths, for Resolume.

    What is worth knowing about how it works is in two other files and not
    repeated here:

    - **Motion.h** -- an instance's placement is a pure function of (index,
      phase). No simulation state, so nothing drifts with the frame rate, beat
      sync costs nothing extra, and any frame can be rendered on its own.
    - **Shaders.h** -- two passes and no framebuffer, and why instanced quads
      rather than one fullscreen pass over all the distance functions.

    This class is the part that talks to the host: it declares the parameters,
    turns them into a `MotionParams`, works out what phase the host is asking
    for, and draws.

    Both plugins are this class. The source draws shapes over its own
    background; the effect draws them over, or into, the incoming clip. They
    differ by a constructor flag, a `#define` handed to the shader compiler, and
    their input count -- little enough that keeping them as one class is what
    stops them drifting apart.

    See AGENTS.md for the traps.
*/
namespace orrery
{
class OrreryPlugin : public CFFGLPlugin
{
public:
	explicit OrreryPlugin( bool overInput );

	// CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters on
	/// a fresh instance and deletes the instance if one fails, and
	/// CFFGLPlugin's SetTextParameter is a stub that returns exactly that
	/// failure -- so without this override no real host can load the plugin,
	/// while every offline harness here carries on passing.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Render one frame into whatever is currently bound, at `width` x `height`.
	///
	/// Exposed for the offline harness, which drives this class rather than a
	/// copy of it -- a test that exercises a reimplementation tests the
	/// reimplementation.
	void Render( int width, int height, GLuint inputTexture, float maxU, float maxV );

	/// The motion parameters as they would be resolved at this size, for the
	/// harness to predict where each shape should land.
	MotionParams CurrentMotion( int width, int height ) const;

	/// The instances placed by the last Render.
	const std::vector< Instance >& LastInstances() const { return instances; }

	/// Pin the driven phase, ignoring the host clock and the beat.
	///
	/// The harness needs this because a test that renders "the frame at phase
	/// 3.25" and then predicts where the shapes are has to be certain that both
	/// halves used the same 3.25 -- and `hostTime` is a double that arrives from
	/// outside.
	///
	/// The Phase parameter is still added on top, so pinning does not make that
	/// slider look dead to a sweep.
	void SetPhaseOverride( float phase );

private:
	/// The ParamId each presets::Param drives, in presets::Param order. The
	/// preset table stays host-agnostic; this is the FFGL binding of it.
	static constexpr unsigned int kPresetParamIDs[ presets::kParamCount ] = {
		PT_SHAPE, PT_INSTANCES, PT_SIZE, PT_SIZE_VARY, PT_STRETCH, PT_ROUNDNESS, PT_OUTLINE, PT_SOFTNESS,
		PT_PATH, PT_SPEED, PT_SPREAD, PT_SCATTER, PT_PATH_SIZE, PT_RATIO_X, PT_RATIO_Y, PT_DIRECTION,
		PT_GRID_COLS, PT_GRID_ROWS, PT_SPIN, PT_SPIN_PHASE, PT_PULSE, PT_PULSE_BRIGHT, PT_PULSE_WIDTH,
		PT_COLOUR_MODE, PT_SHAPE_R, PT_SHAPE_G, PT_SHAPE_B, PT_HUE_SPREAD, PT_OPACITY,
		PT_BACK_R, PT_BACK_G, PT_BACK_B, PT_BACK_OPACITY, PT_BLEND
	};

	/// Copy a factory preset's values into params[] and raise value events so
	/// the host re-reads the sliders. `presetIndex` is 1-based; 0 is Custom.
	void applyPreset( int presetIndex );

	bool BuildShaders();

	/// Phase, in cycles, for the current parameters and host state.
	float CurrentPhase() const;

	const bool overInput;

	ffglex::FFGLShader backgroundShader;
	ffglex::FFGLShader shapeShader;

	/// A core profile refuses to draw with no vertex array bound, even when the
	/// vertex shader sources nothing and builds its quad from `gl_VertexID`.
	/// This is that array: created empty, bound to draw, never filled.
	GLuint emptyVAO = 0;

	float params[ PT_COUNT ] = {};

	//---------------------------------------------------------------------
	// Host clock units.
	//
	// The FFGL header never says what unit SetTime is in, and hosts disagree:
	// Resolume hands over MILLISECONDS (measured live: 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. UpdateClock decides from the first
	// plausible frame delta and sticks: 0.001..0.5 is a seconds-host frame,
	// 2..500 is a milliseconds-host frame, anything else keeps waiting.
	// `hostSeconds` is the normalised clock CurrentPhase reads.
	//---------------------------------------------------------------------
	void UpdateClock();

	//---------------------------------------------------------------------
	// Phase continuity across a Speed change.
	//
	// Placement stays a pure function of (index, phase) -- that is the whole
	// design and none of it changes here. What changes is only which phase a
	// given clock reading maps to.
	//
	// `phase = clock * speed` means a speed change moves the phase by
	// `clock * delta`, and `clock` is however long the composition has been
	// open. Nudging Speed an hour in is a jump of hundreds of cycles: every
	// shape teleports, which is what issue #6 reported after the 1000x bug was
	// out of the way. So remember the phase reached so far and count from
	// there at the new rate.
	//
	// Free only. Beat and Bar deliberately keep jumping: their contract is
	// that phase 0 lands on the bar line, and an offset that made a speed
	// change seamless would slide the animation off the grid it exists to sit
	// on. Nor is any of this in the OpenFX build -- that host renders
	// arbitrary times in arbitrary order and can keyframe Speed, so a running
	// anchor there would make a frame depend on which frames were rendered
	// before it.
	//---------------------------------------------------------------------
	void UpdatePhaseAnchor();

	double phaseAnchor = 0.0; ///< phase already reached at `anchorClock`
	double anchorClock = 0.0; ///< the clock reading that phase belongs to
	float anchorSpeed  = -1.0f;///< speed in force since then; < 0 until the first frame

public:
	FFResult SetTime( double time ) override;

	//---------------------------------------------------------------------
	// Clock test hooks.
	//
	// The offline harness DECLARES its unit rather than leaving UpdateClock to
	// infer one. A single absolute time handed over in one frame is genuinely
	// ambiguous -- 2.0 is two seconds or two milliseconds and nothing in it
	// says which -- so inference is only possible against a live host's frame
	// deltas. An implicit unit is what caused the bug these exist to keep
	// fixed (issue #6).
	//---------------------------------------------------------------------
	void SetClockScaleForTest( double scale );
	void TickClockForTest();
	double ClockScaleForTest() const;
	double HostSecondsForTest() const;

	/// The phase the next frame would be drawn at. `--speed` needs it: the
	/// thing being tested is that a speed change does NOT move the picture,
	/// and reading the phase either side of one says so directly, where a
	/// rendered-frame comparison would only say the two frames match.
	float CurrentPhaseForTest() const;

private:

	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double lastRawTime  = -1.0;
	double lastWallTime = -1.0;
	double wallStart    = -1.0;
	double hostSeconds  = 0.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	bool hostTimeSeen   = false;

	//---------------------------------------------------------------------
	// Audio.
	//
	// The host writes one spectrum bin per element of PT_AUDIO; UpdateAudio
	// runs them through an attack/release filter into `audioLevel`, and
	// CurrentMotion resamples that per instance. The smoothing lives here and
	// not in Motion because Motion is a pure function of its parameters --
	// state would cost it that, and everything the harness proves about it.
	//---------------------------------------------------------------------
	void UpdateAudio();

	std::array< float, kMaxInstances > audioLevel = {};
	double audioClock = -1.0;

	bool phasePinned    = false;
	float pinnedPhase   = 0.0f;

	std::vector< Instance > instances;

	/// Scratch for the uniform upload, kept as a member so that a frame does not
	/// allocate. Interleaved as Xform then Tint, kMaxInstances of each.
	std::vector< float > xformScratch;
	std::vector< float > tintScratch;
};

} // namespace orrery
