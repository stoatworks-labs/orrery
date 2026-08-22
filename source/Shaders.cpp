#include "Shaders.h"

namespace orrery
{
//---------------------------------------------------------------------------
// Background pass
//---------------------------------------------------------------------------
const char* const kBackgroundVertexShader = R"(#version 410 core

out vec2 vUV;

void main()
{
	// Attributeless. A triangle strip of four gives the corners in the order
	// (-1,-1) (1,-1) (-1,1) (1,1), which is what the bit tests below produce.
	vec2 c = vec2( ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0,
	               ( gl_VertexID & 2 ) == 0 ? -1.0 : 1.0 );

	vUV         = c * 0.5 + 0.5;
	gl_Position = vec4( c, 0.0, 1.0 );
}
)";

const char* const kBackgroundFragmentShader = R"(#version 410 core

in vec2 vUV;

uniform vec4 BackColour;

#ifdef ORRERY_EFFECT
uniform sampler2D Clip;
uniform vec2 MaxUV;

// How much of the clip to lay down behind the shapes. 1 for the modes that draw
// over the clip; 1 - Mix for Reveal and Colourise, which build their picture
// only where the shapes are and so have to fade the untouched clip back IN as
// the effect is mixed OUT. At Mix = 1 this is 0 and the background pass clears
// to transparent.
uniform float ClipGain;
#endif

out vec4 fragColor;

void main()
{
#ifdef ORRERY_EFFECT
	// The clip arrives premultiplied and leaves the same way, so scaling both
	// its colour and its alpha by one gain is the whole of the fade.
	fragColor = texture( Clip, vUV * MaxUV ) * ClipGain;
#else
	fragColor = vec4( BackColour.rgb * BackColour.a, BackColour.a );
#endif
}
)";

//---------------------------------------------------------------------------
// Shape pass
//---------------------------------------------------------------------------
//
// The array size is written out as 64 rather than substituted from C++ because
// the shader is a plain string literal. Orrery.cpp carries a static_assert that
// kMaxInstances still equals 64, so raising the C++ constant without raising
// this one is a build error rather than an array overrun.
//
const char* const kShapeVertexShader = R"(#version 410 core

uniform vec4 Xform[ 64 ];   // centre.xy in frame space, radius, rotation
uniform vec4 Tint[ 64 ];    // rgb, alpha
uniform vec2 Resolution;
uniform float Bound;
uniform float Stretch;

out vec2 vLocal;
out vec4 vTint;
out vec2 vClipUV;

void main()
{
	vec2 c = vec2( ( gl_VertexID & 1 ) == 0 ? -1.0 : 1.0,
	               ( gl_VertexID & 2 ) == 0 ? -1.0 : 1.0 );

	vec4 xf = Xform[ gl_InstanceID ];
	vTint   = Tint[ gl_InstanceID ];

	// Shape space: the primitive has unit radius and Stretch widens it on x.
	// The quad has to cover the stretched, rounded, outlined, feathered shape,
	// which is what Bound already accounts for.
	vec2 local = c * vec2( Bound * Stretch, Bound );
	vLocal     = local;

	float ca     = cos( xf.w );
	float sa     = sin( xf.w );
	vec2 rotated = vec2( local.x * ca - local.y * sa,
	                     local.x * sa + local.y * ca );

	// The radius is a fraction of the SHORT edge, expressed here as a fraction
	// of each axis so that a circle comes out round on a 16:9 output. Paths are
	// placed in frame space and shapes are sized in short-edge fractions; those
	// are the same number only on a square render, which is exactly why this
	// looks correct in a 1:1 test and turns into ellipses on a real output if
	// it is dropped.
	vec2 shortEdge = ( Resolution.x >= Resolution.y )
	                 ? vec2( Resolution.y / Resolution.x, 1.0 )
	                 : vec2( 1.0, Resolution.x / Resolution.y );

	vec2 framePos = xf.xy + rotated * xf.z * shortEdge;

	// The clip UV is carried from here rather than recovered from gl_FragCoord
	// in the fragment shader, because gl_FragCoord is in window coordinates and
	// includes the host's viewport offset, which is not always zero.
	vClipUV = vec2( framePos.x, 1.0 - framePos.y );

	// Frame space (0..1, y down) to clip space (-1..1, y up).
	gl_Position = vec4( framePos.x * 2.0 - 1.0, 1.0 - framePos.y * 2.0, 0.0, 1.0 );
}
)";

const char* const kShapeFragmentShader = R"(#version 410 core

in vec2 vLocal;
in vec4 vTint;
in vec2 vClipUV;

uniform int ShapeKind;
uniform float Stretch;
uniform float Roundness;
uniform float Outline;
uniform float Softness;
uniform float Shade;
uniform float LightAngle;
uniform int SampleMode;

#ifdef ORRERY_EFFECT
uniform sampler2D Clip;
uniform vec2 MaxUV;
#endif

out vec4 fragColor;

//---------------------------------------------------------------------------
// Distance functions. All normalised so the shape's furthest point sits at
// radius 1, which is what lets one Bound and one outline width serve all eight.
// The bodies are the standard analytic forms (Inigo Quilez's 2D distance
// function derivations); the normalisation constants are ours.
//---------------------------------------------------------------------------
float sdCircle( vec2 p )
{
	return length( p ) - 1.0;
}

float sdBox( vec2 p, vec2 b )
{
	vec2 q = abs( p ) - b;
	return min( max( q.x, q.y ), 0.0 ) + length( max( q, 0.0 ) );
}

float sdTriangle( vec2 p )
{
	const float k = 1.7320508;   // sqrt(3)
	const float s = 1.1547005;   // 2/sqrt(3): pulls the apex from 2/sqrt(3) to 1

	// Shape space runs y-down to match frame space, and the analytic form is
	// written y-up, so the flip here is what makes the triangle point up on
	// screen rather than down.
	p = vec2( p.x, -p.y ) * s;

	p.x = abs( p.x ) - 1.0;
	p.y = p.y + 1.0 / k;
	if( p.x + k * p.y > 0.0 )
		p = vec2( p.x - k * p.y, -k * p.x - p.y ) * 0.5;
	p.x -= clamp( p.x, -2.0, 0.0 );

	return -length( p ) * sign( p.y ) / s;
}

float sdHexagon( vec2 p )
{
	const vec3 k  = vec3( -0.8660254, 0.5, 0.5773503 );
	const float r = 0.8660254;   // apothem, giving a circumradius of 1

	p = abs( p );
	p -= 2.0 * min( dot( k.xy, p ), 0.0 ) * k.xy;
	p -= vec2( clamp( p.x, -k.z * r, k.z * r ), r );

	return length( p ) * sign( p.y );
}

float sdStar5( vec2 p, float rf )
{
	const vec2 k1 = vec2( 0.809016994, -0.587785252 );
	const vec2 k2 = vec2( -k1.x, k1.y );

	p = vec2( p.x, -p.y );   // point up on screen, as for the triangle

	p.x = abs( p.x );
	p -= 2.0 * max( dot( k1, p ), 0.0 ) * k1;
	p -= 2.0 * max( dot( k2, p ), 0.0 ) * k2;
	p.x = abs( p.x );
	p.y -= 1.0;

	vec2 ba = rf * vec2( -k1.y, k1.x ) - vec2( 0.0, 1.0 );
	float h = clamp( dot( p, ba ) / dot( ba, ba ), 0.0, 1.0 );

	// Negated: this construction is the one shape here whose analytic form comes
	// out POSITIVE inside, and every caller and every downstream step -- the
	// outline's abs(), the feather's smoothstep, the coverage -- assumes
	// negative inside. Left as it stood, the star renders as a solid quad with a
	// star-shaped hole in it, which is a striking picture and entirely wrong.
	return -length( p - ba * h ) * sign( h * ba.x - p.x );
}

float sdCross( vec2 p, vec2 b )
{
	p = abs( p );
	p = ( p.y > p.x ) ? p.yx : p.xy;

	vec2 q  = p - b;
	float k = max( q.y, q.x );
	vec2 w  = ( k > 0.0 ) ? q : vec2( b.y - p.x, -k );

	return sign( k ) * length( max( w, 0.0 ) );
}

float sdRing( vec2 p, float thickness )
{
	// Ring is the one shape where Roundness means thickness rather than corner
	// rounding: a circle has no corners to round, so the slider would otherwise
	// be dead here, and a ring whose weight cannot be set is not much of a ring.
	float t = clamp( thickness, 0.02, 0.9 );
	return abs( length( p ) - ( 1.0 - t * 0.5 ) ) - t * 0.5;
}

float shapeDistance( vec2 p )
{
	// Generic corner rounding: shrink the shape, then dilate the field back out
	// by the same amount. Shrink-then-dilate rather than dilate alone, so the
	// outer extent stays where Bound was told it would be -- a shape that grew
	// past its own quad loses a corner, and a lost corner on a rotating square
	// reads as the shape wobbling rather than as a bounds bug.
	float rr = ( ShapeKind == 6 ) ? 0.0 : clamp( Roundness, 0.0, 1.0 ) * 0.35;
	float k  = 1.0 - rr;

	vec2 q = p / k;
	float d;

	if( ShapeKind == 0 )      d = sdCircle( q );
	else if( ShapeKind == 1 ) d = sdBox( q, vec2( 1.0 ) );
	else if( ShapeKind == 2 ) d = sdTriangle( q );
	else if( ShapeKind == 3 ) d = sdHexagon( q );
	else if( ShapeKind == 4 ) d = sdStar5( q, 0.45 );
	else if( ShapeKind == 5 ) d = sdCross( q, vec2( 1.0, 0.3 ) );
	else if( ShapeKind == 6 ) d = sdRing( q, Roundness );
	else                      d = sdBox( q, vec2( 1.0, 0.15 ) );

	return d * k - rr;
}

void main()
{
	// Undo the stretch to evaluate the primitive, then scale the field by the
	// smaller axis so the result never OVERstates the distance. A conservative
	// field keeps the outline and the feather inside the quad; an optimistic
	// one would let them spill past its edge and get cut off square.
	vec2 st = vec2( Stretch, 1.0 );
	float d0 = shapeDistance( vLocal / st ) * min( st.x, st.y );

	// The antialiasing width is taken from the distance BEFORE the outline's
	// abs(). abs() creases the field at d == 0, which is the centre line of the
	// stroke, and fwidth() of a crease spikes -- giving a dark seam down the
	// middle of every outline. Sampling the width from the smooth field costs
	// nothing and removes the artefact entirely.
	float aa = fwidth( d0 );

	float d = ( Outline > 0.001 ) ? abs( d0 ) - Outline * 0.5 : d0;

	float feather  = max( aa, Softness * 2.0 );
	float coverage = 1.0 - smoothstep( -feather * 0.5, feather * 0.5, d );

	if( coverage <= 0.0 )
		discard;

	vec3 rgb    = vTint.rgb;
	float alpha = coverage * vTint.a;

#ifdef ORRERY_EFFECT
	// 1 = Reveal (the clip, cut to the shapes), 2 = Colourise (the clip tinted).
	// 0 = Over and 3 = Hide both leave the colour alone: Over draws the shape's
	// own colour, and Hide's colour is discarded by the blend function, which
	// uses only the alpha.
	if( SampleMode == 1 || SampleMode == 2 )
	{
		vec4 clip     = texture( Clip, vClipUV * MaxUV );
		vec3 clipRGB  = clip.a > 0.0 ? clip.rgb / clip.a : vec3( 0.0 );

		rgb   = ( SampleMode == 2 ) ? clipRGB * vTint.rgb : clipRGB;
		alpha = alpha * clip.a;
	}
#endif

	//-----------------------------------------------------------------------
	// Shading. Light the flat shape as though it had been inflated.
	//
	// The distance field already knows everything needed for a normal: how far
	// inside the shape a pixel is, and -- through its gradient -- which way the
	// nearest edge lies. In-plane at the rim, facing the viewer in the middle.
	// For a circle that construction IS the sphere normal exactly, which is the
	// shape the request was about; every other primitive gets the same
	// treatment and reads as a pillow rather than a ball, because a pillow is
	// what its field actually describes. A triangle inflates least of the
	// eight, which is correct -- there is less of it to inflate.
	//-----------------------------------------------------------------------
	if( Shade > 0.001 )
	{
		// Central differences rather than fwidth: a screen-space derivative is
		// one value per 2x2 quad, and these shapes are deliberately small, so
		// the facets would be plainly visible.
		const float h = 0.01;
		float gx = shapeDistance( vec2( vLocal.x + h, vLocal.y ) / st )
		         - shapeDistance( vec2( vLocal.x - h, vLocal.y ) / st );
		float gy = shapeDistance( vec2( vLocal.x, vLocal.y + h ) / st )
		         - shapeDistance( vec2( vLocal.x, vLocal.y - h ) / st );

		vec2 g       = vec2( gx, gy );
		float glen   = length( g );
		vec2 outward = ( glen > 1e-6 ) ? g / glen : vec2( 0.0 );

		// 1 at the rim, 0 at the deepest point inside. The reach is the shape's
		// own unit radius carried through the stretch, so a squashed shape
		// inflates by as much as it is wide rather than always by the same
		// amount.
		float reach = max( min( st.x, st.y ), 1e-4 );
		float s     = clamp( 1.0 + d0 / reach, 0.0, 1.0 );

		vec3 normal = vec3( outward * s, sqrt( max( 0.0, 1.0 - s * s ) ) );

		// The y flip is because shape space runs y-down to match frame space:
		// without it, raising the Light control walks the highlight the wrong
		// way round the shape.
		float a     = LightAngle * 6.28318531;
		vec3 toLight = normalize( vec3( cos( a ), -sin( a ), 0.65 ) );

		float lambert = max( dot( normal, toLight ), 0.0 );
		// Not `half`: that is a reserved word in GLSL and the compiler's
		// message about it is not a helpful one.
		vec3 halfway  = normalize( toLight + vec3( 0.0, 0.0, 1.0 ) );
		float spec    = pow( max( dot( normal, halfway ), 0.0 ), 24.0 );

		// The ambient term is not politeness: these shapes are frequently the
		// source for a pixel map, and a fixture that goes to black on the far
		// side of the light reads as a dead channel rather than as shading.
		vec3 lit = rgb * ( 0.35 + 0.75 * lambert ) + vec3( 0.5 * spec );

		rgb = mix( rgb, lit, clamp( Shade, 0.0, 1.0 ) );
	}

	fragColor = vec4( rgb * alpha, alpha );
}
)";

const char* const kEffectDefine = "#define ORRERY_EFFECT 1\n";

} // namespace orrery
