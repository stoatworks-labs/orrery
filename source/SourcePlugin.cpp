#include "Orrery.h"

/**
    The generator: shapes over their own background, no input.

    **This file is listed directly in the OrrerySource target, not in
    orrery_core.** Both plugins share the class; what they do not share is the
    `CFFGLPluginInfo` below, and putting either registration in the shared
    library would register both plugins into both bundles.

    It is also why the shared library is an OBJECT library rather than a STATIC
    one. `CFFGLPluginInfo` registers itself from a file-scope constructor and
    nothing ever references it by name, so in an archive the linker is entitled
    to drop the whole translation unit -- giving a bundle that loads, exports
    `plugMain`, and reports that it contains no plugins.

        nm -gU Orrery.bundle/Contents/MacOS/Orrery | grep plugMain
*/
namespace
{
class OrrerySource : public orrery::OrreryPlugin
{
public:
	OrrerySource() :
		OrreryPlugin( false )
	{
	}
};
} // namespace

static CFFGLPluginInfo PluginInfo(
	PluginFactory< OrrerySource >,                      // Create method
	"OY01",                                             // Plugin unique ID of maximum length 4
	"Orrery",                                           // Plugin name
	2,                                                  // API major version number
	1,                                                  // API minor version number
	0,                                                  // Plugin major version number
	1,                                                  // Plugin minor version number
	FF_SOURCE,                                          // Plugin type
	"Primitive shapes moving on deterministic paths",   // Plugin description
	"Orrery FFGL source"                                // About
);

extern "C" const char* OrrerySourceBuildStamp()
{
	return "orrery " ORRERY_VERSION " source, built " __DATE__ " " __TIME__;
}
