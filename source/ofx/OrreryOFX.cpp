/// The OpenFX builds of Orrery, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts. Two plugins from this one file, as the FFGL side ships
/// two bundles: "Orrery" is a generator, "Orrery Mask" draws the shapes over
/// — or cuts them into — the incoming clip.
///
/// The motion still has exactly one home: Motion.cpp, the same C++ the FFGL
/// build solves per instance and ortest measures. What this file mirrors from
/// Shaders.cpp is the per-pixel half — the signed distance functions and the
/// compositing — because the GPU evaluated those per fragment. When editing
/// the fragment shader's distance functions or blend rules, edit this too.
/// The sdStar5 negation is load-bearing on both sides; see the comment at the
/// function.
///
/// OFX hands render time in *frames*; phase is derived from seconds via the
/// clip frame rate. Beat and Bar sync do not exist here — OFX has no tempo —
/// so Sync offers Free and Manual, and Manual is the mode for keyframing
/// Phase against the edit.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

#include "../Controls.h"
#include "../Motion.h"
#include "../Presets.h"
#include "../Shapes.h"

namespace
{
constexpr const char* kSourceIdentifier = "com.stoatworks.orrery";
constexpr const char* kMaskIdentifier   = "com.stoatworks.orrerymask";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Primitive shapes moving on deterministic paths.\n\n"
	"An instance's placement is a pure function of (index, phase) — no "
	"simulation state — so a chase cannot drift, any frame renders on its "
	"own, and the picture is identical at every resolution. For quick "
	"animated masks, and for chroma animations driving a pixel map.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamShape       = "shape";
constexpr const char* kParamInstances   = "instances";
constexpr const char* kParamSize        = "size";
constexpr const char* kParamSizeVary    = "sizeVariation";
constexpr const char* kParamStretch     = "stretch";
constexpr const char* kParamRoundness   = "roundness";
constexpr const char* kParamOutline     = "outline";
constexpr const char* kParamSoftness    = "softness";
constexpr const char* kParamShade       = "shade";
constexpr const char* kParamLight       = "light";
constexpr const char* kParamPath        = "path";
constexpr const char* kParamSync        = "sync";
constexpr const char* kParamSpeed       = "speed";
constexpr const char* kParamPhase       = "phase";
constexpr const char* kParamSpread      = "spread";
constexpr const char* kParamScatter     = "scatter";
constexpr const char* kParamSeed        = "seed";
constexpr const char* kParamPathSize    = "pathSize";
constexpr const char* kParamCentreX     = "centreX";
constexpr const char* kParamCentreY     = "centreY";
constexpr const char* kParamRatioX      = "ratioX";
constexpr const char* kParamRatioY      = "ratioY";
constexpr const char* kParamDirection   = "direction";
constexpr const char* kParamGridCols    = "gridColumns";
constexpr const char* kParamGridRows    = "gridRows";
constexpr const char* kParamSpin        = "spin";
constexpr const char* kParamSpinPhase   = "spinPhase";
constexpr const char* kParamPulse       = "pulse";
constexpr const char* kParamPulseBright = "pulseBrightness";
constexpr const char* kParamPulseWidth  = "pulseWidth";
constexpr const char* kParamColourMode  = "colourMode";
constexpr const char* kParamColour      = "colour";
constexpr const char* kParamHueSpread   = "hueSpread";
constexpr const char* kParamOpacity     = "opacity";
constexpr const char* kParamBackColour  = "backColour";
constexpr const char* kParamBackOpacity = "backOpacity";
constexpr const char* kParamBlend       = "blend";
constexpr const char* kParamMaskMode    = "maskMode";
constexpr const char* kParamMix         = "mix";
constexpr const char* kParamPreset      = "preset";

//---------------------------------------------------------------------------
// Distance functions — the CPU mirror of the fragment shader's. All
// normalised so the shape's furthest point sits at radius 1. The bodies are
// the standard analytic forms (Inigo Quilez's derivations); when editing one
// side, edit the other.
//---------------------------------------------------------------------------
struct Vec2
{
	float x = 0.0f, y = 0.0f;
};

inline float sdCircle( Vec2 p )
{
	return std::sqrt( p.x * p.x + p.y * p.y ) - 1.0f;
}

inline float sdBox( Vec2 p, float bx, float by )
{
	const float qx = std::abs( p.x ) - bx;
	const float qy = std::abs( p.y ) - by;
	const float mx = std::max( qx, 0.0f );
	const float my = std::max( qy, 0.0f );
	return std::min( std::max( qx, qy ), 0.0f ) + std::sqrt( mx * mx + my * my );
}

inline float sdTriangle( Vec2 p )
{
	const float k = 1.7320508f;
	const float s = 1.1547005f;

	//Shape space runs y-down and the analytic form is written y-up; the flip
	//is what points the triangle up on screen.
	float px = p.x * s;
	float py = -p.y * s;

	px = std::abs( px ) - 1.0f;
	py = py + 1.0f / k;
	if( px + k * py > 0.0f )
	{
		const float nx = ( px - k * py ) * 0.5f;
		const float ny = ( -k * px - py ) * 0.5f;
		px = nx;
		py = ny;
	}
	px -= std::clamp( px, -2.0f, 0.0f );

	const float length = std::sqrt( px * px + py * py );
	return -length * ( py < 0.0f ? -1.0f : 1.0f ) / s;
}

inline float sdHexagon( Vec2 p )
{
	const float kx = -0.8660254f, ky = 0.5f, kz = 0.5773503f;
	const float r  = 0.8660254f;

	float px = std::abs( p.x );
	float py = std::abs( p.y );

	const float dot = std::min( kx * px + ky * py, 0.0f );
	px -= 2.0f * dot * kx;
	py -= 2.0f * dot * ky;

	px -= std::clamp( px, -kz * r, kz * r );
	py -= r;

	return std::sqrt( px * px + py * py ) * ( py < 0.0f ? -1.0f : 1.0f );
}

inline float sdStar5( Vec2 p, float rf )
{
	const float k1x = 0.809016994f, k1y = -0.587785252f;
	const float k2x = -k1x, k2y = k1y;

	float px = p.x;
	float py = -p.y; //point up on screen, as for the triangle

	px = std::abs( px );
	const float d1 = std::max( k1x * px + k1y * py, 0.0f );
	px -= 2.0f * d1 * k1x;
	py -= 2.0f * d1 * k1y;
	const float d2 = std::max( k2x * px + k2y * py, 0.0f );
	px -= 2.0f * d2 * k2x;
	py -= 2.0f * d2 * k2y;
	px = std::abs( px );
	py -= 1.0f;

	const float bax = rf * -k1y;
	const float bay = rf * k1x - 1.0f;
	const float h   = std::clamp( ( px * bax + py * bay ) / ( bax * bax + bay * bay ), 0.0f, 1.0f );

	const float dx = px - bax * h;
	const float dy = py - bay * h;

	//Negated: this is the one shape whose analytic form comes out POSITIVE
	//inside, and everything downstream — the outline's abs(), the feather, the
	//coverage — assumes negative inside. Same on the GLSL side.
	const float sign = ( h * bax - px ) < 0.0f ? -1.0f : 1.0f;
	return -std::sqrt( dx * dx + dy * dy ) * sign;
}

inline float sdCross( Vec2 p, float bx, float by )
{
	float px = std::abs( p.x );
	float py = std::abs( p.y );
	if( py > px )
		std::swap( px, py );

	const float qx = px - bx;
	const float qy = py - by;
	const float k  = std::max( qy, qx );
	const float wx = k > 0.0f ? qx : by - px;
	const float wy = k > 0.0f ? qy : -k;

	const float mx = std::max( wx, 0.0f );
	const float my = std::max( wy, 0.0f );
	return ( k < 0.0f ? -1.0f : 1.0f ) * std::sqrt( mx * mx + my * my );
}

inline float sdRing( Vec2 p, float thickness )
{
	const float t = std::clamp( thickness, 0.02f, 0.9f );
	return std::abs( std::sqrt( p.x * p.x + p.y * p.y ) - ( 1.0f - t * 0.5f ) ) - t * 0.5f;
}

float shapeDistance( orrery::Shape kind, Vec2 p, float roundness )
{
	using orrery::Shape;

	//Generic corner rounding: shrink the shape, then dilate the field back
	//out, so the outer extent stays where the bound was told it would be.
	const float rr = kind == Shape::Ring ? 0.0f : std::clamp( roundness, 0.0f, 1.0f ) * 0.35f;
	const float k  = 1.0f - rr;

	const Vec2 q{ p.x / k, p.y / k };
	float d;

	switch( kind )
	{
	case Shape::Square:   d = sdBox( q, 1.0f, 1.0f ); break;
	case Shape::Triangle: d = sdTriangle( q ); break;
	case Shape::Hexagon:  d = sdHexagon( q ); break;
	case Shape::Star:     d = sdStar5( q, 0.45f ); break;
	case Shape::Cross:    d = sdCross( q, 1.0f, 0.3f ); break;
	case Shape::Ring:     d = sdRing( q, roundness ); break;
	case Shape::Bar:      d = sdBox( q, 1.0f, 0.15f ); break;
	case Shape::Circle:
	default:              d = sdCircle( q ); break;
	}

	return d * k - rr;
}

/// One placed instance with its per-render constants precomputed.
struct DrawInstance
{
	float cx = 0.5f, cy = 0.5f; //!< centre, frame space, y down
	float scale = 0.1f;
	float cosR = 1.0f, sinR = 0.0f;
	float r = 1, g = 1, b = 1, a = 1;

	//Conservative pixel bounds, for the reject test.
	int px0 = 0, px1 = 0, py0 = 0, py1 = 0;
};

/// Everything one render needs, fixed before the threads fan out.
struct ShapeSetup
{
	std::vector<DrawInstance> instances;
	orrery::Shape shape = orrery::Shape::Circle;
	orrery::Blend blend = orrery::Blend::Over;
	orrery::MaskMode mask = orrery::MaskMode::Over;

	float stretch   = 1.0f;
	float roundness = 0.0f;
	float outline   = 0.0f;
	float softness  = 0.0f;
	float shade     = 0.0f;
	float light     = 0.25f;
	float bound     = 1.05f;

	float backR = 0, backG = 0, backB = 0, backA = 1;
	float clipGain = 1.0f; //!< effect: how much clip under the shapes
	bool over      = false;

	//Frame geometry.
	float shortEdgeX = 1.0f, shortEdgeY = 1.0f; //!< short-edge fractions per axis
	float aa = 0.001f;                          //!< antialias width in shape units, per unit scale
};

class OrreryProcessorBase : public OFX::ImageProcessor
{
public:
	explicit OrreryProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( OFX::Image* src, const ShapeSetup* v, bool premultipliedValue )
	{
		srcImg        = src;
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	OFX::Image* srcImg      = nullptr;
	const ShapeSetup* setup = nullptr;
	bool premultiplied      = false;
};

template<class PIX, int nComponents, int maxValue>
class OrreryProcessor : public OrreryProcessorBase
{
public:
	explicit OrreryProcessor( OFX::ImageEffect& effect ) :
		OrreryProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		using namespace orrery;

		const OfxRectI bounds = _dstImg->getBounds();
		const int outW        = bounds.x2 - bounds.x1;
		const int outH        = bounds.y2 - bounds.y1;
		const ShapeSetup& s   = *setup;

		const float quadX = s.bound * std::max( s.stretch, 0.001f );
		const float quadY = s.bound;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );

			//Frame space runs y-down; OFX rows run bottom-up.
			const float frameY = 1.0f - ( y - bounds.y1 + 0.5f ) / outH;
			const int rowPx    = y - bounds.y1;

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				const float frameX = ( x - bounds.x1 + 0.5f ) / outW;
				const int colPx    = x - bounds.x1;

				double clip[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
				if( s.over )
					readClip( x, y, clip );

				//--- pass 1: the background --------------------------------
				double accR, accG, accB, accA;
				if( s.over )
				{
					accR = clip[ 0 ] * s.clipGain;
					accG = clip[ 1 ] * s.clipGain;
					accB = clip[ 2 ] * s.clipGain;
					accA = clip[ 3 ] * s.clipGain;
				}
				else
				{
					accR = s.backR * s.backA;
					accG = s.backG * s.backA;
					accB = s.backB * s.backA;
					accA = s.backA;
				}

				//--- pass 2: the shapes, in instance order ------------------
				for( const DrawInstance& in : s.instances )
				{
					if( colPx < in.px0 || colPx > in.px1 || rowPx < in.py0 || rowPx > in.py1 )
						continue;

					//Invert the vertex transform: frame -> shape space.
					const float offX = ( frameX - in.cx ) / ( in.scale * s.shortEdgeX );
					const float offY = ( frameY - in.cy ) / ( in.scale * s.shortEdgeY );

					const float localX = offX * in.cosR + offY * in.sinR;
					const float localY = -offX * in.sinR + offY * in.cosR;

					//The GPU only rasterises the instance's quad; matching that
					//bound keeps outline and feather clipped identically.
					if( std::abs( localX ) > quadX || std::abs( localY ) > quadY )
						continue;

					const float minSt = std::min( s.stretch, 1.0f );
					const float d0 =
						shapeDistance( s.shape, Vec2{ localX / std::max( s.stretch, 0.001f ), localY }, s.roundness )
						* minSt;

					const float aa = s.aa / in.scale;
					const float d  = s.outline > 0.001f ? std::abs( d0 ) - s.outline * 0.5f : d0;

					const float feather = std::max( aa, s.softness * 2.0f );
					float t             = std::clamp( ( d + feather * 0.5f ) / feather, 0.0f, 1.0f );
					const float coverage = 1.0f - t * t * ( 3.0f - 2.0f * t );
					if( coverage <= 0.0f )
						continue;

					double rgbR = in.r, rgbG = in.g, rgbB = in.b;
					double alpha = coverage * in.a;

					if( s.over && ( s.mask == MaskMode::Reveal || s.mask == MaskMode::Colourise ) )
					{
						const double clipA = clip[ 3 ];
						const double clipR = clipA > 0.0 ? clip[ 0 ] / clipA : 0.0;
						const double clipG = clipA > 0.0 ? clip[ 1 ] / clipA : 0.0;
						const double clipB = clipA > 0.0 ? clip[ 2 ] / clipA : 0.0;

						rgbR  = s.mask == MaskMode::Colourise ? clipR * in.r : clipR;
						rgbG  = s.mask == MaskMode::Colourise ? clipG * in.g : clipG;
						rgbB  = s.mask == MaskMode::Colourise ? clipB * in.b : clipB;
						alpha = alpha * clipA;
					}

					//-------------------------------------------------------
					// Shading, mirroring the block at the end of main() in
					// kShapeFragmentShader. This is the second copy of the
					// maths and it is here for the same reason shapeDistance
					// is: the OpenFX build rasterises on the CPU, so there is
					// no shader to borrow. Keep the two in step.
					//-------------------------------------------------------
					if( s.shade > 0.001f )
					{
						const float h  = 0.01f;
						const float sx = std::max( s.stretch, 0.001f );

						const float gx =
							shapeDistance( s.shape, Vec2{ ( localX + h ) / sx, localY }, s.roundness )
							- shapeDistance( s.shape, Vec2{ ( localX - h ) / sx, localY }, s.roundness );
						const float gy =
							shapeDistance( s.shape, Vec2{ localX / sx, localY + h }, s.roundness )
							- shapeDistance( s.shape, Vec2{ localX / sx, localY - h }, s.roundness );

						const float glen = std::sqrt( gx * gx + gy * gy );
						const float ox   = glen > 1e-6f ? gx / glen : 0.0f;
						const float oy   = glen > 1e-6f ? gy / glen : 0.0f;

						const float reach = std::max( minSt, 1e-4f );
						const float depth = std::clamp( 1.0f + d0 / reach, 0.0f, 1.0f );

						const float nx = ox * depth;
						const float ny = oy * depth;
						const float nz = std::sqrt( std::max( 0.0f, 1.0f - depth * depth ) );

						const float a  = s.light * 6.28318531f;
						const float lx = std::cos( a );
						const float ly = -std::sin( a );
						const float lz = 0.65f;
						const float ll = std::sqrt( lx * lx + ly * ly + lz * lz );

						const float lambert = std::max( 0.0f, ( nx * lx + ny * ly + nz * lz ) / ll );

						const float hx = lx / ll;
						const float hy = ly / ll;
						const float hz = lz / ll + 1.0f;
						const float hl = std::sqrt( hx * hx + hy * hy + hz * hz );
						const float spec =
							std::pow( std::max( 0.0f, ( nx * hx + ny * hy + nz * hz ) / hl ), 24.0f );

						const double gain = 0.35 + 0.75 * lambert;
						const double add  = 0.5 * spec;

						rgbR += s.shade * ( rgbR * gain + add - rgbR );
						rgbG += s.shade * ( rgbG * gain + add - rgbG );
						rgbB += s.shade * ( rgbB * gain + add - rgbB );
					}

					const double srcR = rgbR * alpha;
					const double srcG = rgbG * alpha;
					const double srcB = rgbB * alpha;

					if( s.over && s.mask == MaskMode::Hide )
					{
						//Punch the shapes out of what is already there.
						accR *= 1.0 - alpha;
						accG *= 1.0 - alpha;
						accB *= 1.0 - alpha;
						accA *= 1.0 - alpha;
					}
					else if( s.blend == Blend::Add )
					{
						accR += srcR;
						accG += srcG;
						accB += srcB;
						accA += alpha;
					}
					else if( s.blend == Blend::Max )
					{
						accR = std::max( accR, srcR );
						accG = std::max( accG, srcG );
						accB = std::max( accB, srcB );
						accA = std::max( accA, alpha );
					}
					else
					{
						accR = srcR + accR * ( 1.0 - alpha );
						accG = srcG + accG * ( 1.0 - alpha );
						accB = srcB + accB * ( 1.0 - alpha );
						accA = alpha + accA * ( 1.0 - alpha );
					}
				}

				accA = std::clamp( accA, 0.0, 1.0 );
				accR = std::min( std::clamp( accR, 0.0, 1.0 ), accA );
				accG = std::min( std::clamp( accG, 0.0, 1.0 ), accA );
				accB = std::min( std::clamp( accB, 0.0, 1.0 ), accA );

				if( !premultiplied && nComponents == 4 && accA > 0.0 )
				{
					accR /= accA;
					accG /= accA;
					accB /= accA;
				}

				dstPix[ 0 ] = quantise( accR );
				dstPix[ 1 ] = quantise( accG );
				dstPix[ 2 ] = quantise( accB );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( accA );
			}
		}
	}

private:
	void readClip( int x, int y, double out[ 4 ] ) const
	{
		const PIX* srcPix = srcImg ? static_cast<const PIX*>( srcImg->getPixelAddress( x, y ) ) : nullptr;
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( !premultiplied && nComponents == 4 )
		{
			out[ 0 ] *= out[ 3 ];
			out[ 1 ] *= out[ 3 ];
			out[ 2 ] *= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class OrreryOFXPlugin : public OFX::ImageEffect
{
public:
	OrreryOFXPlugin( OfxImageEffectHandle handle, bool maskVariant ) :
		OFX::ImageEffect( handle ),
		over( maskVariant )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		if( over )
			srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		shape       = fetchChoiceParam( kParamShape );
		instances   = fetchDoubleParam( kParamInstances );
		size        = fetchDoubleParam( kParamSize );
		sizeVary    = fetchDoubleParam( kParamSizeVary );
		stretch     = fetchDoubleParam( kParamStretch );
		roundness   = fetchDoubleParam( kParamRoundness );
		outline     = fetchDoubleParam( kParamOutline );
		softness    = fetchDoubleParam( kParamSoftness );
		shade       = fetchDoubleParam( kParamShade );
		light       = fetchDoubleParam( kParamLight );
		path        = fetchChoiceParam( kParamPath );
		sync        = fetchChoiceParam( kParamSync );
		speed       = fetchDoubleParam( kParamSpeed );
		phase       = fetchDoubleParam( kParamPhase );
		spread      = fetchDoubleParam( kParamSpread );
		scatter     = fetchDoubleParam( kParamScatter );
		seed        = fetchDoubleParam( kParamSeed );
		pathSize    = fetchDoubleParam( kParamPathSize );
		centreX     = fetchDoubleParam( kParamCentreX );
		centreY     = fetchDoubleParam( kParamCentreY );
		ratioX      = fetchDoubleParam( kParamRatioX );
		ratioY      = fetchDoubleParam( kParamRatioY );
		direction   = fetchDoubleParam( kParamDirection );
		gridCols    = fetchDoubleParam( kParamGridCols );
		gridRows    = fetchDoubleParam( kParamGridRows );
		spin        = fetchDoubleParam( kParamSpin );
		spinPhase   = fetchDoubleParam( kParamSpinPhase );
		pulse       = fetchDoubleParam( kParamPulse );
		pulseBright = fetchDoubleParam( kParamPulseBright );
		pulseWidth  = fetchDoubleParam( kParamPulseWidth );
		colourMode  = fetchChoiceParam( kParamColourMode );
		colour      = fetchRGBParam( kParamColour );
		hueSpread   = fetchDoubleParam( kParamHueSpread );
		opacity     = fetchDoubleParam( kParamOpacity );
		blend       = fetchChoiceParam( kParamBlend );
		if( !over )
		{
			backColour  = fetchRGBParam( kParamBackColour );
			backOpacity = fetchDoubleParam( kParamBackOpacity );
		}
		else
		{
			maskMode = fetchChoiceParam( kParamMaskMode );
			mix      = fetchDoubleParam( kParamMix );
		}
		preset = fetchChoiceParam( kParamPreset );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		using namespace orrery::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset — same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			// The null checks matter: the mask variant has no background params.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setChoice( shape, p.v[ kShape ] );
			setDouble( instances, p.v[ kInstances ] );
			setDouble( size, p.v[ kSize ] );
			setDouble( sizeVary, p.v[ kSizeVary ] );
			setDouble( stretch, p.v[ kStretch ] );
			setDouble( roundness, p.v[ kRoundness ] );
			setDouble( outline, p.v[ kOutline ] );
			setDouble( softness, p.v[ kSoftness ] );
			setChoice( path, p.v[ kPath ] );
			setDouble( speed, p.v[ kSpeed ] );
			setDouble( spread, p.v[ kSpread ] );
			setDouble( scatter, p.v[ kScatter ] );
			setDouble( pathSize, p.v[ kPathSize ] );
			setDouble( ratioX, p.v[ kRatioX ] );
			setDouble( ratioY, p.v[ kRatioY ] );
			setDouble( direction, p.v[ kDirection ] );
			setDouble( gridCols, p.v[ kGridCols ] );
			setDouble( gridRows, p.v[ kGridRows ] );
			setDouble( spin, p.v[ kSpin ] );
			setDouble( spinPhase, p.v[ kSpinPhase ] );
			setDouble( pulse, p.v[ kPulse ] );
			setDouble( pulseBright, p.v[ kPulseBright ] );
			setDouble( pulseWidth, p.v[ kPulseWidth ] );
			setChoice( colourMode, p.v[ kColourMode ] );
			setRGB( colour, p.v[ kShapeR ], p.v[ kShapeG ], p.v[ kShapeB ] );
			setDouble( hueSpread, p.v[ kHueSpread ] );
			setDouble( opacity, p.v[ kOpacity ] );
			setRGB( backColour, p.v[ kBackR ], p.v[ kBackG ], p.v[ kBackB ] );
			setDouble( backOpacity, p.v[ kBackOpacity ] );
			setChoice( blend, p.v[ kBlend ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p    = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamShape && choiceDiffers( shape, p.v[ kShape ] ) ) ||
			( paramName == kParamInstances && doubleDiffers( instances, p.v[ kInstances ] ) ) ||
			( paramName == kParamSize && doubleDiffers( size, p.v[ kSize ] ) ) ||
			( paramName == kParamSizeVary && doubleDiffers( sizeVary, p.v[ kSizeVary ] ) ) ||
			( paramName == kParamStretch && doubleDiffers( stretch, p.v[ kStretch ] ) ) ||
			( paramName == kParamRoundness && doubleDiffers( roundness, p.v[ kRoundness ] ) ) ||
			( paramName == kParamOutline && doubleDiffers( outline, p.v[ kOutline ] ) ) ||
			( paramName == kParamSoftness && doubleDiffers( softness, p.v[ kSoftness ] ) ) ||
			( paramName == kParamPath && choiceDiffers( path, p.v[ kPath ] ) ) ||
			( paramName == kParamSpeed && doubleDiffers( speed, p.v[ kSpeed ] ) ) ||
			( paramName == kParamSpread && doubleDiffers( spread, p.v[ kSpread ] ) ) ||
			( paramName == kParamScatter && doubleDiffers( scatter, p.v[ kScatter ] ) ) ||
			( paramName == kParamPathSize && doubleDiffers( pathSize, p.v[ kPathSize ] ) ) ||
			( paramName == kParamRatioX && doubleDiffers( ratioX, p.v[ kRatioX ] ) ) ||
			( paramName == kParamRatioY && doubleDiffers( ratioY, p.v[ kRatioY ] ) ) ||
			( paramName == kParamDirection && doubleDiffers( direction, p.v[ kDirection ] ) ) ||
			( paramName == kParamGridCols && doubleDiffers( gridCols, p.v[ kGridCols ] ) ) ||
			( paramName == kParamGridRows && doubleDiffers( gridRows, p.v[ kGridRows ] ) ) ||
			( paramName == kParamSpin && doubleDiffers( spin, p.v[ kSpin ] ) ) ||
			( paramName == kParamSpinPhase && doubleDiffers( spinPhase, p.v[ kSpinPhase ] ) ) ||
			( paramName == kParamPulse && doubleDiffers( pulse, p.v[ kPulse ] ) ) ||
			( paramName == kParamPulseBright && doubleDiffers( pulseBright, p.v[ kPulseBright ] ) ) ||
			( paramName == kParamPulseWidth && doubleDiffers( pulseWidth, p.v[ kPulseWidth ] ) ) ||
			( paramName == kParamColourMode && choiceDiffers( colourMode, p.v[ kColourMode ] ) ) ||
			( paramName == kParamColour && rgbDiffers( colour, p.v[ kShapeR ], p.v[ kShapeG ], p.v[ kShapeB ] ) ) ||
			( paramName == kParamHueSpread && doubleDiffers( hueSpread, p.v[ kHueSpread ] ) ) ||
			( paramName == kParamOpacity && doubleDiffers( opacity, p.v[ kOpacity ] ) ) ||
			( paramName == kParamBackColour && rgbDiffers( backColour, p.v[ kBackR ], p.v[ kBackG ], p.v[ kBackB ] ) ) ||
			( paramName == kParamBackOpacity && doubleDiffers( backOpacity, p.v[ kBackOpacity ] ) ) ||
			( paramName == kParamBlend && choiceDiffers( blend, p.v[ kBlend ] ) );

		if( covered )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src;
		if( over && srcClip && srcClip->isConnected() )
			src.reset( srcClip->fetchImage( args.time ) );

		const bool premultiplied =
			over && srcClip ? srcClip->getPreMultiplication() == OFX::eImagePreMultiplied
							: dstClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		ShapeSetup setup;
		buildSetup( args, *dst, setup );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run<OrreryProcessor<unsigned char, 4, 255>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<OrreryProcessor<unsigned char, 3, 255>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run<OrreryProcessor<unsigned short, 4, 65535>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<OrreryProcessor<unsigned short, 3, 65535>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run<OrreryProcessor<float, 4, 1>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<OrreryProcessor<float, 3, 1>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

private:
	void buildSetup( const OFX::RenderArguments& args, OFX::Image& dst, ShapeSetup& setup )
	{
		using namespace orrery;

		const double t  = args.time;
		const OfxRectI b = dst.getBounds();
		const int outW   = b.x2 - b.x1;
		const int outH   = b.y2 - b.y1;

		double fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;
		const float seconds = float( t / fps );

		MotionParams m;

		int choice = 0;
		shape->getValueAtTime( t, choice );
		m.shape = Shape( choice );
		path->getValueAtTime( t, choice );
		m.path = Path( choice );

		m.count = InstancesFromParam( float( instances->getValueAtTime( t ) ) );

		//Phase: Free runs off the clip clock, Manual off the slider alone —
		//the mode for keyframing against the edit.
		int syncChoice = 0;
		sync->getValueAtTime( t, syncChoice );
		const float speedValue = SpeedFromParam( float( speed->getValueAtTime( t ) ) );
		const float manual     = float( phase->getValueAtTime( t ) );
		m.phase                = ( syncChoice == 0 ? seconds * speedValue : 0.0f ) + manual;

		m.spread  = float( spread->getValueAtTime( t ) );
		m.scatter = float( scatter->getValueAtTime( t ) );
		m.seed    = SeedFromParam( float( seed->getValueAtTime( t ) ) );

		m.pathSize  = PathSizeFromParam( float( pathSize->getValueAtTime( t ) ) );
		m.centreX   = CentreFromParam( float( centreX->getValueAtTime( t ) ) );
		m.centreY   = CentreFromParam( float( centreY->getValueAtTime( t ) ) );
		m.ratioX    = RatioFromParam( float( ratioX->getValueAtTime( t ) ) );
		m.ratioY    = RatioFromParam( float( ratioY->getValueAtTime( t ) ) );
		m.direction = DirectionFromParam( float( direction->getValueAtTime( t ) ) );
		m.gridCols  = GridFromParam( float( gridCols->getValueAtTime( t ) ) );
		m.gridRows  = GridFromParam( float( gridRows->getValueAtTime( t ) ) );

		m.size     = SizeFromParam( float( size->getValueAtTime( t ) ) );
		m.sizeVary = float( sizeVary->getValueAtTime( t ) );

		m.spin      = SpinFromParam( float( spin->getValueAtTime( t ) ) );
		m.spinPhase = float( spinPhase->getValueAtTime( t ) );

		m.pulse       = float( pulse->getValueAtTime( t ) );
		m.pulseBright = float( pulseBright->getValueAtTime( t ) );
		m.pulseWidth  = PulseWidthFromParam( float( pulseWidth->getValueAtTime( t ) ) );

		colourMode->getValueAtTime( t, choice );
		m.colourMode = ColourMode( choice );

		double r = 1.0, g = 1.0, bChannel = 1.0;
		colour->getValueAtTime( t, r, g, bChannel );
		m.r = float( r );
		m.g = float( g );
		m.b = float( bChannel );

		m.hueSpread = HueSpreadFromParam( float( hueSpread->getValueAtTime( t ) ) );

		float mixValue = 1.0f;
		if( over && mix )
			mixValue = float( mix->getValueAtTime( t ) );
		m.opacity = float( opacity->getValueAtTime( t ) ) * ( over ? mixValue : 1.0f );

		const double par = dst.getPixelAspectRatio() > 0.0 ? dst.getPixelAspectRatio() : 1.0;
		m.aspect         = float( double( outW ) * par / std::max( 1, outH ) );

		std::vector<Instance> solved;
		Solve( m, solved );

		setup.over  = over;
		setup.shape = m.shape;

		int blendChoice = 0;
		blend->getValueAtTime( t, blendChoice );
		setup.blend = Blend( blendChoice );

		if( over && maskMode )
		{
			maskMode->getValueAtTime( t, choice );
			setup.mask = MaskMode( choice );
		}

		//Reveal and Colourise build their picture only where the shapes are,
		//so the clip behind them fades IN as the effect mixes OUT.
		const bool buildsFromClip = setup.mask == MaskMode::Reveal || setup.mask == MaskMode::Colourise;
		setup.clipGain            = buildsFromClip ? 1.0f - mixValue : 1.0f;

		setup.stretch   = StretchFromParam( float( stretch->getValueAtTime( t ) ) );
		setup.roundness = float( roundness->getValueAtTime( t ) );
		setup.outline   = float( outline->getValueAtTime( t ) );
		setup.softness  = SoftnessFromParam( float( softness->getValueAtTime( t ) ) );
		setup.shade     = std::clamp( float( shade->getValueAtTime( t ) ), 0.0f, 1.0f );
		setup.light     = std::clamp( float( light->getValueAtTime( t ) ), 0.0f, 1.0f );
		setup.bound     = ShapeBound( m.shape ) + setup.outline * 0.5f + setup.softness * 2.0f + 0.05f;

		if( !over && backColour )
		{
			backColour->getValueAtTime( t, r, g, bChannel );
			setup.backR = float( r );
			setup.backG = float( g );
			setup.backB = float( bChannel );
			setup.backA = float( backOpacity->getValueAtTime( t ) );
		}

		setup.shortEdgeX = outW >= outH ? float( outH ) / outW : 1.0f;
		setup.shortEdgeY = outW >= outH ? 1.0f : float( outW ) / outH;

		//One shape-space unit spans scale * shortEdge pixels; the antialias
		//width is one pixel of that, folded with the instance scale at use.
		setup.aa = 1.0f / std::max( 1, std::min( outW, outH ) );

		const float quadX = setup.bound * std::max( setup.stretch, 0.001f );
		const float quadY = setup.bound;

		setup.instances.reserve( solved.size() );
		for( const Instance& in : solved )
		{
			DrawInstance d;
			d.cx    = in.x;
			d.cy    = in.y;
			d.scale = std::max( in.scale, 1e-6f );
			d.cosR  = std::cos( in.rotation );
			d.sinR  = std::sin( in.rotation );
			d.r     = in.r;
			d.g     = in.g;
			d.b     = in.b;
			d.a     = in.a;

			//Conservative pixel bounds of the rotated quad.
			const float reach  = std::sqrt( quadX * quadX + quadY * quadY ) * d.scale;
			const float halfPxX = reach * setup.shortEdgeX * outW;
			const float halfPxY = reach * setup.shortEdgeY * outH;
			const float centrePxX = d.cx * outW;
			const float centrePxYDown = d.cy * outH;

			d.px0 = int( std::floor( centrePxX - halfPxX ) );
			d.px1 = int( std::ceil( centrePxX + halfPxX ) );
			//Frame y runs down; pixel rows run up.
			d.py0 = int( std::floor( outH - centrePxYDown - halfPxY ) );
			d.py1 = int( std::ceil( outH - centrePxYDown + halfPxY ) );

			if( d.px1 < 0 || d.px0 >= outW || d.py1 < 0 || d.py0 >= outH )
				continue; //entirely off frame

			setup.instances.push_back( d );
		}
	}

	template<class Processor>
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
			  const ShapeSetup& setup, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( src, &setup, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	const bool over;
	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::ChoiceParam* shape        = nullptr;
	OFX::DoubleParam* instances    = nullptr;
	OFX::DoubleParam* size         = nullptr;
	OFX::DoubleParam* sizeVary     = nullptr;
	OFX::DoubleParam* stretch      = nullptr;
	OFX::DoubleParam* roundness    = nullptr;
	OFX::DoubleParam* outline      = nullptr;
	OFX::DoubleParam* softness     = nullptr;
	OFX::DoubleParam* shade        = nullptr;
	OFX::DoubleParam* light        = nullptr;
	OFX::ChoiceParam* path         = nullptr;
	OFX::ChoiceParam* sync         = nullptr;
	OFX::DoubleParam* speed        = nullptr;
	OFX::DoubleParam* phase        = nullptr;
	OFX::DoubleParam* spread       = nullptr;
	OFX::DoubleParam* scatter      = nullptr;
	OFX::DoubleParam* seed         = nullptr;
	OFX::DoubleParam* pathSize     = nullptr;
	OFX::DoubleParam* centreX      = nullptr;
	OFX::DoubleParam* centreY      = nullptr;
	OFX::DoubleParam* ratioX       = nullptr;
	OFX::DoubleParam* ratioY       = nullptr;
	OFX::DoubleParam* direction    = nullptr;
	OFX::DoubleParam* gridCols     = nullptr;
	OFX::DoubleParam* gridRows     = nullptr;
	OFX::DoubleParam* spin         = nullptr;
	OFX::DoubleParam* spinPhase    = nullptr;
	OFX::DoubleParam* pulse        = nullptr;
	OFX::DoubleParam* pulseBright  = nullptr;
	OFX::DoubleParam* pulseWidth   = nullptr;
	OFX::ChoiceParam* colourMode   = nullptr;
	OFX::RGBParam* colour          = nullptr;
	OFX::DoubleParam* hueSpread    = nullptr;
	OFX::DoubleParam* opacity      = nullptr;
	OFX::RGBParam* backColour      = nullptr;
	OFX::DoubleParam* backOpacity  = nullptr;
	OFX::ChoiceParam* blend        = nullptr;
	OFX::ChoiceParam* maskMode     = nullptr;
	OFX::DoubleParam* mix          = nullptr;
	OFX::ChoiceParam* preset       = nullptr;

	// The preset table is plain floats; these give each param type its
	// reading of one. Option values are element indices, booleans are 0/1.
	// Null-tolerant, because the two variants declare different subsets.
	static bool doubleDiffers( OFX::DoubleParam* p, float v )
	{
		if( !p )
			return false;
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool choiceDiffers( OFX::ChoiceParam* p, float v )
	{
		if( !p )
			return false;
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}
	static bool rgbDiffers( OFX::RGBParam* p, float r, float g, float b )
	{
		if( !p )
			return false;
		double cr = 0.0, cg = 0.0, cb = 0.0;
		p->getValue( cr, cg, cb );
		return std::fabs( cr - double( r ) ) > 1e-4 || std::fabs( cg - double( g ) ) > 1e-4
			   || std::fabs( cb - double( b ) ) > 1e-4;
	}
	static void setDouble( OFX::DoubleParam* p, float v )
	{
		if( doubleDiffers( p, v ) )
			p->setValue( double( v ) );
	}
	static void setChoice( OFX::ChoiceParam* p, float v )
	{
		if( choiceDiffers( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}
	static void setRGB( OFX::RGBParam* p, float r, float g, float b )
	{
		if( rgbDiffers( p, r, g, b ) )
			p->setValue( double( r ), double( g ), double( b ) );
	}

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

void describeCommon( OFX::ImageEffectDescriptor& desc, const char* name )
{
	desc.setLabels( name, name, name );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// Placement is a pure function of (index, phase): frames render in any
	// order, alone, concurrently. Shapes are placed in frame space, so a tile
	// cannot know where they fall without the whole frame's geometry — the
	// geometry is per-render here, so tiles are declined for simplicity.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void describeParams( OFX::ImageEffectDescriptor& desc, bool maskVariant )
{
	using namespace orrery;

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named motions. Picking one sets the covered controls; editing any "
	                      "of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < orrery::presets::kCount; ++i )
		presetParam->appendOption( orrery::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* shapeGroup = desc.defineGroupParam( "Shape" );
	shapeGroup->setLabels( "Shape", "Shape", "Shape" );

	OFX::ChoiceParamDescriptor* shapeParam = desc.defineChoiceParam( kParamShape );
	shapeParam->setLabels( "Shape", "Shape", "Shape" );
	for( int i = 0; i < int( Shape::Count ); ++i )
		shapeParam->appendOption( ShapeName( Shape( i ) ) );
	shapeParam->setDefault( 0 );
	shapeParam->setParent( *shapeGroup );
	page->addChild( *shapeParam );

	defineSlider( desc, page, kParamInstances, "Instances", "Number of shapes, 1 to 64, quadratic.", 0.333 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamSize, "Size", "Radius as a fraction of the short edge, exponential.", 0.540 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamSizeVary, "Size Variation", "Hashed per-instance size spread.", 0.0 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamStretch, "Stretch", "Long-axis stretch, 0.1 to 10; 0.5 is 1:1.", 0.5 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamRoundness, "Roundness", "Corner rounding; ring thickness on the Ring.", 0.0 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamOutline, "Outline", "Stroke instead of fill.", 0.0 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamSoftness, "Softness", "Feathered edge. 0 is pixel-map hard.", 0.0 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamShade, "Shade", "Light the shape as though it were inflated. 0 is flat.", 0.0 )
		->setParent( *shapeGroup );
	defineSlider( desc, page, kParamLight, "Light", "Where the light comes from, one turn over the range.", 0.25 )
		->setParent( *shapeGroup );

	OFX::GroupParamDescriptor* motionGroup = desc.defineGroupParam( "Motion" );
	motionGroup->setLabels( "Motion", "Motion", "Motion" );

	OFX::ChoiceParamDescriptor* pathParam = desc.defineChoiceParam( kParamPath );
	pathParam->setLabels( "Path", "Path", "Path" );
	for( int i = 0; i < int( Path::Count ); ++i )
		pathParam->appendOption( PathName( Path( i ) ) );
	pathParam->setDefault( 0 );
	pathParam->setParent( *motionGroup );
	page->addChild( *pathParam );

	OFX::ChoiceParamDescriptor* syncParam = desc.defineChoiceParam( kParamSync );
	syncParam->setLabels( "Sync", "Sync", "Sync" );
	syncParam->setHint( "Free runs off the clip clock. Manual ignores Speed — keyframe Phase instead. "
						"(Beat and Bar exist only in the FFGL build; OFX hosts carry no tempo.)" );
	syncParam->appendOption( "Free" );
	syncParam->appendOption( "Manual" );
	syncParam->setDefault( 0 );
	syncParam->setParent( *motionGroup );
	page->addChild( *syncParam );

	defineSlider( desc, page, kParamSpeed, "Speed", "Cycles per second, exponential, 0 stops.", 0.521 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamPhase, "Phase", "Added to the driven phase, in cycles.", 0.0 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamSpread, "Spread", "0 moves the set as one body, 1 makes a chase.", 1.0 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamScatter, "Scatter", "Blends the even spread towards a hashed swarm.", 0.0 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamSeed, "Seed", "A different arrangement, 1 to 9999.", 0.0 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamPathSize, "Path Size", "Extent in frame space; past 1 leaves the frame.", 0.467 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamCentreX, "Centre X", "Path centre; can sit off-frame.", 0.5 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamCentreY, "Centre Y", "Path centre; can sit off-frame.", 0.5 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamRatioX, "Ratio X", "Lissajous frequency; bounce rate on x.", 0.646 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamRatioY, "Ratio Y", "Lissajous frequency; bounce rate on y.", 0.5 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamDirection, "Direction", "Drift heading; 0 travels right.", 0.0 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamGridCols, "Grid Columns", "Grid path only, 1 to 16.", 0.2 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamGridRows, "Grid Rows", "Grid path only, 1 to 16.", 0.1333 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamSpin, "Spin", "Revolutions per cycle; 0.5 is none.", 0.5 )
		->setParent( *motionGroup );
	defineSlider( desc, page, kParamSpinPhase, "Spin Phase", "Starting angle.", 0.0 )
		->setParent( *motionGroup );

	OFX::GroupParamDescriptor* pulseGroup = desc.defineGroupParam( "Pulse" );
	pulseGroup->setLabels( "Pulse", "Pulse", "Pulse" );

	defineSlider( desc, page, kParamPulse, "Pulse", "Scales each shape on its own beat.", 0.0 )
		->setParent( *pulseGroup );
	defineSlider( desc, page, kParamPulseBright, "Pulse Brightness", "Fades each shape on its own beat.", 0.0 )
		->setParent( *pulseGroup );
	defineSlider( desc, page, kParamPulseWidth, "Pulse Width", "Fraction of the cycle the flash occupies.", 0.5 )
		->setParent( *pulseGroup );

	OFX::GroupParamDescriptor* colourGroup = desc.defineGroupParam( "Colour" );
	colourGroup->setLabels( "Colour", "Colour", "Colour" );

	OFX::ChoiceParamDescriptor* colourModeParam = desc.defineChoiceParam( kParamColourMode );
	colourModeParam->setLabels( "Colour Mode", "Colour Mode", "Colour Mode" );
	for( int i = 0; i < int( ColourMode::Count ); ++i )
		colourModeParam->appendOption( ColourModeName( ColourMode( i ) ) );
	colourModeParam->setDefault( 0 );
	colourModeParam->setParent( *colourGroup );
	page->addChild( *colourModeParam );

	OFX::RGBParamDescriptor* colourParam = desc.defineRGBParam( kParamColour );
	colourParam->setLabels( "Colour", "Colour", "Colour" );
	colourParam->setHint( "The swatch the hue modes take saturation and value from." );
	colourParam->setDefault( 1.0, 1.0, 1.0 );
	colourParam->setParent( *colourGroup );
	page->addChild( *colourParam );

	defineSlider( desc, page, kParamHueSpread, "Hue Spread", "Hue range across the set, in turns.", 1.0 )
		->setParent( *colourGroup );
	defineSlider( desc, page, kParamOpacity, "Opacity", "", 1.0 )->setParent( *colourGroup );

	if( !maskVariant )
	{
		OFX::RGBParamDescriptor* backParam = desc.defineRGBParam( kParamBackColour );
		backParam->setLabels( "Background", "Background", "Background" );
		backParam->setDefault( 0.0, 0.0, 0.0 );
		backParam->setParent( *colourGroup );
		page->addChild( *backParam );

		defineSlider( desc, page, kParamBackOpacity, "Background Opacity",
					  "Opaque black is what a mask wants; 0 draws on transparency.", 1.0 )
			->setParent( *colourGroup );
	}

	OFX::ChoiceParamDescriptor* blendParam = desc.defineChoiceParam( kParamBlend );
	blendParam->setLabels( "Blend", "Blend", "Blend" );
	blendParam->setHint( "How overlapping shapes combine. Max keeps overlaps from brightening, "
						 "which is what a mask wants." );
	for( int i = 0; i < int( Blend::Count ); ++i )
		blendParam->appendOption( BlendName( Blend( i ) ) );
	blendParam->setDefault( 0 );
	blendParam->setParent( *colourGroup );
	page->addChild( *blendParam );

	if( maskVariant )
	{
		OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
		output->setLabels( "Output", "Output", "Output" );

		OFX::ChoiceParamDescriptor* maskParam = desc.defineChoiceParam( kParamMaskMode );
		maskParam->setLabels( "Mask Mode", "Mask Mode", "Mask Mode" );
		maskParam->setHint( "Over draws the shapes on the clip; Reveal shows the clip only inside "
							"them; Hide punches them out; Colourise tints the clip inside them." );
		for( int i = 0; i < int( MaskMode::Count ); ++i )
			maskParam->appendOption( MaskModeName( MaskMode( i ) ) );
		maskParam->setDefault( 0 );
		maskParam->setParent( *output );
		page->addChild( *maskParam );

		defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched clip.", 1.0 )
			->setParent( *output );
	}
}

} // namespace

//---------------------------------------------------------------------------
// "Orrery": the generator.
//---------------------------------------------------------------------------
mDeclarePluginFactory( OrrerySourceFactory, {}, {} );

void OrrerySourceFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Orrery" );
	desc.addSupportedContext( OFX::eContextGenerator );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void OrrerySourceFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, false );
}

OFX::ImageEffect* OrrerySourceFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new OrreryOFXPlugin( handle, false );
}

//---------------------------------------------------------------------------
// "Orrery Mask": the effect.
//---------------------------------------------------------------------------
mDeclarePluginFactory( OrreryMaskFactory, {}, {} );

void OrreryMaskFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	describeCommon( desc, "Orrery Mask" );
	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );
}

void OrreryMaskFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	describeParams( desc, true );
}

OFX::ImageEffect* OrreryMaskFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new OrreryOFXPlugin( handle, true );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static OrrerySourceFactory* sourceFactory =
		new OrrerySourceFactory( kSourceIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	static OrreryMaskFactory* maskFactory =
		new OrreryMaskFactory( kMaskIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( sourceFactory );
	ids.push_back( maskFactory );
}
