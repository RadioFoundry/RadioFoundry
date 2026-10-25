# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Sector planning: which flash sectors to erase and write for an image, and which to leave alone.

Codec modes (docs/dev/release.md "Codec handling"):
  keep    write only the sectors the firmware image covers; the codec sectors are never erased or written, so the
          codec already in the radio stays. Refused when the image itself reaches into a codec sector.
  merge   the image plus the codec taken from the user's stock file: the codec sectors are erased and rewritten with
          the codec at its stock addresses (0xFF around it). Refused unless every codec window is covered. The image
          may share a sector with a codec window (never the window itself); that sector is written with both.

A written sector is always erased first and written from its start through its last data block (0xFF filling the
gaps), as one run of 1024-byte blocks numbered from 2: the AnyRoad bootloader's region form (hardware-dm1701.md 15.2).
"""
KEEP = "keep"
MERGE = "merge"
MODES = (KEEP, MERGE)

BLOCK = 1024
FIRST_BLOCK = 2


class PlanError(Exception):
    pass


class Step:
    """What happens to one sector. action: "write", "keep" (codec sector left alone) or "skip" (nothing to write)."""

    def __init__(self, sector, action, blocks=(), content="", note=""):
        self.sector = sector
        self.action = action
        self.blocks = list(blocks)	# [(block number, 1024 plain bytes)]
        self.content = content		# "app", "codec", "app+codec"
        self.note = note

    @property
    def data_bytes(self):
        return len(self.blocks) * BLOCK


class Plan:
    def __init__(self, board, mode, steps):
        self.board = board
        self.mode = mode
        self.steps = steps

    @property
    def erase(self):
        return [s.sector for s in self.steps if s.action == "write"]

    @property
    def writes(self):
        return [s for s in self.steps if s.action == "write"]

    @property
    def kept(self):
        return [s.sector for s in self.steps if s.action == "keep"]

    @property
    def total_blocks(self):
        return sum(len(s.blocks) for s in self.writes)

    def describe(self):
        b = self.board
        lines = ["board %s, codec mode: %s" % (b.name, self.mode)]
        for st in self.steps:
            s = st.sector
            head = "  sector %2d  0x%08X..0x%08X  %4d KB  " % (s.index, s.addr, s.end - 1, s.size // 1024)
            if st.action == "write":
                first, last = st.blocks[0][0], st.blocks[-1][0]
                lines.append(head + "ERASE + WRITE %s: blocks %d..%d (%d x %d bytes, up to 0x%08X)"
                             % (st.content, first, last, len(st.blocks), BLOCK, s.addr + len(st.blocks) * BLOCK - 1))
            elif st.action == "keep":
                lines.append(head + "keep (codec in the radio stays: %s)" % st.note)
            else:
                lines.append(head + "not touched" + (" (%s)" % st.note if st.note else ""))
        lines.append("erase %d sector(s): %s" % (len(self.erase), ", ".join("%d" % s.index for s in self.erase)))
        lines.append("write %d blocks (%d bytes)" % (self.total_blocks, self.total_blocks * BLOCK))
        if self.kept:
            lines.append("never erased or written: codec sector(s) %s" % ", ".join("%d" % s.index for s in self.kept))
        for lo, hi in b.protected:
            lines.append("never erased or written: the bootloader 0x%08X..0x%08X" % (lo, hi - 1))
        return lines


def _check_segments(board, segments):
    spans = []
    for addr, data in segments:
        if not data:
            continue
        lo, hi = addr, addr + len(data)
        if lo < board.app_base or hi > board.flash_end:
            raise PlanError("data at 0x%08X..0x%08X is outside the application flash 0x%08X..0x%08X"
                            % (lo, hi, board.app_base, board.flash_end))
        for plo, phi in board.protected:
            if lo < phi and hi > plo:
                raise PlanError("data at 0x%08X..0x%08X overlaps the bootloader" % (lo, hi))
        for slo, shi in spans:
            if lo < shi and hi > slo:
                raise PlanError("segments overlap at 0x%08X" % max(lo, slo))
        spans.append((lo, hi))
    if not spans:
        raise PlanError("nothing to write")
    if min(lo for lo, _ in spans) != board.app_base:
        raise PlanError("the image must start at the application base 0x%08X (the vector table)" % board.app_base)


def make_plan(board, segments, mode, codec_segments=(), codec_windows=None):
    """segments: the firmware image as [(addr, bytes)]; codec_segments: the codec as [(addr, bytes)] (merge mode);
    codec_windows: the windows that codec fills, [(addr, length)] (board.merge_windows(profile)). A board with several
    stock files has several layouts, so the board's own windows (their union) are what the image must avoid, and the
    merged file's are what the codec must cover. Default: the board's windows."""
    if mode not in MODES:
        raise PlanError("unknown codec mode %r" % mode)
    codec_secs = board.codec_sectors()
    codec_idx = {s.index for s in codec_secs}
    for addr, data in segments:
        if not data:
            continue
        win = [(lo, n) for lo, n in board.codec.windows if addr < lo + n and addr + len(data) > lo]
        if win:
            raise PlanError("the firmware image (0x%08X..0x%08X) overlaps the codec window 0x%08X+0x%X; the linker "
                            "keeps the codec windows free" % (addr, addr + len(data), win[0][0], win[0][1]))
        hit = sorted(s.index for s in codec_secs if s.overlaps(addr, addr + len(data)))
        # A board whose codec shares a sector with the application can only be flashed in merge mode: that sector is
        # erased and gets both (no STM32 radio has one since the codec moved). On a board that keeps the codec across
        # updates the firmware never takes part of a codec sector, in either mode
        if hit and (mode == KEEP or board.keep_codec_supported):
            raise PlanError("the firmware image reaches into codec sector(s) %s (0x%08X..0x%08X); a release image "
                            "never does (the linker keeps the codec sectors free)" % (hit, addr, addr + len(data)))
        # The firmware is one image, the application from the base
        if addr != board.app_base:
            raise PlanError("firmware data at 0x%08X..0x%08X is not the application (from 0x%08X) of the %s"
                            % (addr, addr + len(data), board.app_base, board.name))
    if mode == KEEP and codec_segments:
        raise PlanError("codec data given in keep mode")
    if mode == MERGE:
        for wlo, wlen in (board.codec.windows if codec_windows is None else codec_windows):
            covered = any(a <= wlo and wlo + wlen <= a + len(d) for a, d in codec_segments)
            if not covered:
                raise PlanError("merge mode needs the whole codec window 0x%08X+0x%X" % (wlo, wlen))
    allseg = [(a, d) for a, d in list(segments) + list(codec_segments) if d]
    _check_segments(board, allseg)

    windows_note = {}
    for s in codec_secs:
        parts = ["0x%08X..0x%08X" % (max(lo, s.addr), min(lo + n, s.end) - 1)
                 for lo, n in board.codec.windows if s.overlaps(lo, lo + n)]
        windows_note[s.index] = "codec window " + ", ".join(parts)

    steps = []
    for s in board.sectors:
        buf = None
        last = 0
        kinds = set()
        for kind, segs in (("app", segments), ("codec", codec_segments)):
            for addr, data in segs:
                lo, hi = max(addr, s.addr), min(addr + len(data), s.end)
                if lo >= hi:
                    continue
                if buf is None:
                    buf = bytearray(b"\xFF" * s.size)
                buf[lo - s.addr:hi - s.addr] = data[lo - addr:hi - addr]
                last = max(last, hi - s.addr)
                kinds.add(kind)
        if buf is None:
            if s.index in codec_idx and mode == KEEP:
                steps.append(Step(s, "keep", note=windows_note[s.index]))
            else:
                steps.append(Step(s, "skip", note="free" if s.index not in codec_idx else ""))
            continue
        nblocks = (last + BLOCK - 1) // BLOCK
        blocks = [(FIRST_BLOCK + i, bytes(buf[i * BLOCK:(i + 1) * BLOCK])) for i in range(nblocks)]
        content = "+".join(k for k in ("app", "codec") if k in kinds)
        steps.append(Step(s, "write", blocks, content))
    plan = Plan(board, mode, steps)
    if mode == KEEP and any(s.index in codec_idx for s in plan.erase):
        raise PlanError("internal: keep mode would erase a codec sector")	# guarded above; never reached
    return plan
