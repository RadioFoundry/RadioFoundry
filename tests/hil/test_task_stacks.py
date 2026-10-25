# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Every task keeps some stack free: the debug link's tasks view section (docs/spec/debuglink.md 3.7).

The firmware reports each FreeRTOS task's least free stack since it started (the kernel's high water mark). After
the radio has done a little work (every view section read, a screenshot), every task must still have at least
MIN_FREE bytes free (64; `RFOUNDRY_HIL_STACK_MIN_FREE` overrides it) and none may be missing from the table
(`tasks.untracked=0`). The idle, timer, start, radio, input, UI and host tasks must be there. Radio only, nothing
transmitted; skipped on firmware without the section or built with `CONFIG_TASK_STACKS=n`. A task near the limit here
but passing `make stack-check` points at the check's table (docs/dev/stack-check.md); a task failing both needs a
larger stack.
"""
import os

import pytest

from rfoundry_hil import as_int, skip

MIN_FREE = int(os.environ.get("RFOUNDRY_HIL_STACK_MIN_FREE", "64"))
# Created on every radio: the kernel's own two, the core's five (the board's audio, dmr, usb, gps vary)
EXPECTED = ("IDLE", "Tmr_Svc", "start", "radio", "input", "ui", "host")


def tasks_section(radio):
    """The tasks section's keys, found by name (its number differs between builds), or None without it."""
    i = 0
    while i != 0xFF and i < 64:
        _, nxt, kv = radio.link.view_section(i)
        if kv.get("section") == "tasks":
            return kv
        i = nxt
    return None


@pytest.mark.feature("task_stacks")
def test_every_task_has_stack_left(radio):
    radio.view()    # every section: the host task's deepest replies
    radio.screenshot()
    kv = tasks_section(radio)
    if kv is None:
        skip("not_built", "firmware without the tasks view section")
    if kv.get("tasks.built") == "0":
        skip("not_built", "firmware built with CONFIG_TASK_STACKS=n")
    free = {k[len("tasks.free."):]: as_int(v) for k, v in kv.items() if k.startswith("tasks.free.")}
    print("least free stack (bytes):", ", ".join(f"{n} {b}" for n, b in sorted(free.items(), key=lambda x: x[1])))
    assert as_int(kv["tasks.count"]) == len(free), kv
    assert as_int(kv["tasks.untracked"]) == 0, "tasks created past the firmware's table"
    missing = [n for n in EXPECTED if n not in free]
    assert not missing, f"tasks missing: {missing} (have {sorted(free)})"
    low = {n: b for n, b in free.items() if b < MIN_FREE}
    assert not low, f"tasks with under {MIN_FREE} bytes of stack left: {low}"
    least = min(free, key=free.get)
    assert as_int(kv["tasks.min_free"]) == free[least] and kv["tasks.min_task"] in free, kv
