# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""The names of a build's directory, image and release files, from the board id, its build options and the values
chosen. The Makefile names its build directory and image the same way (BUILD, TARGET); tools/release.py,
odrflash --release and tools/boards.py json --for build use these functions. Imports nothing, so tools/boards.py can
use it without loading the generated registry.

options: [(name, [value, ...])], each option's default value first (board.mk <NAME>_VALUES). chosen: {name: value}
for any of them; the others take their default. variant: a release variant (config/variants/<name>.mk), None or
"full" for the full build, or VARIANT (the text "{variant}"), which stays in the name as a placeholder for "" or
"-<variant>" (tools/boards.py json --for build).
"""

VARIANT = "{variant}"


def full(options, chosen):
    """Every option's value: the chosen ones, the defaults for the rest."""
    return {n: (chosen or {}).get(n, vals[0]) for n, vals in options}


def suffix(options, chosen):
    """"-<value>" for each option not at its default, in the board's option order ("-vhf")."""
    f = full(options, chosen)
    return "".join("-" + f[n] for n, vals in options if f[n] != vals[0])


def image_id(board_id, options, chosen):
    """The board id with the non-default values: the image's name in a build and in a release (md380-vhf)."""
    return board_id + suffix(options, chosen)


def _variant(variant):
    if variant == VARIANT:
        return variant
    return "-" + variant if variant and variant != "full" else ""


def build_dir(board_id, options, chosen, variant=None):
    """make's build directory, from the source tree: build/<board>[-<variant>][-<value>...]."""
    return "build/" + board_id + _variant(variant) + suffix(options, chosen)


def build_image(board_id, options, chosen, variant=None, first_stage=False):
    """The image make writes and a release carries: odr_<image id>.bin in the build directory, or
    odr_<image id>_install.bin (the first stage and the application) on a board with a first stage."""
    return "%s/odr_%s%s.bin" % (build_dir(board_id, options, chosen, variant), image_id(board_id, options, chosen),
                                "_install" if first_stage else "")


def release_name(name_id, version, ext="bin"):
    """A release file: odr-<name id>-<version>.<ext>."""
    return "odr-%s-%s.%s" % (name_id, version, ext)


def release_id(board_id, options, chosen, variant=None):
    """The name id of a build's release files: odr-<board>[-<variant>][-<value>...]-<version>.bin."""
    return board_id + _variant(variant) + suffix(options, chosen)
