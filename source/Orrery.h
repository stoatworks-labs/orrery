#pragma once

#include <FFGLSDK.h>

#include <vector>

#include "Controls.h"
#include "Motion.h"
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

	bool phasePinned    = false;
	float pinnedPhase   = 0.0f;

	std::vector< Instance > instances;

	/// Scratch for the uniform upload, kept as a member so that a frame does not
	/// allocate. Interleaved as Xform then Tint, kMaxInstances of each.
	std::vector< float > xformScratch;
	std::vector< float > tintScratch;
};

} // namespace orrery
