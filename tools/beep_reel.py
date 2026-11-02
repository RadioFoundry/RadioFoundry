#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
#
# Makes the beep reel, a video of the UI simulator in which every beep in core/radio/beep.h plays in the situation
# where a radio would play it, with captions naming each beep. It's for hearing what a change to beep.c sounds like in
# context. The scenes are in tests/host/beepReel.c; see also docs/spec/radio-features.md section 4.
#
# Usage:
#   tools/beep_reel.py [--out build/beep-reel] [--beep-dir core/radio] [--lenient] [--geom 160x128] [--scale 4]
#       Builds tests/host/beepReel with the beep.c and beep.h in --beep-dir, runs it, and encodes OUT/beep_reel.mp4
#       with ffmpeg. Each beep on its own goes to OUT/beeps/<name>.wav. Pass --lenient when --beep-dir is from
#       another commit (beepReel.c explains why).
#   tools/beep_reel.py --compare BEFORE AFTER --artifact DIR
#       Puts together the pull request artifact for .github/workflows/beep-reel.yml: DIR/before.mp4, DIR/after.mp4,
#       and DIR/beeps/<name>.before.wav and <name>.after.wav for each beep whose sound changed. A beep that only
#       exists on one side gets just that side's WAV.
#
# The video is MP4 with H.264 and AAC because that plays in browsers, on GitHub and on phones. The scale has to be
# even so the screen's pixels line up with the 4:2:0 chroma grid, or colors bleed at the edges. The captions use
# ffmpeg's subtitles filter, which needs an ffmpeg built with libass, so we check for it before doing any work.
import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, "tests", "host")
FPS = 20        # one frame per 50 ms sim tick
BAND = 24       # height of the caption strip in pixels, per step of --scale


def make(args):
    out = os.path.abspath(args.out)
    beep_dir = os.path.abspath(args.beep_dir)
    if args.scale < 2 or args.scale % 2:
        sys.exit("beep_reel: --scale must be even (2, 4, ...)")
    if shutil.which("ffmpeg") is None:
        sys.exit("beep_reel: needs ffmpeg")
    filters = subprocess.run(["ffmpeg", "-hide_banner", "-filters"], capture_output=True, text=True).stdout
    if not re.search(r"^\s*\S+\s+subtitles\s", filters, re.M):
        sys.exit("beep_reel: this ffmpeg has no subtitles filter for the captions: it needs one built with libass")
    subprocess.run(["make", "-s", "-C", HOST, "beepReel", f"BEEP_DIR={beep_dir}"], check=True)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)
    band = BAND * args.scale
    cmd = [os.path.join(HOST, "beepReel"), "--out", out, "--geom", args.geom, "--map", args.map,
           "--scale", str(args.scale), "--band", str(band)]
    if args.lenient:
        cmd.append("--lenient")
    if subprocess.run(cmd).returncode != 0:
        sys.exit("beep_reel: the reel failed its checks (above)")
    ffmpeg = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                        "-framerate", str(FPS), "-i", os.path.join("frames", "%05d.png"), "-i", "reel.wav",
                        "-vf", f"scale=iw*{args.scale}:ih*{args.scale}:flags=neighbor,"
                               f"pad=iw:ih+{band}:0:0:color=0x101010,subtitles=reel.ass",
                        "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-tune", "animation", "-pix_fmt", "yuv420p",
                        "-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-movflags", "+faststart", "-shortest",
                        "beep_reel.mp4"], cwd=out)    # in OUT, because the subtitles filter can't take an escaped path
    if ffmpeg.returncode != 0:
        sys.exit("beep_reel: ffmpeg failed (above)")
    print(os.path.relpath(os.path.join(out, "beep_reel.mp4")))


def compare(args):
    before, after, art = args.compare[0], args.compare[1], args.artifact
    if os.path.isdir(art):
        shutil.rmtree(art)
    os.makedirs(art)
    shutil.copyfile(os.path.join(before, "beep_reel.mp4"), os.path.join(art, "before.mp4"))
    shutil.copyfile(os.path.join(after, "beep_reel.mp4"), os.path.join(art, "after.mp4"))
    names = set()
    for side in (before, after):
        names.update(n[:-4] for n in os.listdir(os.path.join(side, "beeps")) if n.endswith(".wav"))
    changed = []
    for name in sorted(names):
        b = os.path.join(before, "beeps", name + ".wav")
        a = os.path.join(after, "beeps", name + ".wav")
        if os.path.exists(b) and os.path.exists(a) and filecmp.cmp(b, a, shallow=False):
            continue
        changed.append(name)
        os.makedirs(os.path.join(art, "beeps"), exist_ok=True)
        for src, side in ((b, "before"), (a, "after")):
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(art, "beeps", f"{name}.{side}.wav"))
    print("changed beeps: " + (", ".join(changed) if changed else "none"))


def main():
    p = argparse.ArgumentParser(description="The beep reel: every beep, on the simulated screen.")
    p.add_argument("--out", default=os.path.join(ROOT, "build", "beep-reel"))
    p.add_argument("--beep-dir", default=os.path.join(ROOT, "core", "radio"),
                   help="where beep.c and beep.h come from")
    p.add_argument("--lenient", action="store_true",
                   help="another commit's beep.h: a beep without a scene, or a scene's beep it lacks, only warns")
    p.add_argument("--geom", default="160x128")
    p.add_argument("--map", default="K")
    p.add_argument("--scale", type=int, default=4)
    p.add_argument("--compare", nargs=2, metavar=("BEFORE", "AFTER"))
    p.add_argument("--artifact")
    args = p.parse_args()
    if args.compare:
        if not args.artifact:
            p.error("--compare needs --artifact")
        compare(args)
    else:
        make(args)


if __name__ == "__main__":
    main()
