<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Temperature sensors and the fan

Many radios carry a temperature sensor the firmware can read: a thermistor at the power amplifier, one on the board or
in the battery pack, and every MCU's own. A board lists its sensors in `board_t.temp` (`hal/hal_temp.h`); the core
reads them on request (`core/app/temp.c`) for the debug link and SNMP. Which radios have which sensors:
[feature-matrix.md](../feature-matrix.md) and each radio's hardware page.

What a board does with a reading itself (running a fan, cutting the TX power back) stays in the board. The core only
reports.

## 1. A sensor

| Field | Meaning |
| --- | --- |
| `kind` | `TEMP_KIND_PA`, `TEMP_KIND_BOARD`, `TEMP_KIND_MCU` or `TEMP_KIND_BATTERY` |
| `read_mv` | the sensor's output in mV, or NULL for a sensor that only gives degrees |
| `read_dc` | the temperature in 0.1 degree C, read directly (a digital sensor, an MCU's sensor with its factory constants), or NULL to use the curve. It may answer false: no reading now |
| `curve`, `curve_len` | points of mV and 0.1 degree C, in mV order, rising or falling with temperature the same way throughout. NULL while the board doesn't know the sensor's curve |

`hal_temp_t.fan_on` (optional) says whether the fan the firmware drives runs now.

## 2. Readings

`temp_read` gives each sensor's kind, its mV (when it has `read_mv`) and its temperature (from `read_dc`, else from
the curve). `temp_curve_dc` interpolates linearly between the two points around the reading and rounds to the nearest
0.1 degree. A reading outside the curve gets the end point's temperature. A board without a curve still reports mV,
so a sensor is useful before it is calibrated: a curve is two or more readings at known temperatures.

## 3. Where it shows

| Where | What |
| --- | --- |
| Debug link view `sensors` ([debuglink.md](debuglink.md) 3.7) | `sensor.count`, then per sensor `sensor.<i>.kind` (`pa board mcu battery`), `sensor.<i>.mv`, `sensor.<i>.dc`, `-` where there is none; `fan.on` (`-` without a fan hook) |
| SNMP ([snmp.md](snmp.md) 4) | rfoundryTempTable: a row per sensor (the first four) with kind, mV and 0.1 degree C, a cell missing where the sensor has none; rfoundryFanState |

Code: `hal/hal_temp.h`, `core/app/temp.c`; tests in `tests/host/tempTest.c`.
