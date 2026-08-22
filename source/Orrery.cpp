#include "Orrery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "Diag.h"
#include "Shaders.h"

namespace orrery
{
namespace
{
/// The GLSL declares `Xform[64]` as a literal, because the shader is a plain
/// string. Raising the C++ constant without raising the GLSL one would overrun
/// the uniform array, so make it a build error instead of a rendering one.
static_assert( kMaxInstances == 64, "Shaders.cpp declares Xform[64] and Tint[64] -- keep them in step" );

/// Read an option parameter. Option parameters do NOT hold 0..1: the host stores
/// the element value the operator chose, so this is an index and clamping it is
/// the only thing standing between a stale composition and an out-of-range enum.
int Option( float value, int count )
{
	const int i = static_cast< int >( std::lround( value ) );
	return std::min( count - 1, std::max( 0, i ) );
}

float Clamp01( float v )
{
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

/// Insert defines after the `#version` line, which must be first in a GLSL
/// source.
std::string WithDefines( const char* shader, const char* defines )
{
	std::string source( shader );
	if( defines == nullptr || *defines == '\0' )
		return source;

	const size_t afterVersion = source.find( '\n' );
	if( afterVersion != std::string::npos )
		source.insert( afterVersion + 1, defines );

	return source;
}

void ApplyBlend( Blend blend )
{
	glEnable( GL_BLEND );

	switch( blend )
	{
	case Blend::Add:
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE );
		break;

	case Blend::Max:
		// GL_MAX ignores the factors entirely and takes the channel-wise maximum
		// of source and destination, alpha included. That is what a mask wants:
		// two overlapping white shapes stay white instead of clipping to a
		// brighter white that is no longer the same colour as either of them.
		glBlendEquation( GL_MAX );
		glBlendFunc( GL_ONE, GL_ONE );
		break;

	case Blend::Over:
	default:
		// Premultiplied over, matching the shader's output and the rest of the
		// fleet.
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
		break;
	}
}


/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, to calibrate the host's against. Steady rather than system, so
/// nothing here moves if the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}
} // namespace

// The buttons are declared one per link, so the run in the enum and the run the
// block actually has must agree. They diverge the day somebody writes a user
// guide, and this is what says so.
static_assert( PT_COUNT - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About run no longer matches StoatworksAbout.h -- "
               "add or remove a PT_ABOUT_BUTTON_n to match" );

OrreryPlugin::OrreryPlugin( bool overInput ) :
	overInput( overInput )
{
	// The source has no input; the effect takes one.
	SetMinInputs( overInput ? 1 : 0 );
	SetMaxInputs( overInput ? 1 : 0 );

	//-----------------------------------------------------------------------
	// Defaults.
	//
	// Set BEFORE the parameters are declared, because SetOptionParamInfo takes
	// the default as an argument and reads it from here.
	//
	// Every numeric default is a 0..1 host value that Controls.cpp maps to a
	// physical one -- see the note there on why a ranged parameter cannot carry
	// a ranged default. The values below are the inverse of those mappings, and
	// the physical quantity each one produces is in the comment.
	//-----------------------------------------------------------------------
	params[ PT_SHAPE ]     = static_cast< float >( Shape::Circle );
	params[ PT_INSTANCES ] = 0.333f;  // 8 shapes
	params[ PT_SIZE ]      = 0.540f;  // 0.06 of the short edge
	params[ PT_SIZE_VARY ] = 0.0f;
	params[ PT_STRETCH ]   = 0.5f;    // exactly 1.0
	params[ PT_ROUNDNESS ] = 0.0f;
	params[ PT_OUTLINE ]   = 0.0f;
	params[ PT_SOFTNESS ]  = 0.0f;

	// Off, so an existing composition and every rendered test look exactly as
	// they did. A shape with no shading is the null, byte for byte.
	params[ PT_SHADE ]     = 0.0f;
	params[ PT_LIGHT ]     = 0.25f;   // from the top of the frame

	params[ PT_PATH ]       = static_cast< float >( Path::Orbit );
	params[ PT_SYNC ]       = static_cast< float >( Sync::Free );
	params[ PT_SPEED ]      = 0.521f;  // ~0.15 cycles per second
	params[ PT_PHASE ]      = 0.0f;
	params[ PT_SPREAD ]     = 1.0f;    // a full chase around the path
	params[ PT_SCATTER ]    = 0.0f;
	params[ PT_SEED ]       = 0.0f;    // seed 1
	params[ PT_PATH_SIZE ]  = 0.467f;  // 0.7 of the frame
	params[ PT_CENTRE_X ]   = 0.5f;    // frame centre
	params[ PT_CENTRE_Y ]   = 0.5f;
	params[ PT_RATIO_X ]    = 0.646f;  // 3
	params[ PT_RATIO_Y ]    = 0.5f;    // 2
	params[ PT_DIRECTION ]  = 0.0f;    // travelling right
	params[ PT_GRID_COLS ]  = 0.2f;    // 4
	params[ PT_GRID_ROWS ]  = 0.1333f; // 3
	params[ PT_SPIN ]       = 0.5f;    // exactly 0
	params[ PT_SPIN_PHASE ] = 0.0f;

	params[ PT_PULSE ]        = 0.0f;
	params[ PT_PULSE_BRIGHT ] = 0.0f;
	params[ PT_PULSE_WIDTH ]  = 0.5f;

	params[ PT_COLOUR_MODE ]  = static_cast< float >( ColourMode::White );
	params[ PT_SHAPE_R ]      = 1.0f;
	params[ PT_SHAPE_G ]      = 1.0f;
	params[ PT_SHAPE_B ]      = 1.0f;
	params[ PT_HUE_SPREAD ]   = 1.0f;
	params[ PT_OPACITY ]      = 1.0f;
	params[ PT_BACK_R ]       = 0.0f;
	params[ PT_BACK_G ]       = 0.0f;
	params[ PT_BACK_B ]       = 0.0f;
	params[ PT_BACK_OPACITY ] = 1.0f;  // opaque black: what a mask wants
	params[ PT_BLEND ]        = static_cast< float >( Blend::Over );

	params[ PT_MASK_MODE ] = static_cast< float >( MaskMode::Over );
	params[ PT_MIX ]       = 1.0f;

	params[ PT_AUDIO_SIZE ]   = 0.0f;
	params[ PT_AUDIO_BRIGHT ] = 0.0f;

	//-----------------------------------------------------------------------
	// Declaration. This order is the order the host shows them in.
	//-----------------------------------------------------------------------
	SetOptionParamInfo( PT_SHAPE, "Shape", static_cast< int >( Shape::Count ), params[ PT_SHAPE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Shape::Count ); ++i )
		SetParamElementInfo( PT_SHAPE, i, ShapeName( static_cast< Shape >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_INSTANCES, "Count", FF_TYPE_STANDARD );
	SetParamInfof( PT_SIZE, "Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_SIZE_VARY, "Size Variation", FF_TYPE_STANDARD );
	SetParamInfof( PT_STRETCH, "Stretch", FF_TYPE_STANDARD );
	SetParamInfof( PT_ROUNDNESS, "Roundness", FF_TYPE_STANDARD );
	SetParamInfof( PT_OUTLINE, "Outline", FF_TYPE_STANDARD );
	SetParamInfof( PT_SOFTNESS, "Softness", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_PATH, "Path", static_cast< int >( Path::Count ), params[ PT_PATH ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Path::Count ); ++i )
		SetParamElementInfo( PT_PATH, i, PathName( static_cast< Path >( i ) ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_SYNC, "Sync", static_cast< int >( Sync::Count ), params[ PT_SYNC ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Sync::Count ); ++i )
		SetParamElementInfo( PT_SYNC, i, SyncName( static_cast< Sync >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_SPEED, "Speed", FF_TYPE_STANDARD );
	SetParamInfof( PT_PHASE, "Phase", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPREAD, "Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCATTER, "Scatter", FF_TYPE_STANDARD );
	SetParamInfof( PT_SEED, "Seed", FF_TYPE_STANDARD );
	SetParamInfof( PT_PATH_SIZE, "Path Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_X, "Centre X", FF_TYPE_STANDARD );
	SetParamInfof( PT_CENTRE_Y, "Centre Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_RATIO_X, "Ratio X", FF_TYPE_STANDARD );
	SetParamInfof( PT_RATIO_Y, "Ratio Y", FF_TYPE_STANDARD );
	SetParamInfof( PT_DIRECTION, "Direction", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRID_COLS, "Grid Columns", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRID_ROWS, "Grid Rows", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPIN, "Spin", FF_TYPE_STANDARD );
	SetParamInfof( PT_SPIN_PHASE, "Spin Phase", FF_TYPE_STANDARD );

	SetParamInfof( PT_PULSE, "Pulse Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_PULSE_BRIGHT, "Pulse Bright", FF_TYPE_STANDARD );
	SetParamInfof( PT_PULSE_WIDTH, "Pulse Width", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_COLOUR_MODE, "Colour Mode", static_cast< int >( ColourMode::Count ), params[ PT_COLOUR_MODE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( ColourMode::Count ); ++i )
		SetParamElementInfo( PT_COLOUR_MODE, i, ColourModeName( static_cast< ColourMode >( i ) ), static_cast< float >( i ) );

	// FF_TYPE_RED carries the swatch; the green and blue components are separate
	// parameters that the host groups behind it by type, which is why only the
	// red one gets a human name.
	SetParamInfof( PT_SHAPE_R, "Colour", FF_TYPE_RED );
	SetParamInfof( PT_SHAPE_G, "Colour_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_SHAPE_B, "Colour_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_HUE_SPREAD, "Hue Spread", FF_TYPE_STANDARD );
	SetParamInfof( PT_OPACITY, "Opacity", FF_TYPE_STANDARD );

	SetParamInfof( PT_BACK_R, "Background", FF_TYPE_RED );
	SetParamInfof( PT_BACK_G, "Background_Green", FF_TYPE_GREEN );
	SetParamInfof( PT_BACK_B, "Background_Blue", FF_TYPE_BLUE );
	SetParamInfof( PT_BACK_OPACITY, "Background Opacity", FF_TYPE_STANDARD );

	SetOptionParamInfo( PT_BLEND, "Blend", static_cast< int >( Blend::Count ), params[ PT_BLEND ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( Blend::Count ); ++i )
		SetParamElementInfo( PT_BLEND, i, BlendName( static_cast< Blend >( i ) ), static_cast< float >( i ) );

	SetOptionParamInfo( PT_MASK_MODE, "Mask Mode", static_cast< int >( MaskMode::Count ), params[ PT_MASK_MODE ] );
	for( unsigned int i = 0; i < static_cast< unsigned int >( MaskMode::Count ); ++i )
		SetParamElementInfo( PT_MASK_MODE, i, MaskModeName( static_cast< MaskMode >( i ) ), static_cast< float >( i ) );

	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	// Factory presets. Element 0 is Custom; picking anything else copies that
	// preset's values into the covered parameters and raises value events so
	// the host re-reads the sliders. Editing a covered slider flips back to
	// Custom.
	SetOptionParamInfo( PT_PRESET, "Preset", 1 + presets::kCount, params[ PT_PRESET ] );
	SetParamElementInfo( PT_PRESET, 0, "Custom", 0.0f );
	for( int i = 0; i < presets::kCount; ++i )
		SetParamElementInfo( PT_PRESET, 1 + i, presets::kPresets[ i ].name, static_cast< float >( 1 + i ) );

	//-----------------------------------------------------------------------
	// Audio. PT_AUDIO is an FFT buffer: Resolume shows it as an audio-source
	// picker and writes one spectrum bin per element, low frequencies first.
	// The element defaults are zero on purpose -- with no audio routed the two
	// knobs do nothing, rather than the shapes twitching to a phantom signal.
	//-----------------------------------------------------------------------
	SetBufferParamInfo( PT_AUDIO, "Audio", kMaxInstances, FF_USAGE_FFT );
	for( int i = 0; i < kMaxInstances; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );

	SetParamInfof( PT_AUDIO_SIZE, "Audio Size", FF_TYPE_STANDARD );
	SetParamInfof( PT_AUDIO_BRIGHT, "Audio Bright", FF_TYPE_STANDARD );

	SetParamInfof( PT_SHADE, "Shade", FF_TYPE_STANDARD );
	SetParamInfof( PT_LIGHT, "Light", FF_TYPE_STANDARD );

	//-----------------------------------------------------------------------
	// Groups. Thirty-odd parameters in one flat list is how somebody else's
	// inspector stops being readable. SetParamGroup collapses *runs* of
	// same-group parameters, which is why the ids in Controls.h have to stay in
	// this order.
	//-----------------------------------------------------------------------
	for( unsigned int id = PT_SHAPE; id <= PT_SOFTNESS; ++id )
		SetParamGroup( id, "Shape" );
	for( unsigned int id = PT_PATH; id <= PT_SPIN_PHASE; ++id )
		SetParamGroup( id, "Motion" );
	for( unsigned int id = PT_PULSE; id <= PT_PULSE_WIDTH; ++id )
		SetParamGroup( id, "Pulse" );
	for( unsigned int id = PT_COLOUR_MODE; id <= PT_BLEND; ++id )
		SetParamGroup( id, "Colour" );
	for( unsigned int id = PT_MASK_MODE; id <= PT_MIX; ++id )
		SetParamGroup( id, "Output" );
	SetParamGroup( PT_PRESET, "Preset" );
	for( unsigned int id = PT_AUDIO; id <= PT_AUDIO_BRIGHT; ++id )
		SetParamGroup( id, "Audio" );
	for( unsigned int id = PT_SHADE; id <= PT_LIGHT; ++id )
		SetParamGroup( id, "Shading" );
	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( unsigned int id = PT_ABOUT_TEXT; id < PT_COUNT; ++id )
		SetParamGroup( id, "About" );

}

//---------------------------------------------------------------------------
// GL lifetime
//---------------------------------------------------------------------------
bool OrreryPlugin::BuildShaders()
{
	const char* defines = overInput ? kEffectDefine : "";

	const std::string backgroundFragment = WithDefines( kBackgroundFragmentShader, defines );
	const std::string shapeFragment      = WithDefines( kShapeFragmentShader, defines );

	if( !backgroundShader.Compile( kBackgroundVertexShader, backgroundFragment.c_str() ) )
	{
		diag::error( "background shader would not compile" );
		return false;
	}

	if( !shapeShader.Compile( kShapeVertexShader, shapeFragment.c_str() ) )
	{
		diag::error( "shape shader would not compile" );
		return false;
	}

	// A uniform that does not resolve is a silent no-op -- glUniform on location
	// -1 is documented to do nothing -- and for these two the symptom is a
	// plugin that draws no shapes at all, which is indistinguishable from every
	// shape being off-screen. Say so in the log rather than leaving it to be
	// guessed at.
	if( shapeShader.FindUniform( "Xform" ) < 0 || shapeShader.FindUniform( "Tint" ) < 0 )
		diag::error( "instance uniform arrays did not resolve -- no shapes will be drawn" );

	return true;
}

FFResult OrreryPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();

	const GLubyte* version = glGetString( GL_VERSION );
	diag::info( std::string( "InitGL, GL " )
	            + ( version != nullptr ? reinterpret_cast< const char* >( version ) : "unknown" ) );

	if( !BuildShaders() )
	{
		DeInitGL();
		return FF_FAIL;
	}

	// A core profile refuses to draw with no vertex array bound, even though
	// both shaders build their geometry from gl_VertexID and source nothing.
	glGenVertexArrays( 1, &emptyVAO );

	xformScratch.assign( static_cast< size_t >( kMaxInstances ) * 4, 0.0f );
	tintScratch.assign( static_cast< size_t >( kMaxInstances ) * 4, 0.0f );

	currentViewport = *vp;
	return FF_SUCCESS;
}

FFResult OrreryPlugin::DeInitGL()
{
	backgroundShader.FreeGLResources();
	shapeShader.FreeGLResources();

	if( emptyVAO != 0 )
	{
		glDeleteVertexArrays( 1, &emptyVAO );
		emptyVAO = 0;
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// Parameters
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
char* OrreryPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_TEXT )
	{
		// Function-local rather than a member: the line is built from
		// compile-time facts, so it is the same for every instance, and the
		// host only needs the pointer to outlive the call.
		static const std::string text = stoatworks::about::textParam( 0 );
		return const_cast< char* >( text.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

//---------------------------------------------------------------------------
FFResult OrreryPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult OrreryPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	seedHostValues();

	// The About buttons open a browser and store nothing, so they are handled
	// before any of the bookkeeping below: pressing one is not the operator
	// editing a control.
	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_PRESET )
	{
		const int chosen = static_cast< int >( std::lround( value ) );
		if( chosen != static_cast< int >( std::lround( params[ PT_PRESET ] ) ) )
			applyPreset( chosen );
		return FF_SUCCESS;
	}

	// The host may be restating a value it still believes in rather than the
	// operator moving anything. Letting that through would overwrite the
	// preset's value in params[] AND read as an edit, dropping the dropdown
	// back to Custom -- which is what made presets look like they could not
	// be selected at all. See AGENTS.md.
	if( hostIsRestatingItself( index, value ) )
		return FF_SUCCESS;

	// A slider moved while a preset is active means the operator has taken
	// over: the dropdown falls back to Custom. The equality guard matters —
	// hosts that honour the value events echo the preset's own values straight
	// back through here, and that echo must not un-set the preset.
	const float previous = params[ index ];
	params[ index ]      = value;

	const int active = static_cast< int >( std::lround( params[ PT_PRESET ] ) );
	if( active > 0 && std::fabs( value - previous ) > 1e-4f )
	{
		for( unsigned int id : kPresetParamIDs )
		{
			if( id == index )
			{
				// Logged, unlike an ordinary parameter change: this one is a
				// state change an operator can be surprised by, it happens once
				// rather than per frame, and diagnosing vertigo #2 needed a code
				// read precisely because nothing said it had happened.
				diag::info( "preset dropped to Custom: parameter "
				            + std::to_string( index ) + " moved to "
				            + std::to_string( value ) );
				params[ PT_PRESET ] = 0.0f;
				RaiseParamEvent( PT_PRESET, FF_EVENT_FLAG_VALUE );
				break;
			}
		}
	}

	return FF_SUCCESS;
}

const unsigned int* OrreryPlugin::PresetParamIDsForTest( int& count )
{
	count = orrery::presets::kParamCount;
	return kPresetParamIDs;
}

float OrreryPlugin::presetValue( int presetIndex, unsigned int id ) const
{
	if( presetIndex <= 0 || presetIndex > orrery::presets::kCount )
		return -1.0f;

	const orrery::presets::Preset& preset = orrery::presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < orrery::presets::kParamCount; ++j )
		if( kPresetParamIDs[ j ] == id )
			return preset.v[ j ];

	return -1.0f;
}

void OrreryPlugin::seedHostValues()
{
	// Seeded on first parameter traffic rather than in the constructor, so the
	// whole mechanism stays in one place. It has to happen BEFORE applyPreset
	// can run: seeding afterwards would record the preset's own values as the
	// host's opening position, and the host's very next restatement would then
	// look like an edit -- which is the bug this exists to fix, reintroduced.
	if( hostValuesSeeded )
		return;

	for( unsigned int i = 0; i < PT_COUNT; ++i )
		hostValues[ i ] = params[ i ];
	hostValuesSeeded = true;
}

bool OrreryPlugin::hostIsRestatingItself( unsigned int index, float value )
{
	const float lastFromHost = hostValues[ index ];
	hostValues[ index ]      = value;

	const float fromPreset =
		presetValue( static_cast< int >( std::lround( params[ PT_PRESET ] ) ), index );
	if( fromPreset < 0.0f )
		return false;

	// A quantisation allowance rather than a float epsilon. A host that keeps
	// its parameters shorter than a float -- or round-trips them through a UI,
	// a MIDI value or a saved composition -- hands back a number near ours
	// rather than ours, and 1e-4 read that as an edit.
	constexpr float kSame = 1e-3f;

	if( std::fabs( value - fromPreset ) <= kSame )
	{
		// The host agreeing with the preset. Nothing to write -- and writing it
		// would actively hurt: a host that quantises hands back a ROUNDED copy
		// of our own value, params[] would take the rounding, and the existing
		// "did a covered parameter move?" test below works to a tighter
		// tolerance than this one and would read that rounding as an edit.
		return true;
	}

	if( std::fabs( value - lastFromHost ) > kSame )
		return false;//neither: the operator has taken over

	// Deliberately not logged. A host that pushes its parameters every frame
	// would put a line here every frame, and a log that scrolls is a log nobody
	// reads. The event worth recording is the one below, in the fallback to
	// Custom, which happens once.
	return true;
}

void OrreryPlugin::applyPreset( int presetIndex )
{
	params[ PT_PRESET ] = static_cast< float >( presetIndex );

	if( presetIndex <= 0 || presetIndex > presets::kCount )
		return;//Custom: the sliders keep whatever they said

	const presets::Preset& preset = presets::kPresets[ presetIndex - 1 ];
	for( int j = 0; j < presets::kParamCount; ++j )
	{
		const unsigned int id = kPresetParamIDs[ j ];
		if( std::fabs( params[ id ] - preset.v[ j ] ) <= 1e-6f )
			continue;

		// The copy is what changes the picture; the event only tells the host
		// to re-read the slider. A host that ignores it renders the preset
		// correctly and merely shows stale knobs.
		params[ id ] = preset.v[ j ];
		RaiseParamEvent( id, FF_EVENT_FLAG_VALUE );
	}
}

float OrreryPlugin::GetFloatParameter( unsigned int index )
{
	return index < PT_COUNT ? params[ index ] : 0.0f;
}

void OrreryPlugin::SetPhaseOverride( float phase )
{
	phasePinned = true;
	pinnedPhase = phase;
}

//---------------------------------------------------------------------------
// Phase
//---------------------------------------------------------------------------
float OrreryPlugin::CurrentPhase() const
{
	const Sync sync   = static_cast< Sync >( Option( params[ PT_SYNC ], static_cast< int >( Sync::Count ) ) );
	const float speed = SpeedFromParam( params[ PT_SPEED ] );
	const float manual = params[ PT_PHASE ];

	// Pinning replaces the CLOCK, not the whole phase. The Phase slider stays
	// live underneath it, which is what lets tools/sweep.py prove that slider
	// is connected -- a pin that swallowed it too would make it look dead.
	if( phasePinned )
		return pinnedPhase + manual;

	float driven = 0.0f;

	switch( sync )
	{
	case Sync::Free:
		// Not `hostSeconds * speed`: see UpdatePhaseAnchor. Until the operator
		// has moved Speed this is exactly that product, because the anchor
		// starts at clock zero with phase zero.
		driven = static_cast< float >( phaseAnchor + ( hostSeconds - anchorClock ) * speed );
		break;

	case Sync::Beat:
	case Sync::Bar:
	{
		//-------------------------------------------------------------------
		// The host hands us a tempo and a position *within* the current bar,
		// and never says which bar it is. A bar counter would be state, and
		// state is the thing this plugin does not have.
		//
		// So recover a continuous bar number without keeping one: the clock
		// gives an estimate of how many bars have passed, `barPhase` gives the
		// exact position inside the bar, and the whole number that reconciles
		// them is `round( estimate - barPhase )`. The result is continuous
		// across the bar line -- as `barPhase` wraps from 1 to 0 the rounded
		// integer steps up by one at the same moment -- and it stays exact even
		// if the clock estimate is off by up to half a bar.
		//
		// It can name the wrong absolute bar if the host's transport did not
		// start at time zero. That is invisible: the animation repeats, so an
		// integer bar offset is not a thing anyone can see.
		//-------------------------------------------------------------------
		const float tempo      = bpm > 1.0f ? bpm : 120.0f;
		const float barSeconds = 240.0f / tempo;   // four beats to the bar
		const float estimate   = static_cast< float >( hostSeconds ) / barSeconds;
		const float within     = Clamp01( barPhase );

		const float bars = within + std::round( estimate - within );

		driven = ( sync == Sync::Beat ? bars * 4.0f : bars ) * speed;
		break;
	}

	case Sync::Manual:
	default:
		// Speed is deliberately ignored. This is the mode for driving Phase from
		// Resolume's own BPM-synced animation, or from a keyframe, or from a
		// MIDI fader -- and a second clock underneath it would fight whatever is
		// doing the driving.
		driven = 0.0f;
		break;
	}

	return driven + manual;
}

//---------------------------------------------------------------------------
// The clock, and audio
//---------------------------------------------------------------------------
void OrreryPlugin::UpdateClock()
{
	// FFGL never says what unit SetTime arrives in, and hosts disagree:
	// Resolume sends MILLISECONDS (measured live at 20.0 per frame at its
	// 50 fps, and the SDK's own Particles sample divides by 1000), while the
	// offline harness sends seconds. Reading it raw is a thousand times fast
	// on the one host that matters and exactly right on the one that gets
	// tested, which is how it stays hidden.
	//
	// This used to guess the unit from the magnitude of a single frame delta
	// and then lock. That had three holes: a delta between 0.5 and 2.0 decided
	// nothing, a burst of sub-0.5 ms frames at load -- a thumbnail render on a
	// quick GPU -- locked it to "seconds" for the rest of the session, and
	// while undecided it assumed seconds, which is precisely the millisecond
	// host's wrong answer.
	//
	// So measure instead of guessing. steady_clock says how much real time
	// passed, the host says how much host time passed, and the ratio names the
	// unit outright. Nothing plausible sits between 1 and 1000, so both bands
	// are wide and a frame fitting neither simply does not vote.
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	// Never read `hostTime` before the host has set it: CFFGLPlugin's
	// constructor initialises bpm and barPhase and leaves hostTime
	// uninitialised, so until SetTime lands it is whatever was in that memory.
	const double raw = hostTimeSeen ? hostTime : -1.0;

	if( clockScale == 0.0 && raw >= 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		// A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			// Several frames rather than one, so a single odd frame -- the
			// first after a seek, say -- cannot decide it on its own.
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
			{
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
				diag::info( std::string( "host clock is " )
				            + ( clockScale == 0.001 ? "milliseconds" : "seconds" )
				            + ", scale=" + std::to_string( clockScale ) );
			}
		}
	}

	if( raw >= 0.0 )
		lastRawTime = raw;
	lastWallTime = wallNow;

	// Until the unit is settled -- and for a host that never calls SetTime at
	// all -- run on the real clock. Wrong in origin but right in rate, where
	// assuming seconds would be a thousand times fast on Resolume.
	hostSeconds = ( raw >= 0.0 && clockScale != 0.0 ) ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
void OrreryPlugin::UpdatePhaseAnchor()
{
	const Sync sync   = static_cast< Sync >( Option( params[ PT_SYNC ], static_cast< int >( Sync::Count ) ) );
	const float speed = SpeedFromParam( params[ PT_SPEED ] );

	// Beat and Bar are meant to jump -- they re-lock to the transport, which is
	// the point of them. Keep the anchor following the clock while they are
	// selected so that returning to Free resumes rather than leaps.
	if( sync != Sync::Free )
	{
		anchorClock = hostSeconds;
		anchorSpeed = speed;
		return;
	}

	// First frame: leave the anchor at clock zero, phase zero. That makes the
	// expression above identical to the old `hostSeconds * speed` for as long
	// as nobody touches Speed, which is what keeps every rendered-frame test
	// and tools/sweep.py measuring the same thing they measured before.
	if( anchorSpeed < 0.0f )
	{
		anchorSpeed = speed;
		return;
	}

	if( speed != anchorSpeed )
	{
		// Once per speed change, not once per frame: this carries the exact
		// phase forward rather than integrating it, so a long session cannot
		// accumulate rounding into a drift. Frame rate still cannot affect
		// where anything is.
		phaseAnchor += ( hostSeconds - anchorClock ) * anchorSpeed;
		anchorClock = hostSeconds;
		anchorSpeed = speed;
	}
}

FFResult OrreryPlugin::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

void OrreryPlugin::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void OrreryPlugin::TickClockForTest()
{
	UpdateClock();
	UpdatePhaseAnchor();
}

double OrreryPlugin::ClockScaleForTest() const
{
	return clockScale;
}

float OrreryPlugin::CurrentPhaseForTest() const
{
	return CurrentPhase();
}

double OrreryPlugin::HostSecondsForTest() const
{
	return hostSeconds;
}

void OrreryPlugin::UpdateAudio()
{
	const ParamInfo* info = FindParamInfo( PT_AUDIO );
	if( info == nullptr )
		return;

	// Frame delta for the release filter, off the normalised clock UpdateClock
	// just advanced, so the ms-vs-seconds question is already settled. First
	// frame -- or a clock that has not moved -- snaps instead.
	const double now = hostSeconds;
	const double dt  = ( audioClock >= 0.0 && now > audioClock ) ? now - audioClock : 0.0;
	audioClock       = now;

	// Fast up, slow down -- the same asymmetry as tinsel's temporal filter and
	// for the same reason: a flash that arrives a frame late reads as broken,
	// while one that takes ~150 ms to die away reads as intended. Symmetric
	// smoothing would trade the flicker for lag on the attack, which is worse.
	const float release = dt > 0.0 ? 1.0f - std::exp( static_cast< float >( -dt / 0.15 ) ) : 1.0f;

	const size_t bins = std::min( info->elements.size(), audioLevel.size() );
	for( size_t i = 0; i < bins; ++i )
	{
		// sqrt because bin magnitudes bunch near zero: a spectrum used raw
		// leaves everything but the kick invisible.
		const float raw = std::sqrt( std::max( 0.0f, info->elements[ i ].value ) );

		if( raw >= audioLevel[ i ] )
			audioLevel[ i ] = raw;
		else
			audioLevel[ i ] += ( raw - audioLevel[ i ] ) * release;
	}
}

//---------------------------------------------------------------------------
// Parameters to motion
//---------------------------------------------------------------------------
MotionParams OrreryPlugin::CurrentMotion( int width, int height ) const
{
	MotionParams m;

	m.shape = static_cast< Shape >( Option( params[ PT_SHAPE ], static_cast< int >( Shape::Count ) ) );
	m.path  = static_cast< Path >( Option( params[ PT_PATH ], static_cast< int >( Path::Count ) ) );
	m.count = InstancesFromParam( params[ PT_INSTANCES ] );
	m.phase = CurrentPhase();

	m.spread  = Clamp01( params[ PT_SPREAD ] );
	m.scatter = Clamp01( params[ PT_SCATTER ] );
	m.seed    = SeedFromParam( params[ PT_SEED ] );

	m.pathSize = PathSizeFromParam( params[ PT_PATH_SIZE ] );
	m.centreX  = CentreFromParam( params[ PT_CENTRE_X ] );
	m.centreY  = CentreFromParam( params[ PT_CENTRE_Y ] );
	m.ratioX   = RatioFromParam( params[ PT_RATIO_X ] );
	m.ratioY   = RatioFromParam( params[ PT_RATIO_Y ] );
	m.direction = DirectionFromParam( params[ PT_DIRECTION ] );
	m.gridCols  = GridFromParam( params[ PT_GRID_COLS ] );
	m.gridRows  = GridFromParam( params[ PT_GRID_ROWS ] );

	m.size     = SizeFromParam( params[ PT_SIZE ] );
	m.sizeVary = Clamp01( params[ PT_SIZE_VARY ] );

	m.spin      = SpinFromParam( params[ PT_SPIN ] );
	m.spinPhase = Clamp01( params[ PT_SPIN_PHASE ] );

	m.pulse       = Clamp01( params[ PT_PULSE ] );
	m.pulseBright = Clamp01( params[ PT_PULSE_BRIGHT ] );
	m.pulseWidth  = PulseWidthFromParam( params[ PT_PULSE_WIDTH ] );

	m.audioSize   = Clamp01( params[ PT_AUDIO_SIZE ] );
	m.audioBright = Clamp01( params[ PT_AUDIO_BRIGHT ] );

	// One broad band per instance rather than the first `count` bins: six
	// shapes should carve the whole spectrum into six, not all sit on the
	// bass. Instance order is band order, low frequencies first -- on a Grid
	// that reads left-to-right as a spectrum analyser.
	for( int i = 0; i < m.count; ++i )
	{
		const int lo = i * kMaxInstances / m.count;
		const int hi = std::max( lo + 1, ( i + 1 ) * kMaxInstances / m.count );

		float sum = 0.0f;
		for( int b = lo; b < hi; ++b )
			sum += audioLevel[ static_cast< size_t >( b ) ];
		m.audio[ static_cast< size_t >( i ) ] = sum / static_cast< float >( hi - lo );
	}

	m.colourMode = static_cast< ColourMode >( Option( params[ PT_COLOUR_MODE ], static_cast< int >( ColourMode::Count ) ) );
	m.r          = Clamp01( params[ PT_SHAPE_R ] );
	m.g          = Clamp01( params[ PT_SHAPE_G ] );
	m.b          = Clamp01( params[ PT_SHAPE_B ] );
	m.hueSpread  = HueSpreadFromParam( params[ PT_HUE_SPREAD ] );

	// Mix fades the shape layer. The source has no clip to mix against, so it
	// ignores it -- the parameter exists in both only so that a composition
	// moved from one plugin to the other does not find its parameter list has
	// shifted underneath it.
	m.opacity = Clamp01( params[ PT_OPACITY ] ) * ( overInput ? Clamp01( params[ PT_MIX ] ) : 1.0f );

	m.aspect = ( width > 0 && height > 0 )
	           ? static_cast< float >( width ) / static_cast< float >( height )
	           : 1.0f;

	return m;
}

//---------------------------------------------------------------------------
// Drawing
//---------------------------------------------------------------------------
void OrreryPlugin::Render( int width, int height, GLuint inputTexture, float maxU, float maxV )
{
	if( !backgroundShader.IsReady() || !shapeShader.IsReady() )
		return;

	UpdateClock();
	UpdatePhaseAnchor();
	UpdateAudio();

	const MotionParams motion = CurrentMotion( width, height );
	Solve( motion, instances );

	const MaskMode mask = overInput
	                      ? static_cast< MaskMode >( Option( params[ PT_MASK_MODE ], static_cast< int >( MaskMode::Count ) ) )
	                      : MaskMode::Over;
	const Blend blend = static_cast< Blend >( Option( params[ PT_BLEND ], static_cast< int >( Blend::Count ) ) );
	const float mix   = Clamp01( params[ PT_MIX ] );

	// Reveal and Colourise build their picture only where the shapes are, so
	// the clip behind them fades IN as the effect mixes OUT.
	const bool buildsFromClip = ( mask == MaskMode::Reveal || mask == MaskMode::Colourise );
	const float clipGain      = buildsFromClip ? 1.0f - mix : 1.0f;

	int sampleMode = 0;
	switch( mask )
	{
	case MaskMode::Reveal:    sampleMode = 1; break;
	case MaskMode::Colourise: sampleMode = 2; break;
	case MaskMode::Hide:      sampleMode = 3; break;
	case MaskMode::Over:
	default:                  sampleMode = 0; break;
	}

	glBindVertexArray( emptyVAO );

	//-----------------------------------------------------------------------
	// Pass 1: the background.
	//
	// glUseProgram directly rather than ffglex::ScopedShaderBinding, and an
	// explicit glBindTexture rather than ScopedTextureBinding, because every
	// ffglex Scoped* binding CLEARS to 0 when it leaves scope instead of
	// restoring what was there. That is survivable here but it is a trap worth
	// not stepping into twice, and the explicit form is no longer.
	//-----------------------------------------------------------------------
	glDisable( GL_BLEND );
	glUseProgram( backgroundShader.GetGLID() );

	if( overInput )
	{
		glActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, inputTexture );
		backgroundShader.Set( "Clip", 0 );
		backgroundShader.Set( "MaxUV", maxU, maxV );
		backgroundShader.Set( "ClipGain", clipGain );
	}
	else
	{
		backgroundShader.Set( "BackColour",
		                      Clamp01( params[ PT_BACK_R ] ),
		                      Clamp01( params[ PT_BACK_G ] ),
		                      Clamp01( params[ PT_BACK_B ] ),
		                      Clamp01( params[ PT_BACK_OPACITY ] ) );
	}

	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	//-----------------------------------------------------------------------
	// Pass 2: the shapes.
	//-----------------------------------------------------------------------
	const int count = static_cast< int >( instances.size() );
	if( count > 0 )
	{
		glUseProgram( shapeShader.GetGLID() );

		for( int i = 0; i < count; ++i )
		{
			const Instance& in = instances[ i ];
			float* x           = &xformScratch[ static_cast< size_t >( i ) * 4 ];
			float* t           = &tintScratch[ static_cast< size_t >( i ) * 4 ];

			x[ 0 ] = in.x;
			x[ 1 ] = in.y;
			x[ 2 ] = in.scale;
			x[ 3 ] = in.rotation;

			t[ 0 ] = in.r;
			t[ 1 ] = in.g;
			t[ 2 ] = in.b;
			t[ 3 ] = in.a;
		}

		const GLint xformLoc = shapeShader.FindUniform( "Xform" );
		const GLint tintLoc  = shapeShader.FindUniform( "Tint" );
		if( xformLoc >= 0 )
			glUniform4fv( xformLoc, count, xformScratch.data() );
		if( tintLoc >= 0 )
			glUniform4fv( tintLoc, count, tintScratch.data() );

		const float outline  = Clamp01( params[ PT_OUTLINE ] );
		const float softness = SoftnessFromParam( params[ PT_SOFTNESS ] );
		const float stretch  = StretchFromParam( params[ PT_STRETCH ] );

		// The quad has to contain the shape, its outline, and its feather. Slack
		// on top because overdrawing a few pixels costs nothing and a shape that
		// reaches past its quad silently loses a corner.
		const float bound = ShapeBound( motion.shape ) + outline * 0.5f + softness * 2.0f + 0.05f;

		shapeShader.Set( "Resolution", static_cast< float >( width ), static_cast< float >( height ) );
		shapeShader.Set( "Bound", bound );
		shapeShader.Set( "Stretch", stretch );
		shapeShader.Set( "ShapeKind", static_cast< int >( motion.shape ) );
		shapeShader.Set( "Roundness", Clamp01( params[ PT_ROUNDNESS ] ) );
		shapeShader.Set( "Outline", outline );
		shapeShader.Set( "Softness", softness );
		shapeShader.Set( "Shade", Clamp01( params[ PT_SHADE ] ) );
		shapeShader.Set( "LightAngle", Clamp01( params[ PT_LIGHT ] ) );
		shapeShader.Set( "SampleMode", sampleMode );

		if( overInput )
		{
			glActiveTexture( GL_TEXTURE0 );
			glBindTexture( GL_TEXTURE_2D, inputTexture );
			shapeShader.Set( "Clip", 0 );
			shapeShader.Set( "MaxUV", maxU, maxV );
		}

		if( mask == MaskMode::Hide )
		{
			// Punch the shapes out of what is already there: keep no source, and
			// scale the destination by the shape's transparency. This is the
			// whole of Hide, and it is why there is no mask buffer anywhere in
			// this plugin.
			glEnable( GL_BLEND );
			glBlendEquation( GL_FUNC_ADD );
			glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_ALPHA );
		}
		else
		{
			ApplyBlend( blend );
		}

		glDrawArraysInstanced( GL_TRIANGLE_STRIP, 0, 4, count );
	}

	//-----------------------------------------------------------------------
	// Leave the state as the host is entitled to find it.
	//-----------------------------------------------------------------------
	glDisable( GL_BLEND );
	glBlendEquation( GL_FUNC_ADD );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	glUseProgram( 0 );
	glBindVertexArray( 0 );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, 0 );
}

FFResult OrreryPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	int width           = 0;
	int height          = 0;
	GLuint inputTexture = 0;
	float maxU          = 1.0f;
	float maxV          = 1.0f;

	if( overInput )
	{
		if( pGL == nullptr || pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
			return FF_FAIL;

		const FFGLTextureStruct& texture = *pGL->inputTextures[ 0 ];
		inputTexture                     = texture.Handle;
		width                            = texture.Width;
		height                           = texture.Height;

		// The input texture can be bigger than the picture; MaxUV is the
		// fraction that was really drawn. The shapes are placed in frame space
		// and never touch this -- only the fetch of the clip does.
		const FFGLTexCoords coords = GetMaxGLTexCoords( texture );
		maxU                       = coords.s;
		maxV                       = coords.t;
	}
	else
	{
		width  = static_cast< int >( currentViewport.width );
		height = static_cast< int >( currentViewport.height );
	}

	if( width <= 0 || height <= 0 )
		return FF_FAIL;

	Render( width, height, inputTexture, maxU, maxV );
	return FF_SUCCESS;
}

} // namespace orrery
