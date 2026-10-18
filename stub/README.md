# stub: flash access for the HR_C7000 radios (DM-32UV, DM-1801 Pro)

A small RAM monitor and the host tool that drives it. The tool loads the monitor into the radio's RAM, either through the
chip's serial boot ROM (radio switched on with the cable in) or, with `CONFIG_STUB_ENTRY=y`, from a running ODR over the
debug link (`docs/spec/debuglink.md` 3.18, `docs/spec/hardware-dm32.md` 17.2). It then reads the program flash and the data flash, and installs ODR builds into the application area of the program
flash (`--write-app`), with every block checked.

| File | What |
|---|---|
| `stubtool.py` | the host tool; needs Python 3 and pyserial; has both stub images embedded; `--self-test` runs without a radio |
| `stub_rom.bin` | the monitor linked for the boot-ROM load (RAM 0x10000, 24 MHz clock), 5,288 bytes |
| `stub_odr.bin` | the monitor linked for entry from ODR (RAM 0x17000, 84 MHz clock, SSI divider 4), 5,272 bytes |
| `build_db25d_stubs.sh`, `embed_stub.py` | build the DB25-D runtime-entry stubs for both RAM layouts, and embed one in `stubtool.py` |
| `db25d_update.sh` | DB25-D: enter, write the install image, restart, check |
| `USER-GUIDE.md` | how to use the tool, in plain language |

The binaries are built from the monitor's source (C, about 1,100 lines, written for this project) with the csky-elf GCC 14
toolchain (`-Os`). The source and its protocol document are kept with the reverse-engineering notes outside this
repository for now. Check the embedded images with `sha256sum stub_*.bin` against `stubtool.py --self-test`
(which decodes and checks them).

## Stubs for entry from a running ODR

The monitor is position dependent: it is linked for the RAM span a running ODR offers (`HRC_STUB_BASE` / `HRC_STUB_SIZE`,
reported in the debug link's STUB INFO reply, `docs/spec/debuglink.md` 3.18). `stubtool --odr-enter` reads the span from
the firmware and loads the embedded stub linked for that base, so one tool serves every layout:

| Base | Size | Radio | Source of the span |
|---|---|---|---|
| 0x17000 | 96 KB | DM-32UV, DM-1801 Pro | `platform/hrc7000/platform.h` (the default) |
| 0x30000 | 128 KB | DB25-D | `boards/db25d/config.mk`, a build without `CONFIG_DMR` (`db25d.ld`) |
| 0x1D000 | 72 KB | DB25-D | `boards/db25d/config.mk`, `CONFIG_DMR=y` (`db25d_dmr.ld`, inside the codec window) |

Only the running firmware's span matters. The image being installed can be any build: from a build without DMR to one
with it, between two DMR builds, or back. `--odr-stub FILE` overrides the embedded choice (a stub built elsewhere); the
tool reads the addresses the image was linked with and refuses one that does not match the span the firmware reports,
before anything is sent, because jumping into a stub linked for another base needs a power cycle. A base with no embedded
stub (a new board) gets a message that names `--odr-stub`.

Rebuild the DB25-D stubs with `stub/build_db25d_stubs.sh` (needs the monitor source and a
build host with the csky-elf docker image; the header lists the variables), then embed them with `stub/embed_stub.py`.
The 0x30000 image rebuilds byte for byte. `stub/db25d_update.sh IMAGE [PORT]` is the one command for an update.

## STM32F4 radios (DM-1701, MD-380)

A second stub, with its source here, for the STM32F405 radios behind the AnyRoad / TYT bootloader. A running ODR built
with `CONFIG_STUB_ENTRY=y` loads it over the USB debug link; it then installs builds (changed sectors only, the codec
kept), backs up and verifies the internal flash, and reads and writes the external SPI flash. Spec: [docs/spec/stub-stm32f4.md](../docs/spec/stub-stm32f4.md).

| File | What |
|---|---|
| `stm32f4/` | the stub's C source, linker script and Makefile: `make -C stub/stm32f4` writes `build/stub-stm32f4/f4stub.bin` (about 5.3 KB) |
| `f4stub.py` | its host side, with the stub build embedded (checked by SHA-256; `--embed` puts a new build in, and CI fails while a fresh build differs); `stubtool.py` hands the DM-1701 and MD-380 to it (`--odr-enter`, or `--attach --board dm1701`); `--self-test` runs without a radio |

Status (2026-10-16): built and host tested (`tests/host`: the protocol engine, and the host tool against the real
engine through a pipe), and run on the DM-1701 ([stub-stm32f4.md](../docs/spec/stub-stm32f4.md) 11).

## HR_C7000 status

Status (2026-10-15): reads of both flash chips, entry from ODR, `--write-app` (a 93-sector build in 71 s, changed sectors only,
both directions, ODR up after the restart) and the provisional baud switch were run on a DM-1801 Pro. The DM-32UV is untested.
