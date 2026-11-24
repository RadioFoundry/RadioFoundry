<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Backlight curve

How to measure a PWM backlight and set the board's levels for the core's brightness settings (off, 10 % to 100 %) so
each step looks even. It needs the radio, its owner's eyes and a build with `CONFIG_BACKLIGHT_TEST=y`
(**Settings > Advanced > Backlight tests**, [build-config.md](../build-config.md)). The board provides the display's
optional backlight hooks (`hal_display_t` `set_backlight_raw`, `backlight_raw`, `set_backlight_rate`); the levels go in
the board's backlight table, which its `set_backlight` maps a percent through.

The method uses only frequency, duty and time, which any PWM backlight has. How a board makes its PWM (an MCU timer,
a PWM chip, an LED driver) is its own; the AT-D878UV's MCU timer is the worked example throughout, marked
**Example**.

## Terms

| Term | Meaning |
|---|---|
| Rate | the PWM frequency, in Hz |
| Period | 1 / rate: one on and one off |
| Duty | the part of each period the output is on, 0 to 100 % |
| On-time, off-time | duty x period, and the rest of the period |
| CV | the board's raw setting for duty, in its own steps: 0 is off, the board's maximum (`backlight_raw_max`) is on through the whole period. Page 1 shows it as **CV**, with its duty and on / off time |
| Level | the CV the board's table gives a brightness setting |

**Example: AT-D878UV.** The light is driven by a GD32F30x MCU timer (TIMER7, channel 1's complementary output) in PWM
mode 0. The timer counts its input clock, divided by the prescaler, from 0 up to the reload value, then starts over:
one count past the reload value is one period. The output is on while the count is below the compare value, so CV is
the compare value, and duty = CV / (reload + 1).

| Value | AT-D878UV | Set by |
|---|---|---|
| Timer clock | 120 MHz | the MCU clock setup |
| Prescaler | 3 (divide by 4) | firmware; the rate page changes it |
| Reload | 4095 (0xFFF): 4096 counts a period | firmware |
| Rate | 120 MHz / (3 + 1) / (4095 + 1) = 7324 Hz | |
| One count | 1 / 30 MHz = 33 ns | |
| Full | CV 4096 (reload + 1) | the timer: a compare value above the reload value holds the output on |

```latex
rate = \frac{f_{timer}}{(prescaler + 1)(reload + 1)}, \qquad duty = \frac{CV}{reload + 1}
```

Changing only the prescaler changes the rate and leaves duty for a given CV the same, which is how the rate page
works there.

## What to find

| Item | Meaning | Step |
|---|---|---|
| Full | 100 %: a CV that holds the output on through the whole period | 1 |
| Warm-up | the on-time a pulse needs before any light shows | 2 |
| Hang | how long the light stays on after the output turns off | 3 |
| Rate | the PWM rate: as slow as the eye allows without flicker, clear of RF interference | 4 |
| Floor | the level for 10 %: the dimmest a user should get, not where the light goes out | 5 |
| Curve | the eight levels between floor and full | 6 |

## The test pages

| Page | Controls | Shows |
|---|---|---|
| 1, CV | knob 1; **Up** / **Down** 50 (1 on a knob with end stops); digits type a value (applied at as many digits as the maximum has; **#** applies fewer, **Back** deletes one); **\*** the next rate; **OK** held: full and back | rate, CV; duty and on / off time each period |
| 2, brightness | knob, **Up** / **Down**: 0 to 100 % in 10 % steps | the level |
| 3, PWM rate | knob, **Up** / **Down**: the board's rates | the rate in Hz |

**OK** steps to the next page. Pages the board has no hooks for are left out.

## 1. Check full

- Find, in the PWM hardware's datasheet, what setting holds the output on through the whole period, and what setting
  holds it off. Full must be the first, with no table entry: a setting one step short leaves a short gap every
  period.
- A light driven from a complementary or inverted output: check its polarity on the radio first.

**Example: AT-D878UV.** The GD32F30x user manual (16.1, output PWM function) gives, in PWM mode 0, an output always
active with the compare value above the reload value, and always inactive at 0. Full is 4096; the old 100 % of
4095 was off for one count (33 ns) every period.

## 2. Warm-up: the first glow

Page 1, room light kept the same throughout. At each of several rates (**\***), fastest to slowest:

1. From CV 0, **Up** until any glow shows, then **Down** once.
2. Knob up until the glow first shows. At that level the screen can't be read: press **Up** twice, read the value and
   take off 100. With the debug link connected, the value can be read there instead.
3. Optional: from 50 above, knob down until the glow goes, for the other edge.

Read the on-time page 1 shows for each value and look across the rates:

| Seen | Meaning |
|---|---|
| The same on-time at every rate | a warm-up delay of that length |
| The same duty at every rate | the driver averages the PWM: a turn-on level in duty |
| The same on-time at slow rates, less at fast ones | a warm-up delay; at fast rates the gap is too short for the driver to turn off, so duty takes over |

A line fit of on-time against period over the slower rates gives the delay (its intercept) and a duty term (its
slope). At a rate whose period is close to the delay, most of the duty range gives no light.

**Example: AT-D878UV** (variant 1 panel):

| Rate | Glow CV | Duty | On-time |
|---|---|---|---|
| 29296 Hz | 2603 | 63.5 % | 21.7 µs |
| 14648 Hz | 2350 | 57.4 % | 39.2 µs |
| 7324 Hz | 1877 | 45.8 % | 62.6 µs |
| 3662 Hz | 1013 | 24.7 % | 67.5 µs |
| 1831 Hz | 515 | 12.6 % | 68.7 µs |

The same on-time from 7324 Hz down: a warm-up of about 65 µs (fit: 62 µs + 1.3 % duty). At the standard 7324 Hz it
takes the first 46 % of duty.

## 3. Hang: the first dip

Page 1. Hold **OK** for full, hold it again to come back, then step down from the top (**Down** coarse, knob fine),
holding **OK** to compare with full. Find the highest value that looks dimmer than full, and read its off-time.

| Seen at two rates | Meaning |
|---|---|
| The same duty | the eye sees a duty step; the hang is shorter than the smaller off-time |
| The same off-time | a hang of that length: a shorter gap never turns the light off |

**Example: AT-D878UV.** The first dip was CV 4064 at 7324 Hz (off 1.07 µs) and 4065 at 29296 Hz (off 0.25 µs): the
same duty, so the hang is under 0.3 µs.

## 4. Pick the rate

- Slower gives back the range the warm-up takes, down to where flicker shows: check page 3 at a mid level.
- Check the chosen rate for spurs and desense on receive and transmit before settling on it. A board can use a second
  rate while transmitting.

**Example: AT-D878UV.** With a 65 µs warm-up, 3662 Hz loses about 25 % of duty and 1831 Hz about 13 %, against 46 %
at 7324 Hz.

## 5. Set the floor

Page 1 at the chosen rate: stop where the screen is as dim as anyone should want. Light below it is no reason to move
it down.

**Example: AT-D878UV.** CV 2137 (52 %) at 7324 Hz, still lit well below it.

## 6. Fit the curve

Generate the levels from floor to full once, store them as constants, and look at each neighboring pair on page 2.
Start from a power curve:

```latex
CV(n) = floor + (full - floor) \cdot t^{\gamma}, \quad t = (n - 1) / 9, \; n = 1 \ldots 10
```

| Seen | Change |
|---|---|
| Low settings look alike | lower γ |
| High settings look alike | raise γ |
| Both ends look alike | γ on duty can't fix both: space the on-time above the warm-up instead of the duty |

A setting between levels takes the level above (1-10 % is 10 %, 91-100 % is full).

**Example: AT-D878UV.** γ = 2 from 2137 to 4096 gives 2137, 2161, 2234, 2355, 2524, 2742, 3008, 3322, 3685, then
full. At 7324 Hz, 10 % and 20 % looked close: the warm-up, not γ.

## 7. Record it

- The table and its comment in the board's backlight header, with a host test of the floor, full and the formula.
- Warm-up, hang, rate, floor and curve, with the date, temperature and panel variant, in the radio's bring-up record.
