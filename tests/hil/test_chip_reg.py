# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""CHIP_REG's SoC form reads what READ_MEM reads (docs/spec/debuglink.md 3.24).

A register both commands reach (the interrupt controller's polarity word on the HR_C7000, all ones from start-up; the
SCB's CPUID word on a Cortex-M4, r0p1 on the chips here) reads the same through each, and an address outside the platform's register windows is refused. Reads only, nothing
transmitted. Skipped on firmware without CHIP_REG and on platforms without SoC register windows.
"""
import pytest

from rfoundry_hil import DebugLinkError, skip
from rfoundry_debug import CHIP_AT32F43X, CHIP_GD32F30X, CHIP_HRC7000, ST_REFUSED, ST_UNAVAILABLE

ST_UNKNOWN_CMD = 1
# Per chip family: a register word with no read side effects that both windows hold, its start-up value, and an address
# outside the register windows (main SRAM)
CORTEX_M4_CPUID = (0xE000ED00, 0x410FC241, 0x20000000)
PROBES = {CHIP_HRC7000: (0x17000004, 0xFFFFFFFF, 0x00010000), CHIP_GD32F30X: CORTEX_M4_CPUID,
          CHIP_AT32F43X: CORTEX_M4_CPUID}


@pytest.mark.feature("debuglink")
def test_soc_register_matches_read_mem(radio):
    family = radio.link.chip()["family"]
    try:
        radio.link.soc_reg(PROBES.get(family, (0,))[0])
    except DebugLinkError as e:
        if e.status in (ST_UNKNOWN_CMD, ST_UNAVAILABLE):
            skip("not_built", "firmware or platform without CHIP_REG's SoC registers")
        if family in PROBES:
            raise
    if family not in PROBES:
        skip("not_built", f"no SoC register probe for chip family {family}")
    addr, expect, outside = PROBES[family]
    via_reg = radio.link.soc_reg(addr)
    via_mem = int.from_bytes(radio.link.read_mem(addr, 4), "little")
    print(f"0x{addr:08X}: CHIP_REG 0x{via_reg:08X}, READ_MEM 0x{via_mem:08X}")
    assert via_reg == via_mem == expect
    with pytest.raises(DebugLinkError) as e:
        radio.link.soc_reg(outside)
    assert e.value.status == ST_REFUSED
