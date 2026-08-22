#!/usr/bin/env python3
"""Move every parameter and fail if any of them made no difference to the frame.

**This is the only thing in the repo that catches a dead control**, and it is
not a theoretical risk. A GLSL uniform whose name does not match the C++ is
silently ignored -- `glGetUniformLocation` returns -1 and `glUniform` on -1 is a
documented no-op -- so a slider can be stone dead while everything compiles,
links, loads and renders.

Neither `ortest --motion` nor `--mask` will catch it. `--motion` checks where the
shapes landed, and a uniform that is never set still lands them somewhere
consistent; `--mask` only ever exercises one setting of everything else.

## Why there is a context table

Most of these parameters are *supposed* to do nothing in the default
configuration, and a naive sweep would report a dozen false failures:

- `Ratio X` is Lissajous and Bounce only.
- `Direction` is Drift only. `Grid Columns` is Grid only.
- `Roundness` and `Spin` are invisible on a **circle** -- a rotated circle is the
  same circle, and a circle has no corners to round.
- `Seed` feeds hashes that nothing consumes until `Scatter` or `Size Variation`
  is up.
- `Colour` is ignored unless `Colour Mode` is off White.
- `Blend` needs shapes that **overlap** at partial opacity: over an opaque black
  background, three non-overlapping white shapes are white in all three modes.
- `Speed` and `Sync` need the real clock, which is what `--time` is for.

So each entry below says what else has to be true for the parameter to be
capable of doing anything. Getting one of these wrong shows up as a failure, not
as a silent pass.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose]
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Applied to every render: enough shapes, large enough to see, spread around the
# path so that parameters keyed off an instance's own phase have something to
# act on.
BASE = ["Count=0.45", "Size=0.66", "Spread=1.0"]

# Parameters that only exist for the effect bundle. Swept with --effect, over a
# synthetic clip.
EFFECT_ONLY = {"Mix", "Mask Mode"}

# Parameters the effect ignores, so they are only ever swept against the source.
SOURCE_ONLY = {
    "Background",
    "Background_Green",
    "Background_Blue",
    "Background Opacity",
}

# What else has to be true for a parameter to be able to do anything.
# Values are extra --set assignments, plus optional extra CLI arguments.
CONTEXT = {
    # Path-specific controls.
    "Ratio X":      ["Path=1"],                    # Lissajous
    "Ratio Y":      ["Path=1"],
    "Direction":    ["Path=2"],                    # Drift
    "Grid Columns": ["Path=4", "Count=0.75"],      # Grid, enough to fill it
    "Grid Rows":    ["Path=4", "Count=0.75"],

    # Invisible on a circle.
    "Roundness":    ["Shape=1"],                   # Square: it has corners
    "Spin":         ["Shape=1"],
    "Spin Phase":   ["Shape=1"],

    # Feed hashes that nothing reads until something consumes them.
    "Seed":         ["Scatter=1.0"],

    # The pulse envelope's width does nothing with no pulse.
    "Pulse Width":  ["Pulse Size=1.0"],

    # Colour is ignored on White, and White and Solid are identical while the
    # swatch is still white.
    "Colour Mode":  ["Colour_Green=0.0", "Colour_Blue=0.0"],
    "Colour":       ["Colour Mode=1"],
    "Colour_Green": ["Colour Mode=1"],
    "Colour_Blue":  ["Colour Mode=1"],
    "Hue Spread":   ["Colour Mode=2"],             # Hue Spread

    # Over, Add and Max are the same picture unless shapes overlap at less than
    # full opacity.
    "Blend":        ["Count=0.8", "Size=0.9", "Opacity=0.5"],

    # The light has nothing to light with the shading turned off, and a shape
    # only a few pixels across has nowhere to put a gradient.
    "Light":        ["Shade=1.0", "Count=0.05", "Size=0.9", "Path Size=0.0"],
    "Shade":        ["Count=0.05", "Size=0.9", "Path Size=0.0"],
}

# Parameters that need the host clock running rather than a pinned phase.
NEEDS_CLOCK = {"Speed", "Sync"}

# Parameter *kinds* that have no scalar value to sweep. "Audio" is an FFT
# buffer: its float value is meaningless, so sweeping it proves nothing and
# reports a false dead. ortest feeds every render a synthetic spectrum, and the
# two Audio knobs are the sweepable proof that the buffer is wired through.
SKIP_KINDS = {"buffer"}

# The values every non-option parameter is swept across.
#
# The awkward numbers are load-bearing, and 0, 0.5, 1 is a trap. **An evenly
# spread chase is periodic in phase with period 1/N**: with Spread at 1 and 14
# instances, advancing Phase by exactly 0.5 moves instance i onto where instance
# i+7 was, and since they are all the same colour the frame comes back
# pixel-identical. A sweep of 0, 0.5, 1 therefore reports a perfectly working
# Phase slider as dead -- which it duly did, and which is a far more convincing
# false alarm than a real dead control, because it fails at both of the values a
# person would reach for first.
#
# 0.137 and 0.611 are not rational multiples of any instance count this sweeps.
SWEEP_VALUES = [0.0, 0.137, 0.611, 1.0]

# The two ends of the sweep. Option parameters are swept across their elements
# instead, which is what OPTION_RANGE is for.
OPTION_RANGE = {
    "Shape": 8,
    "Path": 5,
    "Sync": 4,
    "Colour Mode": 4,
    "Blend": 3,
    "Mask Mode": 4,
}


def read_png(path):
    """Decode a PNG to raw bytes. Enough of the format for our own writer's
    output -- 8-bit RGBA, one IDAT, filter 0 on every row."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IDAT":
            idat += body
        pos += 12 + length

    return zlib.decompress(idat)


def render(ortest, out, settings, effect, clock, verbose):
    args = [str(ortest), "--out", str(out), "--size", "480x270"]
    if effect:
        args.append("--effect")
    if clock:
        args += ["--time", "2.0"]
    else:
        args += ["--phase", "0.37"]
    for setting in settings:
        args += ["--set", setting]

    if verbose:
        print("   ", " ".join(args))

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"ortest failed: {result.stderr.strip()}")

    return read_png(out)


def parameters(ortest, effect):
    """Name and default of every parameter, in declaration order."""
    args = [str(ortest), "--list"]
    if effect:
        args.append("--effect")

    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"ortest --list failed: {result.stderr.strip()}")

    found = []
    for line in result.stdout.splitlines()[1:]:
        # id, name (may contain spaces), type, default
        parts = line.split()
        if len(parts) < 3:
            continue
        kind = parts[-2]
        name = " ".join(parts[1:-2])
        found.append((name, kind))

    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    ortest = REPO / args.build / "ortest"
    if not ortest.exists():
        print(f"no ortest at {ortest} -- build first", file=sys.stderr)
        return 2

    dead = []
    checked = 0

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        for effect in (False, True):
            for name, kind in parameters(ortest, effect):
                if kind in SKIP_KINDS:
                    continue
                if effect and name not in EFFECT_ONLY:
                    continue
                if not effect and name in EFFECT_ONLY:
                    continue
                if effect and name in SOURCE_ONLY:
                    continue

                context = CONTEXT.get(name, [])
                clock = name in NEEDS_CLOCK
                base = BASE + context

                if name in OPTION_RANGE:
                    values = [float(i) for i in range(OPTION_RANGE[name])]
                else:
                    values = SWEEP_VALUES

                frames = []
                for value in values:
                    out = tmp / "sweep.png"
                    frames.append(
                        render(ortest, out, base + [f"{name}={value}"],
                               effect, clock, args.verbose))

                checked += 1
                if all(f == frames[0] for f in frames[1:]):
                    where = "effect" if effect else "source"
                    dead.append(f"{name} ({kind}, {where})")
                    print(f"  DEAD {name}")
                elif args.verbose:
                    print(f"  ok   {name}")

    print()
    if dead:
        print(f"sweep: {checked} parameters, {len(dead)} made no difference:")
        for entry in dead:
            print(f"  - {entry}")
        return 1

    print(f"sweep: {checked} parameters, all live")
    return 0


if __name__ == "__main__":
    sys.exit(main())
