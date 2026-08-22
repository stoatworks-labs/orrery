#!/usr/bin/env bash
#
# Everything, in the order that fails fastest.
#
# The build is universal on purpose. An arm64-only bundle builds and tests
# perfectly well here and then fails to load in an Intel Resolume, and the build
# log calls it a success either way -- so the architecture is checked with lipo,
# never with the log.
#
#     tools/verify.sh [BUILD_DIR]
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$REPO/build-verify}"

cd "$REPO"

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$1"; exit 1; }

#---------------------------------------------------------------------------
step "Submodule"
#---------------------------------------------------------------------------
if [[ ! -f external/ffgl/CMakeLists.txt ]]; then
	fail "FFGL SDK missing -- run: git submodule update --init --recursive"
fi
echo "ok   FFGL SDK present at $(git -C external/ffgl rev-parse --short HEAD)"

#---------------------------------------------------------------------------
step "Build (universal)"
#---------------------------------------------------------------------------
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)" >/dev/null
echo "ok   built"

#---------------------------------------------------------------------------
step "Bundles"
#---------------------------------------------------------------------------
for name in "Orrery" "Orrery Mask"; do
	binary="$BUILD/$name.bundle/Contents/MacOS/$name"

	[[ -f "$binary" ]] || fail "$name: no binary at $binary"

	# Universal. The failure this catches ships a plugin that simply does not
	# appear in half the Resolume installs it is given to.
	arches="$(lipo -archs "$binary")"
	[[ "$arches" == *arm64* ]]  || fail "$name: no arm64 slice (got: $arches)"
	[[ "$arches" == *x86_64* ]] || fail "$name: no x86_64 slice (got: $arches)"

	# The entry point. A bundle whose registration got dropped by the linker
	# still loads and still exports this -- but see the note below.
	# Captured, then matched from a herestring -- never `nm ... | grep -q`.
	# Under `set -o pipefail` a `grep -q` that finds its match exits
	# immediately, the writer upstream takes SIGPIPE, and the PIPELINE
	# reports failure even though the symbol is there. It is output-size
	# dependent, so it fires on the bigger binary first and looks
	# intermittent. A herestring is not a pipeline, so nothing can SIGPIPE.
	symbols=$( nm -gU "$binary" 2>/dev/null || true )
	grep -q '_plugMain' <<<"$symbols" || fail "$name: plugMain not exported"

	echo "ok   $name: $arches, plugMain exported"
done

#---------------------------------------------------------------------------
step "Checks"
#---------------------------------------------------------------------------
# The clock and the speed anchor first: neither needs a GL context, both take a
# couple of seconds, and both guard bugs an external user hit in the field
# rather than anything the renderer can show you.
"$BUILD/ortest" --clock
"$BUILD/ortest" --speed
"$BUILD/ortest" --presets

# --motion is the one that matters: it renders real frames and measures where
# every shape actually landed against what Motion.cpp said. It exercises the
# solver, the uniform upload, the vertex transform, the aspect correction, the
# distance function and the blend, all at once.
"$BUILD/ortest" --motion
"$BUILD/ortest" --round
"$BUILD/ortest" --mask

#---------------------------------------------------------------------------
step "Dead controls"
#---------------------------------------------------------------------------
# The only thing that catches a uniform whose name does not match the C++.
python3 tools/sweep.py --build "$(basename "$BUILD")"

#---------------------------------------------------------------------------
step "Browser demo"
#---------------------------------------------------------------------------
# demo/plugin.js carries this repo's shader text a second time, because a web
# page cannot include a C++ file. A change to Shaders.cpp that is not mirrored
# there is invisible until the demo behaves unlike the plugin, so compare them
# character for character.
python3 demo/tools/check_shaders.py

printf '\n\033[32mall green\033[0m\n'
