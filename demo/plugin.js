/**
 * Orrery — browser demo.
 *
 * The four shaders below are copied unedited from `source/Shaders.cpp`,
 * including the `#ifdef ORRERY_EFFECT` branches — the two bundles really are one
 * pair of shaders compiled twice, and the picker in the transport bar compiles
 * them both ways here for the same reason. `withDefines()` inserts the define
 * after the `#version` line exactly as `Orrery.cpp` does.
 *
 * `Motion.cpp` is ported below in full, with `Hash.h`, `Shapes.cpp`'s bounds and
 * `Controls.cpp`'s 0..1 conversions. That port is the interesting part of this
 * page, because it is the plugin's whole design:
 *
 * **An instance's placement is a pure function of (index, phase).** No velocity
 * is integrated, no previous position is kept, no feedback texture exists. A
 * bounce is a triangle wave rather than a collision test; a drift is a wrap
 * rather than an accumulation. So a chase cannot slow down when the host's frame
 * rate drops — which is the thing that matters when the output is driving lights
 * rather than a screen — and any frame renders on its own, which is why Step on
 * this page is exact.
 *
 * There is no GLSL copy of the motion in the plugin and there is none here: it
 * runs once per instance rather than once per pixel, so 64 solves on the CPU and
 * two `vec4` uniform arrays do the job, and there is only ever one copy of the
 * maths to be wrong.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, bindTexture } from './vendor/gl.js';

//===========================================================================
// The shaders. Copied from source/Shaders.cpp.
//===========================================================================

const BACKGROUND_VERTEX_SHADER = `#version 410 core

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
`;

const BACKGROUND_FRAGMENT_SHADER = `#version 410 core

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
`;

const SHAPE_VERTEX_SHADER = `#version 410 core

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
`;

const SHAPE_FRAGMENT_SHADER = `#version 410 core

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
`;

const EFFECT_DEFINE = '#define ORRERY_EFFECT 1\n';

/// Insert defines after the `#version` line, which must be first in a GLSL
/// source. The same function `Orrery.cpp` uses, for the same reason.
function withDefines(shader, defines) {
  if (!defines) return shader;
  const afterVersion = shader.indexOf('\n');
  if (afterVersion === -1) return shader;
  return shader.slice(0, afterVersion + 1) + defines + shader.slice(afterVersion + 1);
}

//===========================================================================
// Port of source/Hash.h
//
// An exact integer hash, not `fract( sin( x ) * 43758.5453 )`. Orrery solves its
// motion on the CPU so it never has to agree with a shader — but it does have to
// agree with itself on another machine, and a composition built on the show
// laptop has to scatter its shapes to the same places on the rack machine.
// `lowbias32` is exact everywhere; a sine is not.
//===========================================================================

function hash32(x) {
  x = x >>> 0;
  x ^= x >>> 16;
  x = Math.imul(x, 0x7feb352d) >>> 0;
  x ^= x >>> 15;
  x = Math.imul(x, 0x846ca68b) >>> 0;
  x ^= x >>> 16;
  return x >>> 0;
}

/// Hash2( instanceIndex, seed ), so nudging the seed reshuffles every instance
/// rather than rotating the set.
function hash2(a, b) {
  return hash32((a >>> 0) ^ hash32((b + 0x9e3779b9) >>> 0));
}

/// Takes the top 24 bits — the widest slice that converts to a float32 without
/// rounding, so two machines cannot disagree in the last bit.
const unit = (h) => (h >>> 8) * (1 / 16777216);
const signed = (h) => unit(h) * 2 - 1;

//===========================================================================
// Port of source/Controls.cpp
//===========================================================================

const clamp01 = (v) => Math.min(1, Math.max(0, v));
const TAU = 6.28318530717958647692;
const PI = 3.14159265358979323846;

/// Exponential map from 0..1 onto lo..hi. Both ends must be positive.
const exponential = (v, lo, hi) => lo * Math.pow(hi / lo, clamp01(v));

const instancesFromParam = (v) => 1 + Math.round(63 * clamp01(v) * clamp01(v));
const sizeFromParam = (v) => exponential(v, 0.005, 0.5);

/// pow(10, 2v-1): 0.1 at the bottom, exactly 1 at the middle, 10 at the top. The
/// midpoint is exact because 2 × 0.5 − 1 is exactly 0 in binary floating point,
/// which is the only reason a "1 at the centre" slider can be trusted to reach 1.
const stretchFromParam = (v) => Math.pow(10, 2 * clamp01(v) - 1);
const softnessFromParam = (v) => clamp01(v) * 0.5;

/// A dead zone rather than a curve that merely approaches zero: "stopped" has to
/// be a place the operator can land on, and 0.0001 cycles per second is not
/// stopped, it is a shape that has moved somewhere unexpected by the second act.
function speedFromParam(value) {
  const v = clamp01(value);
  if (v < 0.02) return 0;
  return exponential((v - 0.02) / 0.98, 0.01, 2.0);
}

const seedFromParam = (v) => 1 + Math.round(clamp01(v) * 9998);
const pathSizeFromParam = (v) => clamp01(v) * 1.5;
const centreFromParam = (v) => clamp01(v) * 1.5 - 0.25;
const ratioFromParam = (v) => exponential(v, 0.5, 8.0);
const directionFromParam = (v) => clamp01(v) * TAU;
const gridFromParam = (v) => 1 + Math.round(clamp01(v) * 15);
const spinFromParam = (v) => (2 * clamp01(v) - 1) * 4;
const pulseWidthFromParam = (v) => 0.02 + clamp01(v) * 0.98;
const hueSpreadFromParam = (v) => clamp01(v);

//===========================================================================
// Port of source/Shapes.cpp
//
// Only the bound. The shader owns the distance functions and there is no
// mirrored copy of them anywhere — the harness checks where each shape landed
// and how much of the frame it covered, not what its edge did pixel by pixel.
//===========================================================================

const SHAPE_NAMES = ['Circle', 'Square', 'Triangle', 'Hexagon', 'Star', 'Cross', 'Ring', 'Bar'];

/**
 * How far the shape reaches from its own centre at unit radius, before rounding
 * and outline. This sizes the quad, and it is the one number here that can
 * produce a visibly wrong picture if it is too small: a shape that reaches past
 * its own quad is not clipped in a way that looks like clipping — it loses a
 * corner, and a lost corner on a rotating square reads as the shape wobbling.
 */
function shapeBound(shape) {
  switch (shape) {
    case 0: // Circle — inscribed in the unit circle by construction
    case 4: // Star
    case 6: // Ring
      return 1.0;
    // The square SDF is built from a half-extent of 1 on each axis, so the
    // corners sit at sqrt(2). At 45 degrees the corners are what you see.
    case 1:
      return 1.4143;
    case 2: // Equilateral triangle with circumradius 1
    case 3: // Flat-topped hexagon with circumradius 1
      return 1.0;
    case 5: // Cross — the arms' corners sit a little beyond 1
    case 7: // Bar — a 1 x 0.15 box, stretched on the long axis in the shader
      return 1.05;
    default:
      return 1.4143;
  }
}

//===========================================================================
// Port of source/Motion.cpp
//
// Two coordinate conventions, on purpose: paths are placed in FRAME space
// (0..1 per axis, y down), shapes are sized in SHORT-EDGE fractions. Mixing them
// up is the one geometric mistake available here, and it is invisible at 1:1 —
// everything looks right on a square render and turns into ellipses the moment
// it reaches a real output.
//===========================================================================

const MAX_INSTANCES = 64;

const PATH_NAMES = ['Orbit', 'Lissajous', 'Drift', 'Bounce', 'Grid'];
const COLOUR_MODE_NAMES = ['White', 'Solid', 'Hue Spread', 'Hue Cycle'];
const BLEND_NAMES = ['Over', 'Add', 'Max'];
const MASK_MODE_NAMES = ['Over', 'Reveal', 'Hide', 'Colourise'];
const SYNC_NAMES = ['Free', 'Beat', 'Bar', 'Manual'];

// Distinct salts so that two quantities drawn for the same instance from the
// same seed are independent. Reusing one salt is not a crash, it is a
// correlation: the biggest shape would always be the one furthest left.
const SALT_PHASE = 0x00000000;
const SALT_CROSS = 0x5bf03635;
const SALT_SIZE = 0x27d4eb2f;
const SALT_RATE_X = 0x165667b1;
const SALT_RATE_Y = 0x9e3779b9;

const fract = (x) => x - Math.floor(x);
const mix = (a, b, t) => a + (b - a) * t;

/// A triangle wave: period 2, range 0..1, tri(0) == 0.
///
/// This is the whole of the bounce. A shape travelling at a constant speed and
/// reflecting off two walls *is* a triangle wave — there is no approximation
/// here and no reason to integrate a velocity to discover it.
function triangle(x) {
  const t = fract(x * 0.5);
  return t < 0.5 ? t * 2 : 2 - t * 2;
}

/// The shape's radius expressed as a fraction of each axis of the frame.
function frameRadius(scale, aspect) {
  if (aspect >= 1) return [scale / aspect, scale]; // landscape: short edge is height
  return [scale, scale * aspect];
}

function instancePhase(p, index) {
  const count = Math.max(1, p.count);
  // The even spread is what makes a chase: consecutive instances sit a fixed
  // distance apart in phase, so they arrive in order.
  const even = count > 1 ? index / count : 0;
  const rand = unit(hash2(index, (p.seed ^ SALT_PHASE) >>> 0));
  return p.phase + mix(even, rand, p.scatter) * p.spread;
}

/**
 * A raised cosine occupying the first `width` of each cycle, zero for the rest.
 * A raised cosine rather than a saw because a saw's discontinuity at the top of
 * the cycle is visible as a click on a lighting rig — a fixture jumping from
 * black to full in one frame reads as a glitch, not a beat.
 */
function pulseEnvelope(cyclePhase, width) {
  // Clamped rather than guarded against zero: a flash shorter than a fiftieth of
  // a cycle is not a flash anyone can see, and at high phase rates it lands
  // between frames and reads as the pulse randomly failing to happen.
  const w = Math.min(1, Math.max(0.02, width));
  const q = fract(cyclePhase);
  if (q >= w) return 0;
  return 0.5 - 0.5 * Math.cos(TAU * (q / w));
}

function hsvToRgb(h, s, v) {
  h = fract(h) * 6;
  s = Math.min(1, Math.max(0, s));
  v = Math.min(1, Math.max(0, v));

  const sector = Math.floor(h) % 6;
  const f = h - Math.floor(h);
  const p = v * (1 - s);
  const q = v * (1 - s * f);
  const t = v * (1 - s * (1 - f));

  switch (sector) {
    case 0: return [v, t, p];
    case 1: return [q, v, p];
    case 2: return [p, v, t];
    case 3: return [p, q, v];
    case 4: return [t, p, v];
    default: return [v, p, q];
  }
}

function rgbToHsv(r, g, b) {
  const maxC = Math.max(r, g, b);
  const minC = Math.min(r, g, b);
  const d = maxC - minC;

  const v = maxC;
  const s = maxC > 0 ? d / maxC : 0;

  if (d <= 0) return [0, s, v];

  let h;
  if (maxC === r) h = (g - b) / d + (g < b ? 6 : 0);
  else if (maxC === g) h = (b - r) / d + 2;
  else h = (r - g) / d + 4;

  return [h / 6, s, v];
}

/// One shape, placed. `out` is a flat [x, y, scale, rotation, r, g, b, a].
function solveOne(p, index) {
  const count = Math.max(1, p.count);
  const ip = instancePhase(p, index);

  //-----------------------------------------------------------------------
  // Size. Varied downwards from the slider so that turning Variation up never
  // makes anything bigger than the size that was asked for — a shape that grew
  // past its slider would also grow past the quad it is drawn on.
  //-----------------------------------------------------------------------
  const sizeRand = unit(hash2(index, (p.seed ^ SALT_SIZE) >>> 0));
  let scale = p.size * (1 - p.sizeVary * sizeRand);

  const [rx, ry] = frameRadius(scale, p.aspect);

  //-----------------------------------------------------------------------
  // Position.
  //-----------------------------------------------------------------------
  const half = p.pathSize * 0.5;
  let x = p.centreX;
  let y = p.centreY;

  if (p.path === 0) {
    // Orbit
    const a = TAU * ip;
    x += half * Math.cos(a);
    y += half * Math.sin(a);
  } else if (p.path === 1) {
    // Lissajous. The quarter-turn on x is what makes ratio 1:1 degenerate to the
    // Orbit circle rather than to a diagonal line. Without it the first thing an
    // operator tries — dragging both ratios to the bottom — collapses the whole
    // path to a stripe.
    x += half * Math.sin(TAU * p.ratioX * ip + PI * 0.5);
    y += half * Math.sin(TAU * p.ratioY * ip);
  } else if (p.path === 2) {
    // Drift
    const c = Math.cos(p.direction);
    const s = Math.sin(p.direction);

    // How far the travel axis has to run to carry a shape right across the frame
    // at this heading, plus its own diameter at each end so it enters and leaves
    // off-screen instead of appearing at the edge.
    const span = Math.abs(c) + Math.abs(s) + 2 * Math.max(rx, ry);
    const along = (fract(ip) - 0.5) * span;

    // Spread across the direction of travel as well as along it, or every shape
    // follows the same single line.
    const evenCross = (index + 0.5) / count;
    const randCross = unit(hash2(index, (p.seed ^ SALT_CROSS) >>> 0));
    const across = (mix(evenCross, randCross, p.scatter) - 0.5) * p.pathSize;

    x += c * along - s * across;
    y += s * along + c * across;
  } else if (p.path === 3) {
    // Bounce. Inset by the shape's own radius so it turns round when its edge
    // reaches the boundary, not its centre.
    const boxX = Math.max(0, half - rx);
    const boxY = Math.max(0, half - ry);

    // With Scatter at zero every instance shares one path and they chase each
    // other round it, which is a usable look in its own right. Scatter detunes
    // the two axes per instance, which is what turns it into the independent
    // wandering people picture when they hear "bounce".
    const rateX = p.ratioX * (1 + 0.4 * signed(hash2(index, (p.seed ^ SALT_RATE_X) >>> 0)) * p.scatter);
    const rateY = p.ratioY * (1 + 0.4 * signed(hash2(index, (p.seed ^ SALT_RATE_Y) >>> 0)) * p.scatter);

    x += (triangle(ip * rateX) * 2 - 1) * boxX;
    y += (triangle(ip * rateY) * 2 - 1) * boxY;
  } else if (p.path === 4) {
    // Grid
    const cols = Math.max(1, p.gridCols);
    const rows = Math.max(1, p.gridRows);

    const col = index % cols;
    const row = Math.floor(index / cols) % rows;

    x += ((col + 0.5) / cols - 0.5) * p.pathSize;
    y += ((row + 0.5) / rows - 0.5) * p.pathSize;
  }

  //-----------------------------------------------------------------------
  // Pulse. Applied on every path, not just Grid — a pulsing orbit is a
  // perfectly good chase and there is no reason to make it a special case.
  //-----------------------------------------------------------------------
  const env = pulseEnvelope(ip, p.pulseWidth);
  scale *= mix(1, env, p.pulse);

  //-----------------------------------------------------------------------
  // Audio. Size grows rather than modulates about 1: the quiet state is the
  // shape the operator set, and the music only ever adds.
  //-----------------------------------------------------------------------
  const band = p.audio[index % p.audio.length];
  scale *= 1 + p.audioSize * 2 * band;

  //-----------------------------------------------------------------------
  // Colour.
  //-----------------------------------------------------------------------
  let r = 1;
  let g = 1;
  let b = 1;

  if (p.colourMode === 1) {
    r = p.r;
    g = p.g;
    b = p.b;
  } else if (p.colourMode === 2 || p.colourMode === 3) {
    let [h, s, v] = rgbToHsv(p.r, p.g, p.b);

    // The swatch defaults to white, and white has no hue and no saturation — so
    // a straight reading of it would make both hue modes produce a row of
    // identical white shapes and look completely broken. Treat an achromatic
    // swatch as "full saturation, hue from the spread".
    if (s < 0.01) {
      s = 1;
      if (v < 0.01) v = 1;
    }

    const across = count > 1 ? index / count : 0;
    let hue = h + p.hueSpread * across;
    if (p.colourMode === 3) hue += p.phase;

    [r, g, b] = hsvToRgb(hue, s, v);
  }

  const alpha = p.opacity * mix(1, env, p.pulseBright) * mix(1, band, p.audioBright);

  return {
    x,
    y,
    scale: Math.max(0, scale),
    rotation: TAU * (p.spin * ip + p.spinPhase),
    r,
    g,
    b,
    a: Math.min(1, Math.max(0, alpha)),
  };
}

//===========================================================================
// Port of source/Presets.h
//===========================================================================

/// The parameters a preset sets, in the fixed order `presets::Param` declares.
const PRESET_PARAM_IDS = [
  'shape', 'count', 'size', 'sizeVary', 'stretch', 'roundness', 'outline', 'softness',
  'path', 'speed', 'spread', 'scatter', 'pathSize', 'ratioX', 'ratioY', 'direction',
  'gridCols', 'gridRows', 'spin', 'spinPhase',
  'pulse', 'pulseBright', 'pulseWidth',
  'colourMode', 'shapeR', 'shapeG', 'shapeB', 'hueSpread', 'opacity',
  'backR', 'backG', 'backB', 'backOpacity', 'blend',
];

const PRESET_TABLE = [
  ['Orbiting Dots', [0, 0.333, 0.54, 0.0, 0.5, 0.0, 0.0, 0.0, 0, 0.521, 1.0, 0.0,
    0.467, 0.646, 0.5, 0.0, 0.2, 0.1333, 0.5, 0.0, 0.0, 0.0, 0.5,
    0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0]],
  ['Scanner Bar', [7, 0.0, 0.8, 0.0, 0.5, 0.0, 0.0, 0.15, 3, 0.45, 1.0, 0.0,
    0.6, 0.646, 0.5, 0.0, 0.2, 0.1333, 0.5, 0.0, 0.0, 0.0, 0.5,
    0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 2]],
  ['Grid Chase', [0, 0.418, 0.4, 0.0, 0.5, 0.0, 0.0, 0.0, 4, 0.5, 1.0, 0.0,
    0.467, 0.646, 0.5, 0.0, 0.2, 0.1333, 0.5, 0.0, 0.6, 0.3, 0.3,
    0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 2]],
  ['Breathing Ring', [6, 0.0, 0.8, 0.0, 0.5, 0.0, 0.0, 0.25, 0, 0.4, 1.0, 0.0,
    0.0, 0.646, 0.5, 0.0, 0.2, 0.1333, 0.5, 0.0, 0.8, 0.5, 0.6,
    0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 2]],
  ['Star Confetti', [4, 0.702, 0.35, 0.6, 0.5, 0.1, 0.0, 0.0, 2, 0.5, 1.0, 1.0,
    0.6, 0.646, 0.5, 0.0, 0.2, 0.1333, 0.7, 0.0, 0.0, 0.0, 0.5,
    2, 1.0, 0.3, 0.3, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0]],
  ['Lissajous Trace', [0, 0.488, 0.42, 0.0, 0.5, 0.0, 0.0, 0.1, 1, 0.45, 0.25, 0.0,
    0.55, 0.646, 0.5, 0.0, 0.2, 0.1333, 0.5, 0.0, 0.0, 0.0, 0.5,
    3, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1]],
];

const PRESETS = Object.fromEntries(
  PRESET_TABLE.map(([name, values]) => [
    name,
    Object.fromEntries(values.map((value, i) => [PRESET_PARAM_IDS[i], value])),
  ]),
);

//===========================================================================
// The renderer. Two passes, no framebuffer.
//===========================================================================

/// The spectrum, all zeros. Resolume writes one bin per element from whatever
/// audio is routed; a browser page has no host FFT, and the plugin's own element
/// defaults are zero for the same reason.
const AUDIO_BINS = new Float32Array(MAX_INSTANCES);

class OrreryRenderer {
  constructor(gl) {
    this.gl = gl;

    // Both variants, compiled up front, because the picker switches between them
    // without a page reload.
    this.programs = {
      source: {
        background: new Program(gl, BACKGROUND_VERTEX_SHADER, BACKGROUND_FRAGMENT_SHADER, 'background'),
        shape: new Program(gl, SHAPE_VERTEX_SHADER, SHAPE_FRAGMENT_SHADER, 'shape'),
      },
      effect: {
        background: new Program(
          gl,
          BACKGROUND_VERTEX_SHADER,
          withDefines(BACKGROUND_FRAGMENT_SHADER, EFFECT_DEFINE),
          'background (effect)',
        ),
        shape: new Program(
          gl,
          SHAPE_VERTEX_SHADER,
          withDefines(SHAPE_FRAGMENT_SHADER, EFFECT_DEFINE),
          'shape (effect)',
        ),
      },
    };

    // A core profile refuses to draw with no vertex array bound, even though
    // both shaders build their geometry from gl_VertexID and source nothing.
    this.emptyVAO = gl.createVertexArray();

    this.xform = new Float32Array(MAX_INSTANCES * 4);
    this.tint = new Float32Array(MAX_INSTANCES * 4);
  }

  /**
   * Free multiplies the clock by the rate — the phase is absolute rather than
   * integrated, which is what makes any frame renderable on its own. Manual
   * ignores Speed entirely: that mode is for driving Phase from a keyframe or a
   * MIDI fader, and a second clock underneath would fight whatever is driving it.
   */
  phaseAt(params, time) {
    const sync = params.option('sync');
    const speed = speedFromParam(params.get('speed'));
    const manual = params.get('phase');

    let driven = 0;
    if (sync === 0) {
      driven = time * speed;
    } else if (sync === 1 || sync === 2) {
      // This page has no transport, so it generates one at 120 BPM — the tempo
      // the plugin falls back to when a host reports none.
      const barSeconds = 240 / 120;
      const bars = time / barSeconds;
      driven = (sync === 1 ? bars * 4 : bars) * speed;
    }

    return driven + manual;
  }

  /// CurrentMotion(): the host's 0..1 sliders as physical quantities.
  motionFrom(params, width, height, time, isEffect) {
    const count = instancesFromParam(params.get('count'));

    // One broad band per instance rather than the first `count` bins: six shapes
    // should carve the whole spectrum into six, not all sit on the bass.
    const audio = new Float32Array(MAX_INSTANCES);
    for (let i = 0; i < count; i += 1) {
      const lo = Math.floor((i * MAX_INSTANCES) / count);
      const hi = Math.max(lo + 1, Math.floor(((i + 1) * MAX_INSTANCES) / count));
      let sum = 0;
      for (let b = lo; b < hi; b += 1) sum += AUDIO_BINS[b];
      audio[i] = sum / (hi - lo);
    }

    return {
      shape: params.option('shape'),
      path: params.option('path'),
      count,
      phase: this.phaseAt(params, time),

      spread: clamp01(params.get('spread')),
      scatter: clamp01(params.get('scatter')),
      seed: seedFromParam(params.get('seed')),

      pathSize: pathSizeFromParam(params.get('pathSize')),
      centreX: centreFromParam(params.get('centreX')),
      centreY: centreFromParam(params.get('centreY')),
      ratioX: ratioFromParam(params.get('ratioX')),
      ratioY: ratioFromParam(params.get('ratioY')),
      direction: directionFromParam(params.get('direction')),
      gridCols: gridFromParam(params.get('gridCols')),
      gridRows: gridFromParam(params.get('gridRows')),

      size: sizeFromParam(params.get('size')),
      sizeVary: clamp01(params.get('sizeVary')),

      spin: spinFromParam(params.get('spin')),
      spinPhase: clamp01(params.get('spinPhase')),

      pulse: clamp01(params.get('pulse')),
      pulseBright: clamp01(params.get('pulseBright')),
      pulseWidth: pulseWidthFromParam(params.get('pulseWidth')),

      audioSize: clamp01(params.get('audioSize')),
      audioBright: clamp01(params.get('audioBright')),
      audio,

      colourMode: params.option('colourMode'),
      r: clamp01(params.get('shapeR')),
      g: clamp01(params.get('shapeG')),
      b: clamp01(params.get('shapeB')),
      hueSpread: hueSpreadFromParam(params.get('hueSpread')),

      // Mix fades the shape layer. The source has no clip to mix against, so it
      // ignores it — the parameter exists in both only so that a composition
      // moved from one plugin to the other does not find its list has shifted.
      opacity: clamp01(params.get('opacity')) * (isEffect ? clamp01(params.get('mix')) : 1),

      aspect: width > 0 && height > 0 ? width / height : 1,
    };
  }

  applyBlend(blend) {
    const gl = this.gl;
    gl.enable(gl.BLEND);

    if (blend === 1) {
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFunc(gl.ONE, gl.ONE);
    } else if (blend === 2) {
      // MAX ignores the factors entirely and takes the channel-wise maximum of
      // source and destination, alpha included. That is what a mask wants: two
      // overlapping white shapes stay white instead of clipping to a brighter
      // white that is no longer the same colour as either of them.
      gl.blendEquation(gl.MAX);
      gl.blendFunc(gl.ONE, gl.ONE);
    } else {
      // Premultiplied over, matching the shader's output.
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    }
  }

  render({ input, params, width, height, time, variant }) {
    const gl = this.gl;
    const isEffect = variant === 'effect';
    const programs = isEffect ? this.programs.effect : this.programs.source;

    const motion = this.motionFrom(params, width, height, time, isEffect);
    const count = Math.min(MAX_INSTANCES, Math.max(1, motion.count));

    const mask = isEffect ? params.option('maskMode') : 0;
    const blend = params.option('blend');
    const mix01 = clamp01(params.get('mix'));

    // Reveal and Colourise build their picture only where the shapes are, so the
    // clip behind them fades IN as the effect mixes OUT.
    const buildsFromClip = mask === 1 || mask === 3;
    const clipGain = buildsFromClip ? 1 - mix01 : 1;

    const sampleMode = mask === 1 ? 1 : mask === 3 ? 2 : mask === 2 ? 3 : 0;

    gl.bindVertexArray(this.emptyVAO);

    //---------------------------------------------------------------------
    // Pass 1: the background.
    //---------------------------------------------------------------------
    gl.disable(gl.BLEND);
    const background = programs.background.use();

    if (isEffect) {
      bindTexture(gl, 0, input.texture);
      background.setSampler('Clip', 0);
      // The kit hands over a texture the picture exactly fills, so MaxUV is the
      // whole of it. In Resolume it is whatever GetMaxGLTexCoords reports.
      background.set('MaxUV', 1, 1);
      background.set('ClipGain', clipGain);
    } else {
      background.set(
        'BackColour',
        clamp01(params.get('backR')),
        clamp01(params.get('backG')),
        clamp01(params.get('backB')),
        clamp01(params.get('backOpacity')),
      );
    }

    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    //---------------------------------------------------------------------
    // Pass 2: the shapes. One attributeless quad per instance, rasterised in
    // index order, so ordinary alpha blending composites them in that order and
    // the three blend modes are three pieces of GL state rather than three
    // branches inside a loop.
    //---------------------------------------------------------------------
    const shape = programs.shape.use();

    for (let i = 0; i < count; i += 1) {
      const inst = solveOne(motion, i);
      const x = i * 4;
      this.xform[x + 0] = inst.x;
      this.xform[x + 1] = inst.y;
      this.xform[x + 2] = inst.scale;
      this.xform[x + 3] = inst.rotation;
      this.tint[x + 0] = inst.r;
      this.tint[x + 1] = inst.g;
      this.tint[x + 2] = inst.b;
      this.tint[x + 3] = inst.a;
    }

    shape.setArray('Xform', this.xform.subarray(0, count * 4), 4);
    shape.setArray('Tint', this.tint.subarray(0, count * 4), 4);

    const outline = clamp01(params.get('outline'));
    const softness = softnessFromParam(params.get('softness'));
    const stretch = stretchFromParam(params.get('stretch'));

    // The quad has to contain the shape, its outline, and its feather. Slack on
    // top because overdrawing a few pixels costs nothing and a shape that
    // reaches past its quad silently loses a corner.
    const bound = shapeBound(motion.shape) + outline * 0.5 + softness * 2 + 0.05;

    shape.set('Resolution', width, height);
    shape.set('Bound', bound);
    shape.set('Stretch', stretch);
    shape.setInt('ShapeKind', motion.shape);
    shape.set('Roundness', clamp01(params.get('roundness')));
    shape.set('Outline', outline);
    shape.set('Softness', softness);
    shape.set('Shade', clamp01(params.get('shade')));
    shape.set('LightAngle', clamp01(params.get('light')));
    shape.setInt('SampleMode', sampleMode);

    if (isEffect) {
      bindTexture(gl, 0, input.texture);
      shape.setSampler('Clip', 0);
      shape.set('MaxUV', 1, 1);
    }

    if (mask === 2) {
      // Hide: punch the shapes out of what is already there — keep no source,
      // and scale the destination by the shape's transparency. This is the whole
      // of Hide, and it is why there is no mask buffer anywhere in this plugin.
      gl.enable(gl.BLEND);
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFunc(gl.ZERO, gl.ONE_MINUS_SRC_ALPHA);
    } else {
      this.applyBlend(blend);
    }

    gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, count);

    //---------------------------------------------------------------------
    // Leave the state as the page is entitled to find it.
    //---------------------------------------------------------------------
    gl.disable(gl.BLEND);
    gl.blendEquation(gl.FUNC_ADD);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.bindVertexArray(null);
  }
}

//===========================================================================
// The page.
//===========================================================================

const pct = (v) => `${Math.round(v * 100)}%`;

mountDemo({
  name: 'Orrery',
  pluginId: 'OR01',
  tagline: 'Primitive shapes on deterministic paths — for animated masks, and for driving a pixel map.',
  repo: 'https://github.com/stoatworks-labs/orrery',
  page: 'https://stoatworks-labs.com/software/orrery/',
  video: 'https://www.youtube.com/watch?v=4VJWYmGRwk8',
  showBackdrop: true,
  presets: PRESETS,

  // Which bundle. Orrery ships two, because FFGL resolves a single entry point
  // per binary: a source that draws over its own background, and a mask effect
  // that draws the same shapes over the clip or cuts them into it. They differ
  // by a constructor flag and a #define handed to the shader compiler.
  variants: {
    label: 'Plugin',
    default: 'source',
    options: [
      { id: 'source', name: 'Orrery', hint: 'The source: shapes over their own background, on their own layer.' },
      { id: 'effect', name: 'Orrery Mask', hint: 'The effect: the same shapes over the clip, or cut into it.' },
    ],
  },

  sources: ['scene', 'grid', 'bars', 'ramp', 'spot', 'detail', 'alpha'],

  differences: [
    'The clip picker only does anything in Orrery Mask. The source has no input at all — SetMinInputs is 0 — and draws over its own Background colour.',
    'There is no host FFT in a browser page, so the spectrum the Audio group reads is all zeros — exactly what Resolume sends with no audio routed, and Audio Size and Audio Bright do nothing rather than the shapes twitching to a phantom signal.',
    'Beat and Bar lock to a 120 BPM transport generated in this page, which is the tempo the plugin falls back to when a host reports none. Resolume would supply its own, and the bar position with it. Manual ignores the clock entirely and is driven by the Phase slider, exactly as in the host.',
    'Preset is an option parameter in the plugin, with Custom as element 0 and a slider edit dropping back to it. Here the same six presets are in the panel header instead, from the plugin\'s own table.',
  ],

  params: [
    //---- Shape ------------------------------------------------------------
    {
      id: 'shape', name: 'Shape', type: 'option', default: 0, group: 'Shape',
      elements: SHAPE_NAMES,
      hint: 'Eight signed distance functions. An SDF is the right representation here because antialiasing is one smoothstep, outline is abs(d) − w, and softness is a wider smoothstep — one expression each, for all eight shapes.',
    },
    {
      id: 'count', name: 'Count', type: 'standard', default: 0.333, group: 'Shape',
      display: (v) => `${instancesFromParam(v)}`,
      hint: 'Quadratic, so the low end where the difference between three shapes and four is visible gets most of the travel. Capped at 64 because the whole set goes up as two vec4 uniform arrays.',
    },
    {
      id: 'size', name: 'Size', type: 'standard', default: 0.54, group: 'Shape',
      display: (v) => sizeFromParam(v).toFixed(3),
      hint: 'Radius as a fraction of the SHORT edge, which is what keeps a circle round on a 16:9 output.',
    },
    {
      id: 'sizeVary', name: 'Size Variation', type: 'standard', default: 0.0, group: 'Shape',
      display: pct,
      hint: 'Varied downwards from the slider, never up: a shape that grew past its slider would also grow past the quad it is drawn on and lose a corner.',
    },
    {
      id: 'stretch', name: 'Stretch', type: 'standard', default: 0.5, group: 'Shape',
      display: (v) => `${stretchFromParam(v).toFixed(2)}×`,
      hint: 'Exactly 1.0 at the centre of the slider, because 2 × 0.5 − 1 is exactly 0 in binary floating point — which is the only reason a "1 at the centre" control can be trusted to reach 1.',
    },
    {
      id: 'roundness', name: 'Roundness', type: 'standard', default: 0.0, group: 'Shape',
      display: pct,
      hint: 'Shrink the shape, then dilate the field back out by the same amount, so the outer extent stays where Bound was told it would be. On Ring this is thickness instead — a circle has no corners to round.',
    },
    { id: 'outline', name: 'Outline', type: 'standard', default: 0.0, group: 'Shape', display: pct },
    {
      id: 'softness', name: 'Softness', type: 'standard', default: 0.0, group: 'Shape',
      display: (v) => softnessFromParam(v).toFixed(3),
      hint: 'Zero gives a hard edge on purpose: a feathered one means a fixture on the boundary reads a half-brightness colour that was never in the design.',
    },
    {
      id: 'shade', name: 'Shade', type: 'standard', default: 0.0, group: 'Shape',
      display: pct,
      hint: 'Lights the shape as though it had been inflated. The normal comes out of the distance field, so on a circle it is exactly a sphere; every other primitive rounds the way its own field says it should.',
    },
    {
      id: 'light', name: 'Light', type: 'standard', default: 0.25, group: 'Shape',
      display: (v) => `${Math.round(clamp01(v) * 360)}°`,
      hint: 'Where the light comes from: one full turn over the range, 0.25 straight down from the top.',
    },

    //---- Motion -----------------------------------------------------------
    {
      id: 'path', name: 'Path', type: 'option', default: 0, group: 'Motion',
      elements: PATH_NAMES,
      hint: 'Every one is a closed-form function of (index, phase). A bounce is a triangle wave, not a collision test; a drift is a wrap, not an accumulation.',
    },
    {
      id: 'sync', name: 'Sync', type: 'option', default: 0, group: 'Motion',
      elements: SYNC_NAMES,
      hint: 'Because phase is just a number, locking to the track costs no separate code path. See the note at the foot — this page generates its own transport at 120 BPM.',
    },
    {
      id: 'speed', name: 'Speed', type: 'standard', default: 0.521, group: 'Motion',
      display: (v) => (speedFromParam(v) === 0 ? 'stopped' : `${speedFromParam(v).toFixed(2)} /s`),
      hint: 'A dead zone at the bottom rather than a curve that merely approaches zero: "stopped" has to be a place you can land on, and 0.0001 cycles per second is a shape that has moved somewhere unexpected by the second act.',
    },
    {
      id: 'phase', name: 'Phase', type: 'standard', default: 0.0, group: 'Motion',
      display: (v) => v.toFixed(3),
      hint: 'Added to the clock in every mode, and the only driver in Manual — for a keyframe, a MIDI fader, or Resolume\'s own BPM-synced animation.',
    },
    {
      id: 'spread', name: 'Spread', type: 'standard', default: 1.0, group: 'Motion',
      display: pct,
      hint: 'How far apart in phase consecutive instances sit. 0 moves them as one body; 1 spreads them evenly around a whole cycle, which is what turns a clump into a chase that arrives in order.',
    },
    {
      id: 'scatter', name: 'Scatter', type: 'standard', default: 0.0, group: 'Motion',
      display: pct,
      hint: 'Blends the even spread towards a hashed one: 0 is a metronome, 1 is a swarm. On Bounce it also detunes the two axes per instance.',
    },
    {
      id: 'seed', name: 'Seed', type: 'standard', default: 0.0, group: 'Motion',
      display: (v) => `${seedFromParam(v)}`,
      hint: 'An exact integer hash, never fract(sin(x)) — a composition built on the show laptop has to scatter its shapes to the same places on the rack machine.',
    },
    {
      id: 'pathSize', name: 'Path Size', type: 'standard', default: 0.467, group: 'Motion',
      display: (v) => pathSizeFromParam(v).toFixed(2),
      hint: 'Extent of the path in FRAME space, so an orbit at full size sweeps the whole of a 16:9 frame rather than a circle in the middle with dead bars at the sides — which is what you want when the frame is the LED rig.',
    },
    { id: 'centreX', name: 'Centre X', type: 'standard', default: 0.5, group: 'Motion', display: (v) => centreFromParam(v).toFixed(2) },
    { id: 'centreY', name: 'Centre Y', type: 'standard', default: 0.5, group: 'Motion', display: (v) => centreFromParam(v).toFixed(2) },
    {
      id: 'ratioX', name: 'Ratio X', type: 'standard', default: 0.646, group: 'Motion',
      display: (v) => ratioFromParam(v).toFixed(2),
      hint: 'Lissajous frequency, and the bounce rate on Bounce. 1:1 degenerates to the Orbit circle rather than a diagonal line, because the x axis carries a fixed quarter-turn.',
    },
    { id: 'ratioY', name: 'Ratio Y', type: 'standard', default: 0.5, group: 'Motion', display: (v) => ratioFromParam(v).toFixed(2) },
    {
      id: 'direction', name: 'Direction', type: 'standard', default: 0.0, group: 'Motion',
      display: (v) => `${((directionFromParam(v) * 180) / Math.PI).toFixed(0)}°`,
      hint: 'Drift heading. 0 travels right, and increasing turns clockwise on screen because y runs down.',
    },
    { id: 'gridCols', name: 'Grid Columns', type: 'standard', default: 0.2, group: 'Motion', display: (v) => `${gridFromParam(v)}` },
    { id: 'gridRows', name: 'Grid Rows', type: 'standard', default: 0.1333, group: 'Motion', display: (v) => `${gridFromParam(v)}` },
    {
      id: 'spin', name: 'Spin', type: 'standard', default: 0.5, group: 'Motion',
      display: (v) => `${spinFromParam(v).toFixed(2)} rev/cycle`,
      hint: 'Revolutions per CYCLE, not per second. Everything here hangs off phase; a spin with its own clock would be the one thing that slid out of time when the rest is beat-locked.',
    },
    { id: 'spinPhase', name: 'Spin Phase', type: 'standard', default: 0.0, group: 'Motion', display: pct },

    //---- Pulse ------------------------------------------------------------
    {
      id: 'pulse', name: 'Pulse Size', type: 'standard', default: 0.0, group: 'Pulse',
      display: pct,
      hint: 'A raised cosine occupying the first Width of each cycle. A raised cosine rather than a saw, because a saw\'s discontinuity reads as a fixture glitching rather than as a beat.',
    },
    { id: 'pulseBright', name: 'Pulse Bright', type: 'standard', default: 0.0, group: 'Pulse', display: pct },
    { id: 'pulseWidth', name: 'Pulse Width', type: 'standard', default: 0.5, group: 'Pulse', display: (v) => pct(pulseWidthFromParam(v)) },

    //---- Colour -----------------------------------------------------------
    {
      id: 'colourMode', name: 'Colour Mode', type: 'option', default: 0, group: 'Colour',
      elements: COLOUR_MODE_NAMES,
      hint: 'The hue modes take saturation and value off the swatch and replace only the hue — and treat an achromatic swatch as full saturation, or white would produce a row of identical shapes and look broken.',
    },
    { id: 'shapeR', name: 'Colour', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'shapeG', name: 'Colour_Green', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'shapeB', name: 'Colour_Blue', type: 'colour', default: 1.0, group: 'Colour' },
    { id: 'hueSpread', name: 'Hue Spread', type: 'standard', default: 1.0, group: 'Colour', display: pct },
    { id: 'opacity', name: 'Opacity', type: 'standard', default: 1.0, group: 'Colour', display: pct },
    { id: 'backR', name: 'Background', type: 'colour', default: 0.0, group: 'Colour' },
    { id: 'backG', name: 'Background_Green', type: 'colour', default: 0.0, group: 'Colour' },
    { id: 'backB', name: 'Background_Blue', type: 'colour', default: 0.0, group: 'Colour' },
    {
      id: 'backOpacity', name: 'Background Opacity', type: 'standard', default: 1.0, group: 'Colour',
      display: pct,
      hint: 'Opaque black by default, which is what a mask wants. The source only — the effect lays down the clip instead.',
    },
    {
      id: 'blend', name: 'Blend', type: 'option', default: 0, group: 'Colour',
      elements: BLEND_NAMES,
      hint: 'Three pieces of GL state rather than three branches in a loop. Max is what a mask wants: two overlapping white shapes stay white instead of clipping to a brighter white that is no longer the same colour as either.',
    },

    //---- Output -----------------------------------------------------------
    {
      id: 'maskMode', name: 'Mask Mode', type: 'option', default: 0, group: 'Output',
      elements: MASK_MODE_NAMES,
      hint: 'Orrery Mask only. Reveal samples the clip inside the shape fragment; Hide punches the shapes out with a blend function. Neither needs a mask buffer, which is why there is no FBO anywhere in this plugin.',
    },
    { id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Output', display: pct },

    //---- Audio ------------------------------------------------------------
    {
      id: 'audioSize', name: 'Audio Size', type: 'standard', default: 0.0, group: 'Audio',
      display: pct,
      hint: 'One broad band per instance rather than the first N bins: six shapes carve the whole spectrum into six rather than all sitting on the bass. There is no host FFT here — see the note at the foot.',
    },
    { id: 'audioBright', name: 'Audio Bright', type: 'standard', default: 0.0, group: 'Audio', display: pct },
  ],

  createRenderer: (gl) => new OrreryRenderer(gl),
});
