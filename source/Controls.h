#pragma once

#include <cstdint>

/**
    Host parameters, and what they mean.

    **Every numeric parameter Orrery declares is a plain 0..1 float**, even where
    it stands for an instance count, a heading in degrees or a seed. That is not
    a style preference. `CFFGLPluginManager::SetParamInfo` clamps an
    `FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange`
    can only be called afterwards because it finds the parameter by id -- so a
    parameter declared in degrees cannot declare a default in degrees. There is
    no `SetParamDefault`. A default of 90 becomes 1, silently, and the plugin
    starts up wrong in a way no build step notices.

    So the range lives here, in the conversion, and the host only ever sees
    0..1.

    Curves rather than straight lines wherever the useful part of a range is
    bunched at one end. Size is the clear case: for pixel mapping you are often
    placing dots a few thousandths of the frame across, and for a mask you want
    something that fills half of it. A linear slider would spend nine tenths of
    its travel between "large" and "slightly larger".
*/
namespace orrery
{
/// How the shapes are combined with each other and with what is behind them.
enum class Blend
{
	Over = 0,  ///< Ordinary alpha compositing, in instance order.
	Add,       ///< Additive. Overlaps brighten.
	Max,       ///< Channel-wise maximum. Overlaps do NOT brighten, which is what
	           ///< a mask wants: two overlapping white shapes stay white.

	Count
};

const char* BlendName( Blend blend );

/// What the effect variant does with the clip it is given. Ignored by the
/// source, which has no clip.
enum class MaskMode
{
	Over = 0,   ///< Shapes drawn on top of the clip, in their own colours.
	Reveal,     ///< The clip shows only where the shapes are.
	Hide,       ///< The clip shows everywhere except where the shapes are.
	Colourise,  ///< The clip, tinted by the shape colour, only inside the shapes.

	Count
};

const char* MaskModeName( MaskMode mode );

/// Where phase comes from.
enum class Sync
{
	Free = 0,  ///< The host clock. Speed is cycles per second.
	Beat,      ///< The host's beat. Speed is cycles per beat.
	Bar,       ///< The host's bar. Speed is cycles per bar.
	Manual,    ///< Speed is ignored; the Phase slider is the only driver, so the
	           ///< operator can key it, or let Resolume's own BPM-synced
	           ///< animation drive it.

	Count
};

const char* SyncName( Sync sync );

/**
    Parameter ids.

    The declaration order in Orrery.cpp is the order they appear in the host, and
    the groups depend on consecutive ids staying consecutive -- `SetParamGroup`
    collapses *runs* of same-group parameters, so reordering these silently
    splits a group into two.
*/
enum ParamId : unsigned int
{
	// Shape
	PT_SHAPE = 0,
	PT_INSTANCES,
	PT_SIZE,
	PT_SIZE_VARY,
	PT_STRETCH,
	PT_ROUNDNESS,
	PT_OUTLINE,
	PT_SOFTNESS,

	// Motion
	PT_PATH,
	PT_SYNC,
	PT_SPEED,
	PT_PHASE,
	PT_SPREAD,
	PT_SCATTER,
	PT_SEED,
	PT_PATH_SIZE,
	PT_CENTRE_X,
	PT_CENTRE_Y,
	PT_RATIO_X,
	PT_RATIO_Y,
	PT_DIRECTION,
	PT_GRID_COLS,
	PT_GRID_ROWS,
	PT_SPIN,
	PT_SPIN_PHASE,

	// Pulse
	PT_PULSE,
	PT_PULSE_BRIGHT,
	PT_PULSE_WIDTH,

	// Colour
	PT_COLOUR_MODE,
	PT_SHAPE_R,
	PT_SHAPE_G,
	PT_SHAPE_B,
	PT_HUE_SPREAD,
	PT_OPACITY,
	PT_BACK_R,
	PT_BACK_G,
	PT_BACK_B,
	PT_BACK_OPACITY,
	PT_BLEND,

	// Output. Both plugins declare both of these so that a composition can be
	// moved between the source and the effect without the parameter list
	// shifting underneath it; the source simply has nothing to mask against and
	// ignores them.
	PT_MASK_MODE,
	PT_MIX,

	// Preset. Declared after the real controls so their ids — which a saved
	// composition refers to — do not shift under existing users.
	PT_PRESET,

	// Audio. Appended for the same reason: these arrived after v0.1.0
	// shipped, and inserting them next to Pulse — where they belong — would
	// renumber every id after them in every saved composition. PT_AUDIO is an
	// FFT buffer (FF_TYPE_BUFFER, FF_USAGE_FFT): Resolume shows it as an
	// audio-source picker and writes one spectrum bin per element. FFGL only;
	// OFX hosts have no audio analysis and never see these.
	PT_AUDIO,
	PT_AUDIO_SIZE,
	PT_AUDIO_BRIGHT,

	PT_COUNT
};

/// Number of shapes. 1..64, quadratic -- the difference between 3 and 4 shapes
/// is a different-looking effect and the difference between 51 and 52 is
/// nothing, so a linear slider would spend most of its travel on choices nobody
/// makes.
int InstancesFromParam( float value );

/// Shape radius as a fraction of the short edge. 0.005..0.5, exponential.
float SizeFromParam( float value );

/// Long-axis stretch. 0.1..10, logarithmic, exactly 1 at the centre of the
/// slider so that "not stretched" is a place you can find by feel.
float StretchFromParam( float value );

/// Feather width as a fraction of the shape's radius. 0..0.5.
///
/// Zero matters more here than at most sliders: for pixel mapping you want a
/// hard edge, because a feathered one means a fixture at the boundary reads a
/// half-brightness colour that was never in the design.
float SoftnessFromParam( float value );

/// Cycles per unit of the sync source. 0..2, exponential, with a dead zone at
/// the bottom so that "stopped" is reachable by dragging to zero rather than by
/// luck.
float SpeedFromParam( float value );

/// The seed. 1..9999 -- an integer, so that nudging the slider gives a different
/// arrangement rather than an imperceptibly different one.
uint32_t SeedFromParam( float value );

/// Path extent in frame space. 0..1.5 -- past 1 the path carries shapes off the
/// frame, which is how you get a drift that arrives from outside rather than
/// materialising at the edge.
float PathSizeFromParam( float value );

/// Path centre in frame space. -0.25..1.25, so the centre can sit off-frame.
float CentreFromParam( float value );

/// Lissajous frequency ratio, and the bounce rate on the same axis. 0.5..8.
///
/// Deliberately continuous rather than snapped to integers: an integer ratio
/// closes the figure and repeats exactly, and a ratio slightly off an integer
/// makes it precess, which is the difference between a logo and a pattern that
/// stays interesting for an hour.
float RatioFromParam( float value );

/// Drift heading in radians. 0..2pi, 0 travelling right.
float DirectionFromParam( float value );

/// Grid columns or rows. 1..16.
int GridFromParam( float value );

/// Revolutions per cycle. -4..4, exactly 0 at the centre of the slider.
float SpinFromParam( float value );

/// The fraction of each cycle the pulse occupies. 0.02..1.
float PulseWidthFromParam( float value );

/// Hue range spanned across the instance set, in turns. 0..1.
float HueSpreadFromParam( float value );

} // namespace orrery
