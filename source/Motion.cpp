#include "Motion.h"

#include <algorithm>
#include <cmath>

#include "Hash.h"

namespace orrery
{
namespace
{
constexpr float kPi  = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

// Distinct salts so that two quantities drawn for the same instance from the
// same seed are independent. Reusing one salt is not a crash, it is a
// correlation: the biggest shape would always be the one furthest left.
constexpr uint32_t kSaltPhase  = 0x00000000U;
constexpr uint32_t kSaltCross  = 0x5bf03635U;
constexpr uint32_t kSaltSize   = 0x27d4eb2fU;
constexpr uint32_t kSaltRateX  = 0x165667b1U;
constexpr uint32_t kSaltRateY  = 0x9e3779b9U;

float Fract( float x )
{
	return x - std::floor( x );
}

/// A triangle wave: period 2, range 0..1, and tri(0) == 0.
///
/// This is the whole of the bounce. A shape travelling at a constant speed and
/// reflecting off two walls *is* a triangle wave -- there is no approximation
/// here and no reason to integrate a velocity to discover it.
float Triangle( float x )
{
	const float t = Fract( x * 0.5f );
	return t < 0.5f ? t * 2.0f : 2.0f - t * 2.0f;
}

float Mix( float a, float b, float t )
{
	return a + ( b - a ) * t;
}

/// The shape's radius expressed as a fraction of each axis of the frame.
///
/// `scale` is in short-edge fractions so that a circle is round. Frame space is
/// 0..1 per axis. Those are the same number only on a square output.
void FrameRadius( float scale, float aspect, float& rx, float& ry )
{
	if( aspect >= 1.0f )
	{
		// Landscape: the short edge is the height.
		rx = scale / aspect;
		ry = scale;
	}
	else
	{
		// Portrait: the short edge is the width.
		rx = scale;
		ry = scale * aspect;
	}
}

} // namespace

const char* PathName( Path path )
{
	switch( path )
	{
	case Path::Orbit:     return "Orbit";
	case Path::Lissajous: return "Lissajous";
	case Path::Drift:     return "Drift";
	case Path::Bounce:    return "Bounce";
	case Path::Grid:      return "Grid";
	default:              return "Orbit";
	}
}

const char* ColourModeName( ColourMode mode )
{
	switch( mode )
	{
	case ColourMode::White:     return "White";
	case ColourMode::Solid:     return "Solid";
	case ColourMode::HueSpread: return "Hue Spread";
	case ColourMode::HueCycle:  return "Hue Cycle";
	default:                    return "White";
	}
}

float InstancePhase( const MotionParams& p, int index )
{
	const int count = std::max( 1, p.count );

	// The even spread is what makes a chase: consecutive instances sit a fixed
	// distance apart in phase, so they arrive in order.
	const float even = count > 1 ? static_cast< float >( index ) / static_cast< float >( count ) : 0.0f;
	const float rand = Unit( Hash2( static_cast< uint32_t >( index ), p.seed ^ kSaltPhase ) );

	return p.phase + Mix( even, rand, p.scatter ) * p.spread;
}

float PulseEnvelope( float cyclePhase, float width )
{
	// Clamped rather than guarded against zero: a flash shorter than a fiftieth
	// of a cycle is not a flash anyone can see, and at high phase rates it lands
	// between frames and reads as the pulse randomly failing to happen.
	const float w = std::min( 1.0f, std::max( 0.02f, width ) );
	const float q = Fract( cyclePhase );

	if( q >= w )
		return 0.0f;

	return 0.5f - 0.5f * std::cos( kTau * ( q / w ) );
}

void HsvToRgb( float h, float s, float v, float& r, float& g, float& b )
{
	h = Fract( h ) * 6.0f;
	s = std::min( 1.0f, std::max( 0.0f, s ) );
	v = std::min( 1.0f, std::max( 0.0f, v ) );

	const int sector = static_cast< int >( std::floor( h ) ) % 6;
	const float f    = h - std::floor( h );
	const float p    = v * ( 1.0f - s );
	const float q    = v * ( 1.0f - s * f );
	const float t    = v * ( 1.0f - s * ( 1.0f - f ) );

	switch( sector )
	{
	case 0:  r = v; g = t; b = p; break;
	case 1:  r = q; g = v; b = p; break;
	case 2:  r = p; g = v; b = t; break;
	case 3:  r = p; g = q; b = v; break;
	case 4:  r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
}

void RgbToHsv( float r, float g, float b, float& h, float& s, float& v )
{
	const float maxC = std::max( r, std::max( g, b ) );
	const float minC = std::min( r, std::min( g, b ) );
	const float d    = maxC - minC;

	v = maxC;
	s = maxC > 0.0f ? d / maxC : 0.0f;

	if( d <= 0.0f )
	{
		h = 0.0f;
		return;
	}

	if( maxC == r )
		h = ( g - b ) / d + ( g < b ? 6.0f : 0.0f );
	else if( maxC == g )
		h = ( b - r ) / d + 2.0f;
	else
		h = ( r - g ) / d + 4.0f;

	h /= 6.0f;
}

Instance SolveOne( const MotionParams& p, int index )
{
	Instance out;

	const int count = std::max( 1, p.count );
	const float ip  = InstancePhase( p, index );

	//-----------------------------------------------------------------------
	// Size. Varied downwards from the slider so that turning Variation up never
	// makes anything bigger than the size that was asked for -- a shape that
	// grew past its slider would also grow past the quad it is drawn on.
	//-----------------------------------------------------------------------
	const float sizeRand = Unit( Hash2( static_cast< uint32_t >( index ), p.seed ^ kSaltSize ) );
	float scale          = p.size * ( 1.0f - p.sizeVary * sizeRand );

	float rx = 0.0f;
	float ry = 0.0f;
	FrameRadius( scale, p.aspect, rx, ry );

	//-----------------------------------------------------------------------
	// Position.
	//-----------------------------------------------------------------------
	const float half = p.pathSize * 0.5f;
	float x = p.centreX;
	float y = p.centreY;

	switch( p.path )
	{
	case Path::Orbit:
	{
		const float a = kTau * ip;
		x += half * std::cos( a );
		y += half * std::sin( a );
		break;
	}

	case Path::Lissajous:
	{
		// The quarter-turn on x is what makes ratio 1:1 degenerate to the Orbit
		// circle rather than to a diagonal line. Without it the first thing an
		// operator tries -- dragging both ratios to the bottom -- collapses the
		// whole path to a stripe.
		x += half * std::sin( kTau * p.ratioX * ip + kPi * 0.5f );
		y += half * std::sin( kTau * p.ratioY * ip );
		break;
	}

	case Path::Drift:
	{
		const float c = std::cos( p.direction );
		const float s = std::sin( p.direction );

		// How far the travel axis has to run to carry a shape right across the
		// frame at this heading, plus its own diameter at each end so it enters
		// and leaves off-screen instead of appearing at the edge.
		const float span = std::fabs( c ) + std::fabs( s ) + 2.0f * std::max( rx, ry );

		const float along = ( Fract( ip ) - 0.5f ) * span;

		// Spread across the direction of travel as well as along it, or every
		// shape follows the same single line.
		const float evenCross = ( static_cast< float >( index ) + 0.5f ) / static_cast< float >( count );
		const float randCross = Unit( Hash2( static_cast< uint32_t >( index ), p.seed ^ kSaltCross ) );
		const float across    = ( Mix( evenCross, randCross, p.scatter ) - 0.5f ) * p.pathSize;

		x += c * along - s * across;
		y += s * along + c * across;
		break;
	}

	case Path::Bounce:
	{
		// Inset by the shape's own radius so it turns round when its edge
		// reaches the boundary, not its centre.
		const float boxX = std::max( 0.0f, half - rx );
		const float boxY = std::max( 0.0f, half - ry );

		// With Scatter at zero every instance shares one path and they chase
		// each other round it, which is a usable look in its own right. Scatter
		// detunes the two axes per instance, which is what turns it into the
		// independent wandering people picture when they hear "bounce".
		const float rateX = p.ratioX * ( 1.0f + 0.4f * Signed( Hash2( static_cast< uint32_t >( index ), p.seed ^ kSaltRateX ) ) * p.scatter );
		const float rateY = p.ratioY * ( 1.0f + 0.4f * Signed( Hash2( static_cast< uint32_t >( index ), p.seed ^ kSaltRateY ) ) * p.scatter );

		x += ( Triangle( ip * rateX ) * 2.0f - 1.0f ) * boxX;
		y += ( Triangle( ip * rateY ) * 2.0f - 1.0f ) * boxY;
		break;
	}

	case Path::Grid:
	{
		const int cols = std::max( 1, p.gridCols );
		const int rows = std::max( 1, p.gridRows );

		const int col = index % cols;
		const int row = ( index / cols ) % rows;

		x += ( ( static_cast< float >( col ) + 0.5f ) / static_cast< float >( cols ) - 0.5f ) * p.pathSize;
		y += ( ( static_cast< float >( row ) + 0.5f ) / static_cast< float >( rows ) - 0.5f ) * p.pathSize;
		break;
	}

	default:
		break;
	}

	//-----------------------------------------------------------------------
	// Pulse. Applied on every path, not just Grid -- a pulsing orbit is a
	// perfectly good chase and there is no reason to make it a special case.
	//-----------------------------------------------------------------------
	const float env = PulseEnvelope( ip, p.pulseWidth );
	scale *= Mix( 1.0f, env, p.pulse );

	//-----------------------------------------------------------------------
	// Colour.
	//-----------------------------------------------------------------------
	float r = 1.0f;
	float g = 1.0f;
	float b = 1.0f;

	switch( p.colourMode )
	{
	case ColourMode::White:
		break;

	case ColourMode::Solid:
		r = p.r;
		g = p.g;
		b = p.b;
		break;

	case ColourMode::HueSpread:
	case ColourMode::HueCycle:
	{
		float h = 0.0f;
		float s = 0.0f;
		float v = 0.0f;
		RgbToHsv( p.r, p.g, p.b, h, s, v );

		// The swatch defaults to white, and white has no hue and no saturation
		// -- so a straight reading of it would make both hue modes produce a
		// row of identical white shapes and look completely broken. Treat an
		// achromatic swatch as "full saturation, hue from the spread".
		if( s < 0.01f )
		{
			s = 1.0f;
			if( v < 0.01f )
				v = 1.0f;
		}

		const float across = count > 1 ? static_cast< float >( index ) / static_cast< float >( count ) : 0.0f;
		float hue          = h + p.hueSpread * across;

		if( p.colourMode == ColourMode::HueCycle )
			hue += p.phase;

		HsvToRgb( hue, s, v, r, g, b );
		break;
	}

	default:
		break;
	}

	float alpha = p.opacity * Mix( 1.0f, env, p.pulseBright );

	out.x        = x;
	out.y        = y;
	out.scale    = std::max( 0.0f, scale );
	out.rotation = kTau * ( p.spin * ip + p.spinPhase );
	out.r        = r;
	out.g        = g;
	out.b        = b;
	out.a        = std::min( 1.0f, std::max( 0.0f, alpha ) );

	return out;
}

void Solve( const MotionParams& p, std::vector< Instance >& out )
{
	const int count = std::min( kMaxInstances, std::max( 1, p.count ) );

	out.clear();
	out.reserve( static_cast< size_t >( count ) );

	for( int i = 0; i < count; ++i )
		out.push_back( SolveOne( p, i ) );
}

} // namespace orrery
