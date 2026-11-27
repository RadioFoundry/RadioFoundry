<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- Copyright (c) 2026 RadioFoundry Theme Pack contributors -->

# Theme packs

Theme packs let you change the color palettes available under **Settings > Display > Theme** without replacing the
radio's firmware each time. A pack contains names and color values only. It does not contain program code or a
codeplug, and installing one does not flash firmware.

## First-time setup

Theme-pack support is optional. The current flash map supports it on the DM-32UV. The firmware build must enable
`CONFIG_THEME_PACKS`. On this radio, the pack uses data-flash sector `0x71D000`, which overlaps the satellite TLE
storage area, so build with TLE storage disabled:

```sh
make BOARD=dm32 CONFIG_THEME_PACKS=y CONFIG_SATELLITE_TLE=n
```

Install that firmware first. The feature is off in the normal build, and the theme uploader will report that a radio
is unsupported if its running firmware does not expose theme-pack support.

## Create a pack

1. Open the [Theme Pack Builder](https://photogdoug.github.io/RadioFoundry-sat-fw-Themes/tools/themes/builder.html)
   in a Chromium browser such as Chrome or Edge.
2. Choose a **Radio model** and **Firmware screen** above the preview. The builder puts that model's radio drawing
   around a screen captured from the firmware UI simulator at the model's display geometry. You can switch among the
   DMR home screen, simpler home screen, Display settings, group-call receive screen, and transmit screen when that
   view is available for the model. Color screens apply your edited palette to the captured UI; monochrome screens
   show their actual grayscale output. The RT10 has no display, so it is not listed.
3. Choose one of the four stock palettes or create your own by editing the color fields. The starter library contains
   only the themes in RadioFoundry source: Dark, Light, High contrast, and Night (red). Your saved themes are kept in
   this browser.
4. Save each custom theme by name, then add themes from the library to **Your pack**. A pack can hold up to 10
   palettes, including stock palettes and your own saved palettes.
5. Download the `.rftheme` file. Keep the editable JSON download too if you want to move the pack to another browser
   or continue editing it later.

## Install or remove a pack

1. Open the [Theme Pack Uploader](https://photogdoug.github.io/RadioFoundry-sat-fw-Themes/tools/themes/index.html)
   in the same Chromium browser.
2. Connect to the radio's programming cable when the browser asks, then choose your `.rftheme` file and install it.
   The uploader transfers only the bounded theme-pack data area.
3. On the radio, open **Settings > Display > Theme**. The installed theme names appear after Dark, Light, High
   contrast, and Night (red).
4. To remove the pack, use **Remove theme pack** in the uploader. Stock themes remain available.

The radio stores one pack at a time. Installing another pack replaces it. The pack occupies one erase sector, so power
loss during replacement can make the previous pack unavailable; the stock themes remain available. Removing a pack
does not alter the firmware, codeplug, channels, or other radio settings.

## Build the site locally

From the repository root, install the dependencies in `site/requirements.txt` and run:

```sh
python3 site/build.py --serve
```

Open `http://localhost:8000/tools/themes/builder.html` to create a pack and
`http://localhost:8000/tools/themes/index.html` to install or remove one.
