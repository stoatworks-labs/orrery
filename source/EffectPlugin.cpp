#include "Orrery.h"

/**
    The effect: the same shapes, over or into the incoming clip.

    Read the note in SourcePlugin.cpp on why this file is listed directly in its
    own target rather than in `orrery_core`. The short version is that the
    `CFFGLPluginInfo` below is the one thing the two plugins must NOT share.

    The plugin ID differs from the source's, and it has to: Resolume keys a
    saved composition's effect to that ID, so two plugins sharing one would make
    a composition ambiguous about which of them it meant.
*/
namespace
{
class OrreryEffect : public orrery::OrreryPlugin
{
public:
	OrreryEffect() :
		OrreryPlugin( true )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< OrreryEffect >,                    // Create method
	"OY02",                                           // Plugin unique ID of maximum length 4
	"Orrery Mask",                                    // Plugin name
	2,                                                // API major version number
	1,                                                // API minor version number
	0,                                                // Plugin major version number
	1,                                                // Plugin minor version number
	FF_EFFECT,                                        // Plugin type
	"Animated shape masks over the clip.\n\nPrimitive shapes moving on deterministic paths - for quick animated masks, and for chroma animations driving a pixel map.\n\nAn instance's placement is a pure function of its index and the phase. There is no simulation state anywhere, so nothing drifts with the frame rate and any frame renders on its own.\n\nStart from a Preset, at the bottom.",// Plugin description
	"Orrery FFGL effect"                              // About
);

extern "C" const char* OrreryEffectBuildStamp()
{
	return "orrery " ORRERY_VERSION " effect, built " __DATE__ " " __TIME__;
}
