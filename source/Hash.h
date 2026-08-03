#pragma once

#include <cstdint>

/**
    An exact integer hash.

    Every "random" quantity in Orrery -- an instance's scatter offset, its size
    variation, its bounce frequencies -- comes from here, and the reason it is an
    integer hash rather than the usual `fract( sin( x ) * 43758.5453 )` is that
    the usual one is transcendental. Its result differs between GPUs, between
    drivers, and between a GPU and a CPU. Orrery solves its motion on the CPU
    (see Motion.h) so it never has to agree with a shader -- but it does have to
    agree with **itself on another machine**. A composition built on the show
    laptop and opened on the rack machine has to scatter its shapes to the same
    places, and `lowbias32` is exact everywhere while a sine is not.

    `lowbias32` is Chris Wellons' 32-bit integer bijection, chosen for having
    about the lowest avalanche bias of any two-round xorshift-multiply.
*/
namespace orrery
{
inline uint32_t Hash32( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

/// Mix two values into one hash. Used as Hash2( instanceIndex, seed ) so that
/// nudging the seed reshuffles every instance rather than rotating the set.
inline uint32_t Hash2( uint32_t a, uint32_t b )
{
	return Hash32( a ^ Hash32( b + 0x9e3779b9U ) );
}

/// A hash to 0..1.
///
/// Takes the **top 24 bits**. That is the widest slice that converts to a
/// float32 without rounding -- a float32 has a 24-bit significand -- so the
/// conversion is exact and two machines cannot disagree in the last bit.
inline float Unit( uint32_t h )
{
	return static_cast< float >( h >> 8 ) * ( 1.0f / 16777216.0f );
}

/// A hash to -1..1.
inline float Signed( uint32_t h )
{
	return Unit( h ) * 2.0f - 1.0f;
}

} // namespace orrery
