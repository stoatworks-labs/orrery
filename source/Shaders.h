#pragma once

/**
    Two passes, no framebuffer.

    1. **Background.** One fullscreen quad. For the source that is the background
       colour; for the effect it is the incoming clip, or nothing at all in the
       mask modes that build their output only where the shapes are.
    2. **Shapes.** One attributeless quad, `glDrawArraysInstanced`, one instance
       per shape.

    ## Why instanced quads and not one fullscreen pass

    The obvious shape renderer evaluates all N distance functions at every pixel
    and takes the minimum. That costs `pixels x instances` -- 64 shapes at 4K is
    530 million distance evaluations a frame to draw a few thousand lit pixels.

    Drawing each shape on its own quad costs `shape area x instances` instead,
    which is the number that actually scales the way the picture looks like it
    should: a screen full of small dots is cheap, and it stays cheap when the
    output is 4K because the dots do not get bigger.

    It also gets the compositing right for free. The instances rasterise in
    index order, so ordinary alpha blending composites them in that order, and
    the three blend modes are three pieces of GL state rather than three
    branches inside a loop.

    ## Why there is no FBO anywhere

    Every mode Orrery offers -- including the effect's Reveal and Hide, which
    look like they need a mask buffer -- is reachable with a background pass and
    a blend function:

    - **Reveal** samples the clip *inside the shape fragment* and writes it with
      the shape's coverage as alpha, over a transparent background.
    - **Hide** draws the clip, then punches the shapes out of it with
      `glBlendFunc( GL_ZERO, GL_ONE_MINUS_SRC_ALPHA )`.

    That is worth stating plainly because reaching for an FBO here is the
    obvious move and it walks straight into two SDK bugs: `FFGLFBO::Release`
    leaks its colour texture, and `FFGLFBO::Initialise` allocates under a
    `ScopedTextureBinding` whose destructor **clears the binding to 0 rather
    than restoring it**, so allocating a buffer silently unbinds the input
    texture for exactly the frames on which the buffer was allocated. Orrery
    never allocates one, so neither bug can reach it.

    ## Alpha is premultiplied

    The same convention as the rest of the fleet, and what Resolume expects.
    The shape shader outputs `vec4( rgb * coverage, coverage )`, so Over is
    `GL_ONE, GL_ONE_MINUS_SRC_ALPHA`.

    ## Things that will catch you out

    - **A uniform name that does not match the C++ is silently ignored.**
      `glGetUniformLocation` returns -1 and `glUniform` on -1 is a documented
      no-op, so a control can be stone dead while everything compiles, links,
      loads and renders. `tools/sweep.py` is the only thing that catches it.

    - **`FFGLShader::Set` has no integer-vector overload and reaching for the
      float one is silent.** `Set( name, someInt, someInt )` against an `ivec2`
      resolves to the `(float,float)` overload, issues a `glUniform2f` against
      an integer uniform, and leaves it at zero with nothing anywhere the plugin
      can see. Every uniform here is `float`, `vec2`, `vec3`, `vec4` or a single
      `int` for that reason.

    - **`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are
      GLSL reserved words**, and a shader that will not compile surfaces only at
      runtime, as "the plugin does nothing". That is what Diag is for.
*/
namespace orrery
{
/// Fullscreen quad, attributeless -- built from `gl_VertexID`.
extern const char* const kBackgroundVertexShader;

/// The background: a flat colour, or the clip, or nothing.
extern const char* const kBackgroundFragmentShader;

/// One quad per instance, placed from the `Xform` uniform array.
extern const char* const kShapeVertexShader;

/// The distance functions, the outline, the feather and the four sample modes.
extern const char* const kShapeFragmentShader;

/// Prepended to the shape fragment shader for the effect build, which adds the
/// clip sampler and the mask modes that read it. Goes in after the `#version`
/// line, which is why it is a define rather than a second copy of the shader.
extern const char* const kEffectDefine;

} // namespace orrery
