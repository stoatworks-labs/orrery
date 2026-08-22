/**
    ortest -- the offline harness.

    It drives **the real plugin class** through the real FFGL sequence in a
    headless core-profile context. Not a reimplementation of the motion and not
    a preview: the thing under test is `OrreryPlugin`, compiled from the same
    objects that go into the bundles, and every number below comes out of a
    frame it actually rendered.

        --out PATH        render a frame
        --shapes PATH     a contact sheet of all eight primitives
        --paths PATH      a contact sheet of all five paths
        --list            parameters, with their types and defaults
        --motion          where every shape landed, against Motion.cpp
        --round           circles stay round, and stay put, off 1:1
        --effect          render the effect variant over a test clip

    ## What --motion actually checks

    It is the test that matters, and it is deliberately not a mirror comparison.
    Downpour has to compare its shader's arithmetic against a C++ copy of the
    same arithmetic, because its maths runs per pixel and therefore exists
    twice. Orrery's motion runs once per instance on the CPU and is uploaded, so
    there is only one copy and nothing to compare it against.

    So instead of checking the maths against itself, `--motion` checks the
    **whole chain against the picture**: it asks Motion.cpp where instance 7
    should be, looks in that part of the rendered frame, and measures where the
    light actually is. Passing means the solver, the uniform upload, the vertex
    transform, the aspect correction, the distance function and the blend all
    agree -- and it fails if any one of them is wrong, including in ways a
    mirror test cannot see, such as the instance array being uploaded off by
    one.

    `--round` is separate because it catches one specific mistake that
    `--motion` is blind to: paths are placed in frame space and shapes are sized
    in short-edge fractions, and confusing the two is invisible on a square
    render and turns every circle into an ellipse on a real 16:9 output.

    Neither of them catches a dead uniform. See `tools/sweep.py`.
*/

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "Controls.h"
#include "Motion.h"
#include "Orrery.h"
#include "Shapes.h"

using namespace orrery;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );// filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );// bit depth
	ihdr.push_back( 6 );// truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

struct Target
{
	GLuint texture = 0;
	GLuint fbo     = 0;
	int width      = 0;
	int height     = 0;
};

Target makeTarget( int width, int height )
{
	Target target;
	target.width  = width;
	target.height = height;

	glGenTextures( 1, &target.texture );
	glBindTexture( GL_TEXTURE_2D, target.texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glBindTexture( GL_TEXTURE_2D, 0 );

	glGenFramebuffers( 1, &target.fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0 );
	return target;
}

void releaseTarget( Target& target )
{
	if( target.fbo != 0 )
		glDeleteFramebuffers( 1, &target.fbo );
	if( target.texture != 0 )
		glDeleteTextures( 1, &target.texture );
	target = Target();
}

/// Straight out of GL, bottom row first.
std::vector< unsigned char > readBytes( const Target& target )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( target.width ) * target.height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// A test clip for the effect: coloured quadrants over a gradient, so that a
/// mask mode getting its geometry or its UV flip wrong is obvious rather than
/// merely plausible.
GLuint makeTestClip( int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = static_cast< float >( x ) / static_cast< float >( width );
			const float v = static_cast< float >( y ) / static_cast< float >( height );

			unsigned char* p = &pixels[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ] = static_cast< unsigned char >( ( u < 0.5f ? 220.0f : 40.0f ) * ( 0.4f + 0.6f * v ) );
			p[ 1 ] = static_cast< unsigned char >( ( v < 0.5f ? 200.0f : 60.0f ) * ( 0.4f + 0.6f * u ) );
			p[ 2 ] = static_cast< unsigned char >( 255.0f * ( 0.3f + 0.7f * ( 1.0f - v ) ) );
			p[ 3 ] = 255;
		}
	}

	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// Parameters by name.
//---------------------------------------------------------------------------
std::map< std::string, unsigned int > parameterIndex( OrreryPlugin& plugin )
{
	std::map< std::string, unsigned int > byName;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* name = plugin.GetParamName( i );
		if( name != nullptr )
			byName[ name ] = i;
	}
	return byName;
}

bool applySetting( OrreryPlugin& plugin, const std::string& assignment )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		fprintf( stderr, "--set wants Name=value, got '%s'\n", assignment.c_str() );
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );

	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	const auto found                                   = byName.find( name );
	if( found == byName.end() )
	{
		fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	plugin.SetFloatParameter( found->second, std::stof( value ) );
	return true;
}

//---------------------------------------------------------------------------
// Rendering
//---------------------------------------------------------------------------
void render( OrreryPlugin& plugin, const Target& target, GLuint input = 0 )
{
	// A synthetic spectrum, written the way the host writes one: one value per
	// element of the Audio buffer. Without it the two Audio knobs measurably
	// do nothing offline, and the sweep would report them dead. A fixed shape
	// rather than anything time-driven, so renders stay reproducible:
	// bass-heavy like programme material, with a ripple so neighbouring
	// per-instance bands differ.
	for( int bin = 0; bin < kMaxInstances; ++bin )
	{
		const float across = static_cast< float >( bin ) / static_cast< float >( kMaxInstances - 1 );
		const float level  = 0.7f * ( 1.0f - across ) * ( 1.0f - across ) + 0.2f * ( 0.5f + 0.5f * std::sin( 25.0f * across ) );
		plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned int >( bin ), level );
	}

	glBindFramebuffer( GL_FRAMEBUFFER, target.fbo );
	glViewport( 0, 0, target.width, target.height );
	glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	glClear( GL_COLOR_BUFFER_BIT );

	plugin.Render( target.width, target.height, input, 1.0f, 1.0f );
	glFinish();
}

/// Set up a plugin, initialise its GL, and hand it back ready to draw.
bool prepare( OrreryPlugin& plugin, int width, int height )
{
	FFGLViewportStruct viewport {};
	viewport.x      = 0;
	viewport.y      = 0;
	viewport.width  = static_cast< unsigned int >( width );
	viewport.height = static_cast< unsigned int >( height );

	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		fprintf( stderr, "InitGL failed -- see the log\n" );
		return false;
	}
	return true;
}

//---------------------------------------------------------------------------
// Measuring what was drawn
//---------------------------------------------------------------------------
struct Blob
{
	double weight = 0.0;   ///< Total luminance found.
	double x      = 0.0;   ///< Centroid, in frame space (0..1, y down).
	double y      = 0.0;
	double spanX  = 0.0;   ///< Extent in pixels, from the second moment.
	double spanY  = 0.0;
};

double luminanceAt( const std::vector< unsigned char >& image, int width, int height, int col, int glRow )
{
	if( col < 0 || col >= width || glRow < 0 || glRow >= height )
		return 0.0;

	const unsigned char* p = &image[ ( static_cast< size_t >( glRow ) * width + col ) * 4 ];
	return ( 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ] ) / 255.0;
}

/**
    The luminance-weighted centroid of whatever is inside a window centred on
    where a shape was predicted to be.

    A window rather than a global blob search on purpose. A global search would
    have to decide which blob belongs to which instance, and the obvious way to
    do that -- nearest to the prediction -- is exactly the assumption the test
    is supposed to be checking. Looking only where the shape was promised means
    a shape that is somewhere else registers as an empty window and fails, which
    is the answer we want.
*/
Blob measure( const std::vector< unsigned char >& image, int width, int height,
              double predictedX, double predictedY, double radiusPx )
{
	const double centreCol  = predictedX * width;
	const double centreGlRow = ( 1.0 - predictedY ) * height;

	const int reach = static_cast< int >( std::ceil( radiusPx * 2.0 ) ) + 2;

	Blob blob;
	double sumX = 0.0;
	double sumY = 0.0;

	const int col0 = static_cast< int >( std::floor( centreCol ) ) - reach;
	const int col1 = static_cast< int >( std::ceil( centreCol ) ) + reach;
	const int row0 = static_cast< int >( std::floor( centreGlRow ) ) - reach;
	const int row1 = static_cast< int >( std::ceil( centreGlRow ) ) + reach;

	for( int row = row0; row <= row1; ++row )
	{
		for( int col = col0; col <= col1; ++col )
		{
			const double l = luminanceAt( image, width, height, col, row );
			if( l <= 0.0 )
				continue;

			blob.weight += l;
			sumX += l * ( col + 0.5 );
			sumY += l * ( row + 0.5 );
		}
	}

	if( blob.weight <= 0.0 )
		return blob;

	const double meanCol   = sumX / blob.weight;
	const double meanGlRow = sumY / blob.weight;

	// Second moment, for --round. Reported as a full width rather than a
	// standard deviation so the two axes of a circle can be compared directly.
	double varX = 0.0;
	double varY = 0.0;
	for( int row = row0; row <= row1; ++row )
	{
		for( int col = col0; col <= col1; ++col )
		{
			const double l = luminanceAt( image, width, height, col, row );
			if( l <= 0.0 )
				continue;

			varX += l * ( col + 0.5 - meanCol ) * ( col + 0.5 - meanCol );
			varY += l * ( row + 0.5 - meanGlRow ) * ( row + 0.5 - meanGlRow );
		}
	}

	blob.x     = meanCol / width;
	blob.y     = 1.0 - meanGlRow / height;
	blob.spanX = std::sqrt( varX / blob.weight );
	blob.spanY = std::sqrt( varY / blob.weight );

	return blob;
}

/// The shape's radius in pixels, from the same convention Motion.h documents:
/// a fraction of the SHORT edge.
double radiusPixels( double scale, int width, int height )
{
	return scale * std::min( width, height );
}

//---------------------------------------------------------------------------
// --list
//---------------------------------------------------------------------------
int listParameters( OrreryPlugin& plugin )
{
	printf( "%-4s %-20s %-10s %s\n", "id", "name", "type", "default" );
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* name        = plugin.GetParamName( i );
		const unsigned int type = plugin.GetParamType( i );

		const char* typeName = "standard";
		if( type == FF_TYPE_BOOLEAN )
			typeName = "boolean";
		else if( type == FF_TYPE_OPTION )
			typeName = "option";
		else if( type == FF_TYPE_RED || type == FF_TYPE_GREEN || type == FF_TYPE_BLUE )
			typeName = "colour";
		else if( type == FF_TYPE_TEXT )
			typeName = "text";
		else if( type == FF_TYPE_BUFFER )
			typeName = "buffer";

		printf( "%-4u %-20s %-10s %.4f\n", i, name != nullptr ? name : "?", typeName,
		        plugin.GetFloatParameter( i ) );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --motion
//---------------------------------------------------------------------------
struct MotionCase
{
	const char* name;
	Path path;
	int instances;    ///< The host value is derived from this.
	int width;
	int height;
	float phase;
	float scatter;
};

/// The 0..1 host value that produces a given instance count.
float instancesParam( int wanted )
{
	// InstancesFromParam is 1 + round( 63 v^2 ); invert it and nudge onto the
	// centre of the rounding bucket.
	const float v = std::sqrt( static_cast< float >( wanted - 1 ) / 63.0f );
	return std::min( 1.0f, std::max( 0.0f, v ) );
}

int motionCheck()
{
	const MotionCase cases[] = {
		{ "orbit 16:9",      Path::Orbit,     6,  1280, 720, 0.000f, 0.0f },
		{ "orbit 16:9 late", Path::Orbit,     6,  1280, 720, 3.250f, 0.0f },
		{ "lissajous 16:9",  Path::Lissajous, 5,  1280, 720, 1.700f, 0.0f },
		{ "drift 4:3",       Path::Drift,     4,   800, 600, 0.400f, 0.0f },
		{ "bounce 1:1",      Path::Bounce,    3,   720, 720, 2.100f, 0.0f },
		{ "bounce scatter",  Path::Bounce,    5,  1280, 720, 5.900f, 0.8f },
		{ "grid 16:9",       Path::Grid,      12, 1280, 720, 0.250f, 0.0f },
		{ "orbit portrait",  Path::Orbit,     6,   720, 1280, 0.800f, 0.0f },
	};

	// A shape has to be found within this of where Motion.cpp said it would be.
	// One and a half pixels: an antialiased centroid lands well inside a pixel,
	// so this is loose enough not to be flaky and tight enough that a genuine
	// half-cell error cannot hide in it.
	const double kTolerancePx = 1.5;

	int failures = 0;
	int checked  = 0;

	for( const MotionCase& c : cases )
	{
		OrreryPlugin plugin( false );
		if( !prepare( plugin, c.width, c.height ) )
			return 1;

		plugin.SetFloatParameter( PT_PATH, static_cast< float >( c.path ) );
		plugin.SetFloatParameter( PT_INSTANCES, instancesParam( c.instances ) );
		plugin.SetFloatParameter( PT_SCATTER, c.scatter );
		plugin.SetFloatParameter( PT_SIZE, 0.40f );   // ~0.032 of the short edge
		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Circle ) );
		plugin.SetPhaseOverride( c.phase );

		Target target = makeTarget( c.width, c.height );
		render( plugin, target );
		const std::vector< unsigned char > image = readBytes( target );

		const MotionParams motion            = plugin.CurrentMotion( c.width, c.height );
		const std::vector< Instance >& drawn = plugin.LastInstances();

		int caseFailures = 0;
		for( size_t i = 0; i < drawn.size(); ++i )
		{
			// Predict independently of what Render happened to place, so that a
			// solver called with the wrong arguments is still caught.
			const Instance expected = SolveOne( motion, static_cast< int >( i ) );
			const double radius     = radiusPixels( expected.scale, c.width, c.height );

			const Blob blob = measure( image, c.width, c.height, expected.x, expected.y, radius );

			// A filled circle of this radius, at full brightness. Half of it is
			// generous: what is being tested is "the shape is here", and the
			// window's edges clip a little of it.
			const double expectedWeight = 3.14159 * radius * radius * 0.5;

			if( blob.weight < expectedWeight )
			{
				printf( "  FAIL %-16s instance %2zu: expected light at (%.4f, %.4f), found %.0f of %.0f\n",
				        c.name, i, expected.x, expected.y, blob.weight, expectedWeight );
				++caseFailures;
				continue;
			}

			const double dx = ( blob.x - expected.x ) * c.width;
			const double dy = ( blob.y - expected.y ) * c.height;
			const double d  = std::sqrt( dx * dx + dy * dy );

			if( d > kTolerancePx )
			{
				printf( "  FAIL %-16s instance %2zu: predicted (%.4f, %.4f), measured (%.4f, %.4f), %.2f px out\n",
				        c.name, i, expected.x, expected.y, blob.x, blob.y, d );
				++caseFailures;
			}

			++checked;
		}

		if( caseFailures == 0 )
			printf( "  ok   %-16s %2zu instances at %dx%d, phase %.3f\n",
			        c.name, drawn.size(), c.width, c.height, c.phase );

		failures += caseFailures;
		releaseTarget( target );
		plugin.DeInitGL();
	}

	printf( "\n--motion: %d instances placed, %d wrong\n", checked, failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --round
//---------------------------------------------------------------------------
/**
    A circle has to come out round, and a path has to reach the same fraction of
    the frame, on outputs that are not square.

    This is its own test because it catches one specific confusion that
    `--motion` cannot see: shape size is a fraction of the SHORT edge, path
    extent is a fraction of EACH axis, and those are the same number only at
    1:1. Get it wrong and a square test render looks perfect while every real
    output draws ellipses.
*/
int roundCheck()
{
	struct Case
	{
		const char* name;
		int width;
		int height;
	};

	const Case cases[] = {
		{ "1:1",      720, 720 },
		{ "16:9",     1280, 720 },
		{ "portrait", 720, 1280 },
		{ "2.39:1",   1720, 720 },
	};

	int failures = 0;

	for( const Case& c : cases )
	{
		OrreryPlugin plugin( false );
		if( !prepare( plugin, c.width, c.height ) )
			return 1;

		// One shape, parked in the middle, so nothing can overlap it.
		plugin.SetFloatParameter( PT_INSTANCES, 0.0f );
		plugin.SetFloatParameter( PT_PATH_SIZE, 0.0f );
		plugin.SetFloatParameter( PT_SIZE, 0.55f );
		plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( Shape::Circle ) );
		plugin.SetPhaseOverride( 0.0f );

		Target target = makeTarget( c.width, c.height );
		render( plugin, target );
		const std::vector< unsigned char > image = readBytes( target );

		const MotionParams motion = plugin.CurrentMotion( c.width, c.height );
		const Instance expected   = SolveOne( motion, 0 );
		const double radius       = radiusPixels( expected.scale, c.width, c.height );

		const Blob blob = measure( image, c.width, c.height, expected.x, expected.y, radius );

		if( blob.weight <= 0.0 )
		{
			printf( "  FAIL %-9s nothing drawn\n", c.name );
			++failures;
			releaseTarget( target );
			plugin.DeInitGL();
			continue;
		}

		// The two axes of the second moment must match: that is roundness, in
		// pixels, measured off the picture.
		const double ratio = blob.spanX / std::max( 1e-6, blob.spanY );

		// A disc of radius r has a second moment of r/2 on each axis. Checking
		// it against the radius the solver asked for is what would catch the
		// shape being sized off the wrong edge -- which produces a perfectly
		// round circle of entirely the wrong size, and so survives the ratio
		// test on its own.
		const double expectedSpan = radius * 0.5;
		const double sizeError    = std::fabs( blob.spanX - expectedSpan ) / expectedSpan;

		const bool roundEnough = std::fabs( ratio - 1.0 ) < 0.02;
		const bool sizedRight  = sizeError < 0.06;

		if( !roundEnough || !sizedRight )
		{
			printf( "  FAIL %-9s %dx%d: axes %.3f x %.3f px (ratio %.4f), expected span %.3f\n",
			        c.name, c.width, c.height, blob.spanX, blob.spanY, ratio, expectedSpan );
			++failures;
		}
		else
		{
			printf( "  ok   %-9s %dx%d: round to %.2f%%, sized to %.2f%%\n",
			        c.name, c.width, c.height,
			        std::fabs( ratio - 1.0 ) * 100.0, sizeError * 100.0 );
		}

		releaseTarget( target );
		plugin.DeInitGL();
	}

	printf( "\n--round: %d aspect ratios, %d wrong\n", static_cast< int >( sizeof( cases ) / sizeof( cases[ 0 ] ) ), failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --mask
//---------------------------------------------------------------------------
struct Pixel
{
	double r = 0.0;
	double g = 0.0;
	double b = 0.0;
	double a = 0.0;
};

Pixel pixelAt( const std::vector< unsigned char >& image, int width, int height, int col, int glRow )
{
	const unsigned char* p = &image[ ( static_cast< size_t >( glRow ) * width + col ) * 4 ];
	return { p[ 0 ] / 255.0, p[ 1 ] / 255.0, p[ 2 ] / 255.0, p[ 3 ] / 255.0 };
}

bool near( double a, double b, double tolerance = 0.02 )
{
	return std::fabs( a - b ) <= tolerance;
}

/**
    Each mask mode does what it says, checked on the picture.

    The reference clip is captured by rendering the effect with the shapes at
    zero opacity rather than by predicting what the test clip ought to look
    like. That is deliberate: predicting it would mean reimplementing the UV flip
    here, and a test that reimplements the thing it is testing agrees with its
    own mistakes.

    Two samples per mode -- the middle of the single shape, and a corner well
    outside it -- are enough, because what separates these four modes is
    precisely what happens inside a shape versus outside one.
*/
int maskCheck()
{
	const int width  = 640;
	const int height = 360;

	// One shape, parked in the middle, big enough to cover the centre sample.
	auto configure = []( OrreryPlugin& plugin, MaskMode mode, float opacity ) {
		plugin.SetFloatParameter( PT_INSTANCES, 0.0f );
		plugin.SetFloatParameter( PT_PATH_SIZE, 0.0f );
		plugin.SetFloatParameter( PT_SIZE, 0.80f );
		plugin.SetFloatParameter( PT_MASK_MODE, static_cast< float >( mode ) );
		plugin.SetFloatParameter( PT_OPACITY, opacity );
		plugin.SetPhaseOverride( 0.0f );
	};

	GLuint clip = makeTestClip( width, height );

	// The reference: the clip, straight through.
	Pixel refCentre;
	Pixel refCorner;
	{
		OrreryPlugin plugin( true );
		if( !prepare( plugin, width, height ) )
			return 1;
		configure( plugin, MaskMode::Over, 0.0f );

		Target target = makeTarget( width, height );
		render( plugin, target, clip );
		const std::vector< unsigned char > image = readBytes( target );

		refCentre = pixelAt( image, width, height, width / 2, height / 2 );
		refCorner = pixelAt( image, width, height, 12, 12 );

		releaseTarget( target );
		plugin.DeInitGL();
	}

	int failures = 0;

	struct Expectation
	{
		const char* name;
		MaskMode mode;
	};

	const Expectation modes[] = {
		{ "Over",      MaskMode::Over },
		{ "Reveal",    MaskMode::Reveal },
		{ "Hide",      MaskMode::Hide },
		{ "Colourise", MaskMode::Colourise },
	};

	for( const Expectation& e : modes )
	{
		OrreryPlugin plugin( true );
		if( !prepare( plugin, width, height ) )
			return 1;

		configure( plugin, e.mode, 1.0f );

		// Colourise multiplies the clip by the shape colour, so it needs a
		// colour that is not white -- against white it is arithmetically
		// identical to Reveal, and a test that used the default swatch would
		// pass whether or not the multiply happened at all.
		if( e.mode == MaskMode::Colourise )
		{
			plugin.SetFloatParameter( PT_COLOUR_MODE, static_cast< float >( ColourMode::Solid ) );
			plugin.SetFloatParameter( PT_SHAPE_R, 1.0f );
			plugin.SetFloatParameter( PT_SHAPE_G, 0.0f );
			plugin.SetFloatParameter( PT_SHAPE_B, 0.0f );
		}

		Target target = makeTarget( width, height );
		render( plugin, target, clip );
		const std::vector< unsigned char > image = readBytes( target );

		const Pixel centre = pixelAt( image, width, height, width / 2, height / 2 );
		const Pixel corner = pixelAt( image, width, height, 12, 12 );

		bool ok = false;
		std::string wanted;

		switch( e.mode )
		{
		case MaskMode::Over:
			wanted = "opaque white inside, clip outside";
			ok     = near( centre.r, 1.0 ) && near( centre.g, 1.0 ) && near( centre.b, 1.0 ) && near( centre.a, 1.0 )
			     && near( corner.r, refCorner.r ) && near( corner.a, refCorner.a );
			break;

		case MaskMode::Reveal:
			wanted = "clip inside, transparent outside";
			ok     = near( centre.r, refCentre.r ) && near( centre.g, refCentre.g )
			     && near( centre.b, refCentre.b ) && near( centre.a, refCentre.a )
			     && near( corner.a, 0.0 );
			break;

		case MaskMode::Hide:
			wanted = "transparent inside, clip outside";
			ok     = near( centre.a, 0.0 )
			     && near( corner.r, refCorner.r ) && near( corner.g, refCorner.g ) && near( corner.a, refCorner.a );
			break;

		case MaskMode::Colourise:
			wanted = "clip times red inside, transparent outside";
			ok     = near( centre.r, refCentre.r ) && near( centre.g, 0.0 ) && near( centre.b, 0.0 )
			     && near( corner.a, 0.0 );
			break;

		default:
			break;
		}

		if( ok )
		{
			printf( "  ok   %-10s %s\n", e.name, wanted.c_str() );
		}
		else
		{
			printf( "  FAIL %-10s wanted %s\n", e.name, wanted.c_str() );
			printf( "         inside  rgba %.3f %.3f %.3f %.3f  (clip is %.3f %.3f %.3f %.3f)\n",
			        centre.r, centre.g, centre.b, centre.a, refCentre.r, refCentre.g, refCentre.b, refCentre.a );
			printf( "         outside rgba %.3f %.3f %.3f %.3f  (clip is %.3f %.3f %.3f %.3f)\n",
			        corner.r, corner.g, corner.b, corner.a, refCorner.r, refCorner.g, refCorner.b, refCorner.a );
			++failures;
		}

		releaseTarget( target );
		plugin.DeInitGL();
	}

	glDeleteTextures( 1, &clip );

	printf( "\n--mask: 4 modes, %d wrong\n", failures );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Contact sheets
//---------------------------------------------------------------------------
int contactSheet( const std::string& path, bool byShape )
{
	const int cell    = 320;
	const int count   = byShape ? static_cast< int >( Shape::Count ) : static_cast< int >( Path::Count );
	const int columns = 4;
	const int rows    = ( count + columns - 1 ) / columns;

	const int sheetW = cell * columns;
	const int sheetH = cell * rows;

	// Opaque black, not zero: a zeroed buffer is transparent, and the cells left
	// over on the last row would show as white in any viewer that composites the
	// alpha -- which reads as a rendering fault rather than as padding.
	std::vector< unsigned char > sheet( static_cast< size_t >( sheetW ) * sheetH * 4, 0 );
	for( size_t i = 3; i < sheet.size(); i += 4 )
		sheet[ i ] = 255;

	for( int i = 0; i < count; ++i )
	{
		OrreryPlugin plugin( false );
		if( !prepare( plugin, cell, cell ) )
			return 1;

		if( byShape )
		{
			plugin.SetFloatParameter( PT_SHAPE, static_cast< float >( i ) );
			plugin.SetFloatParameter( PT_INSTANCES, instancesParam( 5 ) );
			plugin.SetFloatParameter( PT_SIZE, 0.62f );
			plugin.SetFloatParameter( PT_ROUNDNESS, 0.25f );
		}
		else
		{
			plugin.SetFloatParameter( PT_PATH, static_cast< float >( i ) );
			plugin.SetFloatParameter( PT_INSTANCES, instancesParam( 12 ) );
			plugin.SetFloatParameter( PT_SIZE, 0.45f );
			plugin.SetFloatParameter( PT_COLOUR_MODE, static_cast< float >( ColourMode::HueSpread ) );
		}

		plugin.SetPhaseOverride( 0.37f );

		Target target = makeTarget( cell, cell );
		render( plugin, target );
		const std::vector< unsigned char > tile = flipRows( readBytes( target ), cell, cell );

		const int col = i % columns;
		const int row = i / columns;
		for( int y = 0; y < cell; ++y )
		{
			std::memcpy( &sheet[ ( ( static_cast< size_t >( row * cell + y ) ) * sheetW + col * cell ) * 4 ],
			             &tile[ static_cast< size_t >( y ) * cell * 4 ],
			             static_cast< size_t >( cell ) * 4 );
		}

		printf( "  %s\n", byShape ? ShapeName( static_cast< Shape >( i ) ) : PathName( static_cast< Path >( i ) ) );

		releaseTarget( target );
		plugin.DeInitGL();
	}

	if( !writePng( path, sheetW, sheetH, sheet ) )
	{
		fprintf( stderr, "could not write %s\n", path.c_str() );
		return 1;
	}

	printf( "wrote %s (%dx%d)\n", path.c_str(), sheetW, sheetH );
	return 0;
}

//---------------------------------------------------------------------------
// --sequence
//---------------------------------------------------------------------------
//
// A cue sheet, so the project video is the real plugin being *operated* rather
// than a mock-up or a screen recording:
//
//     12.0        Shape=1              set at a time
//     4.0..9.0    Count=0.3..0.7       ramp between two times
//
// Times are seconds on the video's own clock, which is also the host clock
// handed to the plugin -- so a cue at 12s is the frame you see at 12s.
//
// The phase is deliberately NOT pinned here. The plugin runs off the host clock
// exactly as it does in Resolume, which is the only way the video can honestly
// show Speed and Sync doing anything.
//
struct Cue
{
	double from = 0.0;
	double to   = 0.0;
	std::string name;
	float first  = 0.0f;
	float second = 0.0f;
	bool ramp    = false;
};

bool parseCues( const std::string& path, std::vector< Cue >& cues )
{
	FILE* file = fopen( path.c_str(), "rb" );
	if( file == nullptr )
	{
		fprintf( stderr, "cannot open cue sheet %s\n", path.c_str() );
		return false;
	}

	char line[ 1024 ];
	int number = 0;
	while( fgets( line, sizeof( line ), file ) != nullptr )
	{
		++number;
		std::string text = line;

		const size_t hash = text.find( '#' );
		if( hash != std::string::npos )
			text = text.substr( 0, hash );

		const size_t firstReal = text.find_first_not_of( " \t\r\n" );
		if( firstReal == std::string::npos )
			continue;
		text = text.substr( firstReal );

		const size_t split = text.find_first_of( " \t" );
		if( split == std::string::npos )
			continue;

		const std::string when = text.substr( 0, split );
		std::string assignment = text.substr( split );

		const size_t assignStart = assignment.find_first_not_of( " \t" );
		if( assignStart == std::string::npos )
			continue;
		assignment = assignment.substr( assignStart );
		while( !assignment.empty() && ( assignment.back() == '\n' || assignment.back() == '\r'
		                                || assignment.back() == ' ' || assignment.back() == '\t' ) )
			assignment.pop_back();

		Cue cue;
		const size_t timeRange = when.find( ".." );
		if( timeRange != std::string::npos )
		{
			cue.from = std::strtod( when.substr( 0, timeRange ).c_str(), nullptr );
			cue.to   = std::strtod( when.substr( timeRange + 2 ).c_str(), nullptr );
			cue.ramp = true;
		}
		else
		{
			cue.from = cue.to = std::strtod( when.c_str(), nullptr );
		}

		const size_t equals = assignment.find( '=' );
		if( equals == std::string::npos )
		{
			fprintf( stderr, "%s:%d: expected Name=value\n", path.c_str(), number );
			fclose( file );
			return false;
		}

		cue.name                = assignment.substr( 0, equals );
		const std::string value = assignment.substr( equals + 1 );

		const size_t valueRange = value.find( ".." );
		if( cue.ramp && valueRange != std::string::npos )
		{
			cue.first  = std::strtof( value.substr( 0, valueRange ).c_str(), nullptr );
			cue.second = std::strtof( value.substr( valueRange + 2 ).c_str(), nullptr );
		}
		else
		{
			cue.first = cue.second = std::strtof( value.c_str(), nullptr );
			cue.ramp  = false;
		}

		cues.push_back( cue );
	}

	fclose( file );
	return true;
}

int renderSequence( const std::string& directory, const std::string& cuePath,
                    int width, int height, double seconds, double fps, bool effect )
{
	std::vector< Cue > cues;
	if( !cuePath.empty() && !parseCues( cuePath, cues ) )
		return 1;

	OrreryPlugin plugin( effect );
	if( !prepare( plugin, width, height ) )
		return 1;

	// Every cue is checked against the real parameter list before a single frame
	// is rendered. A typo in a name would otherwise be a cue that silently never
	// fires, and the only symptom would be a video that is subtly less
	// interesting than the sheet says it is.
	const std::map< std::string, unsigned int > byName = parameterIndex( plugin );
	for( const Cue& cue : cues )
	{
		if( byName.find( cue.name ) == byName.end() )
		{
			fprintf( stderr, "cue names '%s', which is not a parameter\n", cue.name.c_str() );
			return 1;
		}
	}

	Target target = makeTarget( width, height );
	const GLuint clip = effect ? makeTestClip( width, height ) : 0;

	const int frames = static_cast< int >( seconds * fps + 0.5 );
	int written      = 0;

	for( int frame = 0; frame < frames; ++frame )
	{
		const double now = static_cast< double >( frame ) / fps;

		// Apply every cue whose window has started. Cues are applied in file
		// order each frame rather than tracked as state, so a later cue on the
		// same parameter simply wins -- which is what reading the sheet top to
		// bottom would lead you to expect.
		for( const Cue& cue : cues )
		{
			if( now < cue.from )
				continue;

			float value = cue.second;
			if( cue.ramp && now < cue.to && cue.to > cue.from )
			{
				const double t = ( now - cue.from ) / ( cue.to - cue.from );
				// Smoothstep rather than linear. A parameter that starts and
				// stops abruptly reads as a jump cut even when the value in
				// between is right.
				const double eased = t * t * ( 3.0 - 2.0 * t );
				value = static_cast< float >( cue.first + ( cue.second - cue.first ) * eased );
			}

			plugin.SetFloatParameter( byName.at( cue.name ), value );
		}

		// The host clock and a steady 120bpm transport, so Sync has something
		// real to lock to. Declare the unit: the harness renders as fast as
		// the GPU allows, so the plugin's calibration -- which measures host
		// time against real elapsed time -- has nothing to measure here.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( now );
		plugin.SetBeatInfo( 120.0f, static_cast< float >( std::fmod( now / 2.0, 1.0 ) ) );

		render( plugin, target, clip );

		char path[ 1024 ];
		snprintf( path, sizeof( path ), "%s/frame%05d.png", directory.c_str(), frame );

		const std::vector< unsigned char > image = flipRows( readBytes( target ), width, height );
		if( !writePng( path, width, height, image ) )
		{
			fprintf( stderr, "could not write %s\n", path );
			releaseTarget( target );
			return 1;
		}

		++written;
		if( written % 60 == 0 )
			printf( "  %d / %d frames\n", written, frames );
	}

	releaseTarget( target );
	if( clip != 0 )
		glDeleteTextures( 1, &clip );
	plugin.DeInitGL();

	printf( "wrote %d frames to %s at %g fps (%.1f seconds)\n", written, directory.c_str(), fps,
	        written / fps );
	return 0;
}

void usage()
{
	printf( "ortest -- the Orrery offline harness\n\n"
	        "  --out PATH        render a frame\n"
	        "  --shapes PATH     a contact sheet of all eight primitives\n"
	        "  --paths PATH      a contact sheet of all five paths\n"
	        "  --list            parameters, with their types and defaults\n"
	        "  --clock           the host clock lands in seconds, whatever unit it speaks\n"
	        "  --speed           a Speed change does not move the picture\n"
	        "  --motion          where every shape landed, against Motion.cpp\n"
	        "  --round           circles stay round, and stay put, off 1:1\n"
	        "  --mask            each effect mask mode does what it says\n"
	        "  --effect          render over a test clip (with --out)\n"
	        "  --phase P         pin the phase (default 0)\n"
	        "  --time T          drive the host clock instead of pinning\n"
	        "  --size WxH        output size (default 1280x720)\n"
	        "  --set Name=value  set any parameter by name, repeatable\n"
	        "  --sequence DIR    render a cue-sheet driven frame sequence\n"
	        "  --script FILE     the cue sheet (with --sequence)\n"
	        "  --seconds S       sequence length (default 45)\n"
	        "  --fps F           sequence frame rate (default 30)\n" );
}

} // namespace

//---------------------------------------------------------------------------
/// Prove the host clock lands in seconds whatever unit the host speaks.
///
/// This is the gap that let a thousand-times-fast bug ship: every harness in
/// the fleet drove SetTime in SECONDS, Resolume drives it in MILLISECONDS, so
/// the path users actually run was the one path nothing exercised. The deltas
/// below are fed in real time -- the calibration measures host time against a
/// steady_clock, so a test that raced through them would measure nothing.
//---------------------------------------------------------------------------
int runClockTest()
{
	struct Case
	{
		const char* name;
		double perFrame;///< what the host adds per frame
		double expected;///< the scale it should settle on
	};
	const Case cases[] = {
		{ "milliseconds (Resolume)", 20.0, 0.001 },
		{ "seconds (harness)", 0.02, 1.0 },
	};

	int failures = 0;

	for( const Case& c : cases )
	{
		OrreryPlugin plugin( false );
		double host = 0.0;

		// Twelve frames a real ~20 ms apart: comfortably more than the four
		// agreeing frames the calibration asks for, and slow enough that the
		// wall clock has something to measure.
		for( int frame = 0; frame < 12; ++frame )
		{
			std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
			host += c.perFrame;
			plugin.SetTime( host );
			plugin.TickClockForTest();
		}

		const double scale = plugin.ClockScaleForTest();
		const double secs  = plugin.HostSecondsForTest();

		// Twelve frames of 20 ms is about 0.24 s of programme time whichever
		// unit the host counts in. Loose bounds: the point is that it is not
		// out by a factor of a thousand.
		const bool scaleOk = std::abs( scale - c.expected ) < 1e-9;
		const bool timeOk  = secs > 0.05 && secs < 1.0;

		std::printf( "clock %-26s scale=%-6g seconds=%-8.4f %s\n",
		             c.name, scale, secs, ( scaleOk && timeOk ) ? "ok" : "FAILED" );

		if( !scaleOk )
		{
			std::fprintf( stderr, "  expected scale %g, got %g\n", c.expected, scale );
			++failures;
		}
		if( !timeOk )
		{
			std::fprintf( stderr, "  %.4f s is not a plausible 0.24 s of clock\n", secs );
			++failures;
		}
	}

	// And the arithmetic itself: a declared millisecond host and a declared
	// seconds host must put the clock in the same place for the same instant.
	{
		OrreryPlugin ms( false );
		ms.SetClockScaleForTest( 0.001 );
		ms.SetTime( 2500.0 );
		ms.TickClockForTest();

		OrreryPlugin sec( false );
		sec.SetClockScaleForTest( 1.0 );
		sec.SetTime( 2.5 );
		sec.TickClockForTest();

		const double a  = ms.HostSecondsForTest();
		const double b  = sec.HostSecondsForTest();
		const bool same = std::abs( a - b ) < 1e-9 && std::abs( a - 2.5 ) < 1e-9;
		std::printf( "clock %-26s ms=%.4f seconds=%.4f %s\n",
		             "2500ms == 2.5s", a, b, same ? "ok" : "FAILED" );
		if( !same )
			++failures;
	}

	std::printf( "%s\n", failures == 0 ? "clock: all ok" : "clock: FAILURES" );
	return failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
/// Prove a Speed change does not move the picture.
///
/// `phase = clock * speed` puts the shapes somewhere entirely new every time
/// Speed is nudged, because `clock` is however long the composition has been
/// open -- an hour in, a small nudge is hundreds of cycles. Issue #6's reporter
/// hit it live: "the objects jump, almost as though they are restarting from a
/// specific position", which is exactly what it looks like.
///
/// Phase is what places every instance, so reading it either side of the change
/// says what happened directly. A rendered-frame comparison would only say the
/// two frames matched, and an evenly spread chase has enough symmetries to
/// match for the wrong reason (see AGENTS.md on phase periodicity).
//---------------------------------------------------------------------------
int runSpeedTest()
{
	int failures = 0;

	auto check = [ &failures ]( const char* what, double got, double want, double tol ) {
		const bool ok = std::abs( got - want ) <= tol;
		std::printf( "speed %-34s got=%-12.6f want=%-12.6f %s\n", what, got, want, ok ? "ok" : "FAILED" );
		if( !ok )
			++failures;
	};

	// Slider positions, not cycles per second. 0.02 is the bottom of the live
	// range and anything below it is the deliberate stopped zone.
	struct Step
	{
		const char* name;
		float slider;
	};
	const Step steps[] = {
		{ "default -> 0.10 (slower)", 0.10f },
		{ "0.10 -> 0.95 (much faster)", 0.95f },
		{ "0.95 -> 0.00 (stopped)", 0.00f },
		{ "0.00 -> 0.80 (running again)", 0.80f },
	};

	OrreryPlugin plugin( false );
	plugin.SetClockScaleForTest( 1.0 );

	// An hour in, which is where the old arithmetic hurt most and where a live
	// operator actually is when they reach for the slider.
	double host = 3600.0;
	plugin.SetTime( host );
	plugin.TickClockForTest();

	// Untouched, the anchor must leave the old expression exactly as it was --
	// this is what keeps tools/sweep.py and every rendered-frame test honest.
	// The plugin's own default is asked for rather than written down here: a
	// test that hard-codes it goes quietly wrong the day the default moves.
	check( "untouched == clock * speed",
	       plugin.CurrentPhaseForTest(),
	       host * SpeedFromParam( plugin.GetFloatParameter( PT_SPEED ) ), 1e-3 );

	for( const Step& step : steps )
	{
		const float before = plugin.CurrentPhaseForTest();

		// The same instant, a new speed: nothing about the clock has moved, so
		// nothing about the picture may either.
		plugin.SetFloatParameter( PT_SPEED, step.slider );
		plugin.TickClockForTest();
		check( step.name, plugin.CurrentPhaseForTest(), before, 1e-3 );

		// And then it must actually run at the new rate.
		const float resumed = plugin.CurrentPhaseForTest();
		host += 1.0;
		plugin.SetTime( host );
		plugin.TickClockForTest();
		check( "  one second later", plugin.CurrentPhaseForTest() - resumed,
		       SpeedFromParam( step.slider ), 1e-3 );
	}

	// Bar sync is deliberately NOT anchored: its contract is that phase 0 lands
	// on the bar line, so it must still be the plain transport product. If the
	// anchor ever leaks into it, beat sync stops meaning anything.
	{
		OrreryPlugin bar( false );
		bar.SetClockScaleForTest( 1.0 );
		bar.SetFloatParameter( PT_SYNC, static_cast< float >( Sync::Bar ) );
		bar.SetBeatInfo( 120.0f, 0.25f );//120bpm: a bar is two seconds
		bar.SetTime( 8.0 );
		bar.TickClockForTest();
		const float before = bar.CurrentPhaseForTest();

		bar.SetFloatParameter( PT_SPEED, 0.95f );
		bar.TickClockForTest();
		const float after = bar.CurrentPhaseForTest();

		// 4.25 bars in, so the phase is 4.25 * speed and the two speeds differ.
		const bool jumped = std::abs( after - before ) > 1e-3;
		std::printf( "speed %-34s %s\n", "Bar sync still re-locks", jumped ? "ok" : "FAILED" );
		if( !jumped )
			++failures;

		check( "  Bar phase == bars * speed", after, 4.25 * SpeedFromParam( 0.95f ), 1e-3 );
	}

	std::printf( "%s\n", failures == 0 ? "speed: all ok" : "speed: FAILURES" );
	return failures == 0 ? 0 : 1;
}


int main( int argc, char** argv )
{
	std::string outPath;
	std::string sequenceDir;
	std::string scriptPath;
	std::string shapesPath;
	std::string pathsPath;
	std::vector< std::string > settings;

	bool wantList   = false;
	bool wantClock  = false;
	bool wantSpeed  = false;
	bool wantMotion = false;
	bool wantRound  = false;
	bool wantMask   = false;
	bool wantEffect = false;

	float phase     = 0.0f;
	float hostTime  = -1.0f;   // negative means "pin the phase instead"
	double seconds  = 45.0;
	double fps      = 30.0;
	int width      = 1280;
	int height     = 720;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		const bool hasNext    = ( i + 1 < argc );

		if( arg == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( arg == "--sequence" && hasNext )
			sequenceDir = argv[ ++i ];
		else if( arg == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( arg == "--seconds" && hasNext )
			seconds = std::stod( argv[ ++i ] );
		else if( arg == "--fps" && hasNext )
			fps = std::stod( argv[ ++i ] );
		else if( arg == "--shapes" && hasNext )
			shapesPath = argv[ ++i ];
		else if( arg == "--paths" && hasNext )
			pathsPath = argv[ ++i ];
		else if( arg == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( arg == "--phase" && hasNext )
			phase = std::stof( argv[ ++i ] );
		else if( arg == "--time" && hasNext )
			hostTime = std::stof( argv[ ++i ] );
		else if( arg == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x != std::string::npos )
			{
				width  = std::stoi( size.substr( 0, x ) );
				height = std::stoi( size.substr( x + 1 ) );
			}
		}
		else if( arg == "--list" )
			wantList = true;
		else if( arg == "--clock" )
			wantClock = true;
		else if( arg == "--speed" )
			wantSpeed = true;
		else if( arg == "--motion" )
			wantMotion = true;
		else if( arg == "--round" )
			wantRound = true;
		else if( arg == "--mask" )
			wantMask = true;
		else if( arg == "--effect" )
			wantEffect = true;
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			fprintf( stderr, "unrecognised argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( outPath.empty() && sequenceDir.empty() && shapesPath.empty() && pathsPath.empty()
	    && !wantList && !wantClock && !wantSpeed && !wantMotion && !wantRound && !wantMask )
	{
		usage();
		return 2;
	}

	// Before any GL: neither of these touches the GPU, and a self-test that
	// needed a context would not run on a CI box without one.
	if( wantClock )
		return runClockTest();

	if( wantSpeed )
		return runSpeedTest();

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		fprintf( stderr, "could not create an OpenGL 4 core context\n" );
		return 1;
	}

	int status = 0;

	if( wantList )
	{
		OrreryPlugin plugin( wantEffect );
		status |= listParameters( plugin );
	}

	if( wantMotion )
		status |= motionCheck();

	if( wantRound )
		status |= roundCheck();

	if( wantMask )
		status |= maskCheck();

	if( !sequenceDir.empty() )
		status |= renderSequence( sequenceDir, scriptPath, width, height, seconds, fps, wantEffect );

	if( !shapesPath.empty() )
		status |= contactSheet( shapesPath, true );

	if( !pathsPath.empty() )
		status |= contactSheet( pathsPath, false );

	if( !outPath.empty() )
	{
		OrreryPlugin plugin( wantEffect );
		if( !prepare( plugin, width, height ) )
		{
			CGLDestroyContext( context );
			return 1;
		}

		for( const std::string& setting : settings )
		{
			if( !applySetting( plugin, setting ) )
			{
				CGLDestroyContext( context );
				return 2;
			}
		}

		if( hostTime >= 0.0f )
		{
			// Drive the real clock rather than pinning, so that Speed and Sync
			// do something. Without this they are correctly inert -- a pinned
			// phase ignores both -- and a dead-control sweep would report two
			// working parameters as broken.
			plugin.SetClockScaleForTest( 1.0 );//seconds, said out loud
			plugin.SetTime( static_cast< double >( hostTime ) );
			plugin.SetBeatInfo( 120.0f, 0.25f );
		}
		else
		{
			plugin.SetPhaseOverride( phase );
		}

		GLuint clip = wantEffect ? makeTestClip( width, height ) : 0;

		Target target = makeTarget( width, height );
		render( plugin, target, clip );

		const std::vector< unsigned char > image = flipRows( readBytes( target ), width, height );
		if( writePng( outPath, width, height, image ) )
			printf( "wrote %s (%dx%d, phase %.3f)\n", outPath.c_str(), width, height, phase );
		else
		{
			fprintf( stderr, "could not write %s\n", outPath.c_str() );
			status |= 1;
		}

		releaseTarget( target );
		if( clip != 0 )
			glDeleteTextures( 1, &clip );
		plugin.DeInitGL();
	}

	CGLDestroyContext( context );
	return status;
}
