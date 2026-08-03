#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace orrery
{
namespace
{
constexpr float kTau = 6.28318530717958647692f;

float Clamp01( float v )
{
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

/// Exponential map from 0..1 onto lo..hi. Both ends must be positive.
float Exponential( float v, float lo, float hi )
{
	return lo * std::pow( hi / lo, Clamp01( v ) );
}

} // namespace

const char* BlendName( Blend blend )
{
	switch( blend )
	{
	case Blend::Over: return "Over";
	case Blend::Add:  return "Add";
	case Blend::Max:  return "Max";
	default:          return "Over";
	}
}

const char* MaskModeName( MaskMode mode )
{
	switch( mode )
	{
	case MaskMode::Over:      return "Over";
	case MaskMode::Reveal:    return "Reveal";
	case MaskMode::Hide:      return "Hide";
	case MaskMode::Colourise: return "Colourise";
	default:                  return "Over";
	}
}

const char* SyncName( Sync sync )
{
	switch( sync )
	{
	case Sync::Free:   return "Free";
	case Sync::Beat:   return "Beat";
	case Sync::Bar:    return "Bar";
	case Sync::Manual: return "Manual";
	default:           return "Free";
	}
}

int InstancesFromParam( float value )
{
	const float v = Clamp01( value );
	return 1 + static_cast< int >( std::lround( 63.0f * v * v ) );
}

float SizeFromParam( float value )
{
	return Exponential( value, 0.005f, 0.5f );
}

float StretchFromParam( float value )
{
	// pow( 10, 2v - 1 ): 0.1 at the bottom, exactly 1 at the middle, 10 at the
	// top. The midpoint is exact because 2 * 0.5 - 1 is exactly 0 in binary
	// floating point, which is the only reason a "1 at the centre" slider can be
	// trusted to actually reach 1.
	return std::pow( 10.0f, 2.0f * Clamp01( value ) - 1.0f );
}

float SoftnessFromParam( float value )
{
	return Clamp01( value ) * 0.5f;
}

float SpeedFromParam( float value )
{
	const float v = Clamp01( value );

	// A dead zone rather than a curve that merely approaches zero: "stopped" has
	// to be a place the operator can land on, and 0.0001 cycles per second is
	// not stopped, it is a shape that has moved somewhere unexpected by the
	// second act.
	if( v < 0.02f )
		return 0.0f;

	return Exponential( ( v - 0.02f ) / 0.98f, 0.01f, 2.0f );
}

uint32_t SeedFromParam( float value )
{
	return 1u + static_cast< uint32_t >( std::lround( Clamp01( value ) * 9998.0f ) );
}

float PathSizeFromParam( float value )
{
	return Clamp01( value ) * 1.5f;
}

float CentreFromParam( float value )
{
	return Clamp01( value ) * 1.5f - 0.25f;
}

float RatioFromParam( float value )
{
	return Exponential( value, 0.5f, 8.0f );
}

float DirectionFromParam( float value )
{
	return Clamp01( value ) * kTau;
}

int GridFromParam( float value )
{
	return 1 + static_cast< int >( std::lround( Clamp01( value ) * 15.0f ) );
}

float SpinFromParam( float value )
{
	return ( 2.0f * Clamp01( value ) - 1.0f ) * 4.0f;
}

float PulseWidthFromParam( float value )
{
	return 0.02f + Clamp01( value ) * 0.98f;
}

float HueSpreadFromParam( float value )
{
	return Clamp01( value );
}

} // namespace orrery
