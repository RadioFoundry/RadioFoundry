# OpenDigitalRadio

Free (MIT) firmware for DMR handheld and mobile radios, built to run on many radios from one generic core.

The first targets are the Baofeng DM-1701 (STM32F405, HR_C6000, AT1846S) and the Baofeng DM-32UV (HR_C7000 / C-SKY CK803S, FD6818). Next are the TYT MD-UV380, MD-9600 and MD-380, the Radioddity GD-77 and the GD-88.

Status: early. Specs and the first bring-up are in progress. See `docs/`.

## Layout

| Path | What |
|---|---|
| `core/` | Radio-independent code: DMR stack (FEC, call state machine, data service, privacy, hotspot, SFR), codeplug, settings, UI, CPS protocol, GPS |
| `hal/` | Interfaces (ops structs) the core uses: DMR modem, RF, audio, AMBE codec, flash, display, input, timers, the `board_t` descriptor |
| `drivers/` | Chip drivers shared across radios (RF chips, displays, SPI NOR flash, modems, keypads) |
| `platform/` | Per-SoC startup, clocks, interrupts and RTOS glue (`hrc7000`, `stm32f4`, …) |
| `boards/` | One directory per radio: the board descriptor, pin map, driver choice, linker script, loader and codec profiles |
| `third_party/` | Permissively licensed upstream code (FreeRTOS, ST HAL, TinyUSB), unchanged where possible |
| `tools/` | Build, packing, flashing and codec extraction tools |
| `tests/host/` | Host unit and integration tests |
| `docs/spec/` | Hardware facts, modem behavior, protocols, formats, known gotchas |
| `docs/ux/` | UI design: principles, task flows, wireframes, usability tests |

Core code never tests a radio define. Anything specific to a radio belongs in `boards/`, `platform/` or a driver, and the core reaches it through `hal/` and the board descriptor.

## AMBE codec

DMR voice needs the AMBE+2 vocoder, which is proprietary (DVSI). It is not in this repository or in any released binary. Each radio uses exactly the codec its own stock firmware shipped with. `tools/codec_extract.py` takes the stock firmware file you supply, checks it against the board's profile, and merges the codec into your image on your machine. Without a codec the firmware runs FM only.

## License

MIT, see `LICENSE`. Third-party components keep their own permissive licenses; see `LICENSES/` and `PROVENANCE.md`.
