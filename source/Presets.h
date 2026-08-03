#pragma once

/**
    Factory presets: named motions an operator can reach in one gesture. Each
    entry is a recognisable *use* — a scanner bar for a wipe, a grid chase
    for a pixel map, drifting confetti for a background — not a random
    collection of slider positions.

    The values live in the same 0..1 parameter space both builds expose (the
    FFGL and OFX builds deliberately share it), so ONE table drives both and a
    preset looks identical in Resolume and Resolve. Plain data only; the
    application machinery lives with each host's glue. Both FFGL plugins (the
    source and the mask) share the same class, so both get the dropdown from
    this one table too.

    Element 0 of the host-facing dropdown is "Custom" and is not in this
    table: it means "the sliders are the truth".

    A preset covers shape, motion, pulse and colour. It leaves alone: Sync
    (the FFGL build offers beat modes the OFX build cannot, so an index here
    would mean different things in different hosts), Phase (the operator's
    driver, often keyed), Centre (framing), Seed (which variation, not what
    kind), Mask Mode (what the effect does to the clip is the operator's
    call), and Mix.
*/

namespace orrery
{
namespace presets
{
/// The parameters a preset sets, in one fixed order. The FFGL build binds
/// this order to its ParamIds and the OFX build to its param handles; both
/// static_assert against kParamCount so the three lists cannot drift apart
/// silently.
enum Param
{
	kShape,
	kInstances,
	kSize,
	kSizeVary,
	kStretch,
	kRoundness,
	kOutline,
	kSoftness,
	kPath,
	kSpeed,
	kSpread,
	kScatter,
	kPathSize,
	kRatioX,
	kRatioY,
	kDirection,
	kGridCols,
	kGridRows,
	kSpin,
	kSpinPhase,
	kPulse,
	kPulseBright,
	kPulseWidth,
	kColourMode,
	kShapeR,
	kShapeG,
	kShapeB,
	kHueSpread,
	kOpacity,
	kBackR,
	kBackG,
	kBackB,
	kBackOpacity,
	kBlend,
	kParamCount
};

struct Preset
{
	const char* name;
	float v[ kParamCount ];
};

// Option values are element indices: Shape 0 Circle / 4 Star / 6 Ring /
// 7 Bar; Path 0 Orbit / 1 Lissajous / 2 Drift / 3 Bounce / 4 Grid; Colour
// mode 0 White / 1 Solid / 2 Hue Spread / 3 Hue Cycle; Blend 0 Over / 1 Add /
// 2 Max. Count is the 1..64 quadratic curve (0.333 is 8 shapes), Stretch and
// Spin sit at unity/zero on 0.5.
inline constexpr Preset kPresets[] = {
	// Eight white dots around an orbit: the plugin's own defaults, named.
	{ "Orbiting Dots",
	  { /*Shape*/ 0, /*Count*/ 0.333f, /*Size*/ 0.54f, /*Vary*/ 0.0f, /*Stretch*/ 0.5f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Path*/ 0, /*Speed*/ 0.521f, /*Spread*/ 1.0f, /*Scatter*/ 0.0f,
	    /*PathSize*/ 0.467f, /*RatioX*/ 0.646f, /*RatioY*/ 0.5f, /*Dir*/ 0.0f, /*Cols*/ 0.2f, /*Rows*/ 0.1333f,
	    /*Spin*/ 0.5f, /*SpinPh*/ 0.0f, /*Pulse*/ 0.0f, /*PulseB*/ 0.0f, /*PulseW*/ 0.5f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 0 } },

	// One full-height bar bouncing across the frame: the wipe mask.
	{ "Scanner Bar",
	  { /*Shape*/ 7, /*Count*/ 0.0f, /*Size*/ 0.8f, /*Vary*/ 0.0f, /*Stretch*/ 0.5f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.15f, /*Path*/ 3, /*Speed*/ 0.45f, /*Spread*/ 1.0f, /*Scatter*/ 0.0f,
	    /*PathSize*/ 0.6f, /*RatioX*/ 0.646f, /*RatioY*/ 0.5f, /*Dir*/ 0.0f, /*Cols*/ 0.2f, /*Rows*/ 0.1333f,
	    /*Spin*/ 0.5f, /*SpinPh*/ 0.0f, /*Pulse*/ 0.0f, /*PulseB*/ 0.0f, /*PulseW*/ 0.5f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 2 } },

	// A dozen dots chasing across a grid with a brightness pulse: the pixel-map
	// workhorse.
	{ "Grid Chase",
	  { /*Shape*/ 0, /*Count*/ 0.418f, /*Size*/ 0.4f, /*Vary*/ 0.0f, /*Stretch*/ 0.5f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Path*/ 4, /*Speed*/ 0.5f, /*Spread*/ 1.0f, /*Scatter*/ 0.0f,
	    /*PathSize*/ 0.467f, /*RatioX*/ 0.646f, /*RatioY*/ 0.5f, /*Dir*/ 0.0f, /*Cols*/ 0.2f, /*Rows*/ 0.1333f,
	    /*Spin*/ 0.5f, /*SpinPh*/ 0.0f, /*Pulse*/ 0.6f, /*PulseB*/ 0.3f, /*PulseW*/ 0.3f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 2 } },

	// A single large ring holding the centre, breathing on the pulse.
	{ "Breathing Ring",
	  { /*Shape*/ 6, /*Count*/ 0.0f, /*Size*/ 0.8f, /*Vary*/ 0.0f, /*Stretch*/ 0.5f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.25f, /*Path*/ 0, /*Speed*/ 0.4f, /*Spread*/ 1.0f, /*Scatter*/ 0.0f,
	    /*PathSize*/ 0.0f, /*RatioX*/ 0.646f, /*RatioY*/ 0.5f, /*Dir*/ 0.0f, /*Cols*/ 0.2f, /*Rows*/ 0.1333f,
	    /*Spin*/ 0.5f, /*SpinPh*/ 0.0f, /*Pulse*/ 0.8f, /*PulseB*/ 0.5f, /*PulseW*/ 0.6f,
	    /*ColMode*/ 0, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 2 } },

	// Thirty-two spinning stars drifting through, scattered and rainbowed.
	{ "Star Confetti",
	  { /*Shape*/ 4, /*Count*/ 0.702f, /*Size*/ 0.35f, /*Vary*/ 0.6f, /*Stretch*/ 0.5f, /*Round*/ 0.1f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.0f, /*Path*/ 2, /*Speed*/ 0.5f, /*Spread*/ 1.0f, /*Scatter*/ 1.0f,
	    /*PathSize*/ 0.6f, /*RatioX*/ 0.646f, /*RatioY*/ 0.5f, /*Dir*/ 0.0f, /*Cols*/ 0.2f, /*Rows*/ 0.1333f,
	    /*Spin*/ 0.7f, /*SpinPh*/ 0.0f, /*Pulse*/ 0.0f, /*PulseB*/ 0.0f, /*PulseW*/ 0.5f,
	    /*ColMode*/ 2, /*RGB*/ 1.0f, 0.3f, 0.3f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 0 } },

	// A tight train of hue-cycling dots tracing a Lissajous figure.
	{ "Lissajous Trace",
	  { /*Shape*/ 0, /*Count*/ 0.488f, /*Size*/ 0.42f, /*Vary*/ 0.0f, /*Stretch*/ 0.5f, /*Round*/ 0.0f,
	    /*Outline*/ 0.0f, /*Soft*/ 0.1f, /*Path*/ 1, /*Speed*/ 0.45f, /*Spread*/ 0.25f, /*Scatter*/ 0.0f,
	    /*PathSize*/ 0.55f, /*RatioX*/ 0.646f, /*RatioY*/ 0.5f, /*Dir*/ 0.0f, /*Cols*/ 0.2f, /*Rows*/ 0.1333f,
	    /*Spin*/ 0.5f, /*SpinPh*/ 0.0f, /*Pulse*/ 0.0f, /*PulseB*/ 0.0f, /*PulseW*/ 0.5f,
	    /*ColMode*/ 3, /*RGB*/ 1.0f, 1.0f, 1.0f, /*HueSpr*/ 1.0f, /*Opacity*/ 1.0f,
	    /*Back*/ 0.0f, 0.0f, 0.0f, /*BackOp*/ 1.0f, /*Blend*/ 1 } },
};

inline constexpr int kCount = int( sizeof( kPresets ) / sizeof( kPresets[ 0 ] ) );

} // namespace presets
} // namespace orrery
