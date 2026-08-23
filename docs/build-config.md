<!-- SPDX-License-Identifier: MIT -->

# Build configuration

Features that cost flash, and values that differ per radio, are chosen at build time. Defaults live in
`config/features.mk`, a board overrides them in `boards/<board>/config.mk`, and the make command line overrides both:

```
make BOARD=dm1701                        # the board's defaults
make BOARD=dm1701 CONFIG_DMR=n           # FM only
make BOARD=dm1701 CONFIG_VFO=n           # programmed channels only
make BOARD=dm1701 CONFIG_DISPLAY_INSET=3 # smaller screen margins
```

Each switch reaches C as `CONFIG_<NAME>` = 0 or 1, always defined: test it with `#if CONFIG_X`, never `#ifdef`.
`core/config.h` holds the same defaults (all on) for host tests and checks the dependencies. Switches can also leave
whole source directories out of the build (`Makefile`, `platform/*/platform.mk`). A sub-feature follows its parent
unless it is set: `CONFIG_DMR=n` turns off every DMR feature, and asking for a sub-feature without its parent is an
error. Changing a switch rebuilds everything (the build keeps the flags in `build/<board>/flags`, checked when make
starts). `make BOARD=<b> print-config` lists the resolved switches.

The firmware version (`ODR_GIT`) is the commit plus `-dirty` when the tree differs from it (tracked changes, or
untracked sources in the built directories); only the few files that use it are rebuilt when it changes.

## Switches

| Switch | Default | What it covers |
|---|---|---|
| `CONFIG_FM` | y | analog FM receive and transmit |
| `CONFIG_SUBTONE` | = FM | CTCSS / DCS encode and decode |
| `CONFIG_DMR` | y | DMR voice: modem driver, call stack, AMBE FEC, vocoder hosting, PCM audio |
| `CONFIG_DMR_PRIVACY` | = DMR | basic privacy (Motorola / Anytone BP) on the voice path, `core/dmr/dmrPrivacy.c` (`docs/spec/dmr-voice.md` 5) |
| `CONFIG_DMR_TALKER_ALIAS` | = DMR | talker alias decode and our own alias TX (`docs/spec/dmr-voice.md` 3) |
| `CONFIG_DMR_SMS` | = DMR | text messages (not ported yet) |
| `CONFIG_DMR_ARS` | = DMR | ARS registration (not ported yet) |
| `CONFIG_DMR_LRRP` | = DMR | LRRP location (not ported yet) |
| `CONFIG_DMR_SIGNALING` | = DMR | CSBK signaling: call alert, radio check (not ported yet) |
| `CONFIG_DMR_IP_GATEWAY` | n | IP over DMR packet data through the USB network adapter (`docs/spec/usb-network.md`) |
| `CONFIG_SFR` | n | single frequency repeater (not ported yet) |
| `CONFIG_USB` | y | USB device (CDC serial) |
| `CONFIG_USB_NETWORK` | = IP_GATEWAY | USB network adapter personality: CDC-NCM, switched with the serial one at run time (debug link op 15) |
| `CONFIG_HOTSPOT` | = DMR and USB | MMDVM modem / hotspot over USB |
| `CONFIG_DEBUGLINK` | = USB | USB debug link for bench automation: diagnostics, screenshots, key / PTT injection, log ring (`docs/spec/debuglink.md`) |
| `CONFIG_VFO` | y | tuning a frequency by hand |
| `CONFIG_VFO_FM` | = VFO and FM | analog VFO |
| `CONFIG_VFO_DMR` | = VFO and DMR | digital VFO (not built yet) |
| `CONFIG_DIAG` | y | the diagnostics page |
| `CONFIG_FONT_XL` | n | 16x32 font, for 240 px and wider displays |

Derived (not set by hand): `CONFIG_DMR_DATA_CORE`, the shared packet data / CSBK code, on when any data feature is.

## Values

| Value | Default | Meaning |
|---|---|---|
| `CONFIG_DISPLAY_INSET` | 0 (DM-1701: 5) | pixels hidden by the bezel or lens at each edge; the UI draws inside them |
| `CONFIG_DISPLAY_INSET_TOP` / `_BOTTOM` / `_LEFT` / `_RIGHT` | = `CONFIG_DISPLAY_INSET` | one edge |

## Adding a feature

1. Add the switch with a comment and its default to `config/features.mk`, to `FEATURES`, and to the dependency checks.
2. Add the default and the checks to `core/config.h`.
3. Guard the code, and leave a directory out in the Makefile when the whole directory belongs to the feature.
4. Add a row here, and check the build links with the switch off (`make BOARD=<b> CONFIG_X=n BUILD=build/test-x`).
   `tools/ci.sh` builds every board with each default-on switch off alone, an FM-minimal and a maximal
   configuration, all with zero warnings, and reports the image sizes (`docs/testing/README.md`).
