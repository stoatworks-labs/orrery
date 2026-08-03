#pragma once

/**
    The primitives.

    Each is a signed distance function evaluated per pixel in the fragment
    shader, in the instance's own local space where the shape has unit radius.
    An SDF is the right representation here for three reasons that are all
    load-bearing rather than aesthetic:

    - **Antialiasing is one line.** `smoothstep` across a distance of one screen
      pixel, obtained from `fwidth`, gives a correctly filtered edge at any size
      and any rotation without multisampling the whole frame.
    - **Outline is free.** A stroke of width w around any shape is
      `abs( d ) - w`. That is one expression that works for all eight shapes,
      rather than eight more sets of geometry.
    - **Softness is free.** Widening the smoothstep turns a hard edge into a
      feathered one, which is the difference between a shape that pixel-maps
      cleanly to a fixture and one that reads as a soft mask.

    ## Why the C++ side carries no SDFs

    Only the bound. The shader owns the distance functions and there is no
    mirrored C++ copy of them, because nothing needs one: the offline harness
    checks *where each shape landed* (against Motion.cpp) and *how much of the
    frame it covered*, not what its edge did pixel by pixel. A mirrored SDF would
    be a second implementation to keep in step in exchange for a test that
    largely restates the shader. See AGENTS.md.
*/
namespace orrery
{
enum class Shape
{
	Circle = 0,
	Square,
	Triangle,
	Hexagon,
	Star,
	Cross,
	Ring,
	Bar,

	Count
};

/// The name shown in the host's dropdown.
const char* ShapeName( Shape shape );

/**
    How far the shape reaches from its own centre, at unit radius, before
    rounding and outline are applied.

    This sizes the quad each instance is drawn on, and **it is the one number
    here that can produce a visibly wrong picture if it is too small**: the quad
    is the only thing being rasterised, so a shape that reaches past its own quad
    is not clipped in a way that looks like clipping -- it loses a corner, and a
    lost corner on a rotating square reads as the shape "wobbling" rather than as
    a bounds bug.

    Erring high costs only overdraw on a few hundred pixels per instance, so
    these are the true extents rounded up, and Orrery.cpp adds a further margin
    for the outline and the feather on top.
*/
float ShapeBound( Shape shape );

} // namespace orrery
