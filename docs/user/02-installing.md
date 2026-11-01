<!-- SPDX-License-Identifier: MIT -->

# Installing

The recommended way to install RadioFoundry, update it, back up a radio and restore it is the **web installer** on the
project's web site, [radiofoundry.org](https://radiofoundry.org/flash/index.html). It runs in a Chromium-based browser
(Chrome, Edge) and needs nothing installed. The files you load into it never leave your computer. Nothing here changes
the radio's calibration.

The command line tool **rfoundry** does the same from a terminal. It is the advanced option: use it for scripts, for a
browser that can't talk to a radio, or for a local build ([Advanced: rfoundry](#advanced-rfoundry-on-the-command-line)).

What differs from radio to radio (the stock firmware file, the keys that start the bootloader, the install route) is in
your radio's "Installing" section of the [Radio reference](23-radios.md). A radio that runs a manufacturer's stock
firmware and has a migration chapter is covered in [Moving from the stock firmware](README.md#moving-from-the-stock-firmware).

## What you need

| Item | Where it comes from |
|---|---|
| A computer with Windows, macOS or Linux, or an Android phone or tablet | Android: Chrome, and a USB OTG adapter for the cable ([FAQ](21-faq.md)) |
| A Chromium-based browser | For the web installer |
| Your radio's stock firmware file | The manufacturer's file, in a version the installer recognizes. Your radio's "Installing" section names it and where to find it |
| The radio's USB or programming cable | The cable that came with the radio |

The release firmware has everything except the AMBE codec. The installer either keeps the codec already in your radio,
or takes it out of the stock firmware file on your computer and writes it to the radio. The stock file also gives the
installer the key the radio's bootloader expects. Once the radio runs RadioFoundry, the installer reads the key from
the radio instead. Files are checked by SHA-256, and unknown files are refused.

Back up the radio's current codeplug before you flash, with the program you use now (the manufacturer's CPS, or qdmr).
RadioFoundry uses a different codeplug layout from the stock firmware. If you go back to stock later, you need that
backup. The web installer's own backup (below) holds it too.

## Install with the web installer

Open the [web installer](https://radiofoundry.org/flash/index.html), choose **Install**, and follow the steps (a radio
that runs RadioFoundry already: [Updating RadioFoundry](#updating-radiofoundry)):

1. **Radio.** Pick your radio.
2. **Build.** Pick **Full** (every feature the radio supports), **Amateur**, **Commercial**, or **Custom** to choose
   features one by one. Features the radio can't have are marked. A custom build is made when you continue and takes
   under a minute. When your browser asks for a language the radio can show but the build leaves out, the page offers
   to include it (Settings > Display > Language on the radio, [Settings](17-settings.md)). **Radio language** picks
   the language the radio starts in, preset to the page's language when the build has it. Picking a language the
   build leaves out adds it to the build. You can change it on the radio later. A build too big for the radio says
   by how much: turn some features off in **Custom**, or pick a smaller build, and build again.
3. **Current firmware.** Say what the radio runs now:

   | Choice | When |
   | --- | --- |
   | **RadioFoundry** | Connect the radio and turn it on. The installer reads the installed version. |
   | **Original firmware** | The firmware installed at the factory. |
   | **Another open firmware** | Another open firmware project, such as one of the OpenGD77 family. |
   | **Other / Not sure** | Any other firmware, or you don't know what the radio runs. |

   The installer asks for the stock firmware file only when it needs it for the codec or the key. If you choose
   RadioFoundry and the radio runs an OpenGD77-family firmware, the installer says so and offers **Another open
   firmware** instead. A RadioFoundry build without the debug link answers the same way.
4. **Back up.** The installer saves a backup file (a `.zip`) on your computer: the firmware, the channels and settings
   and the calibration. Keep it. Where the original firmware can't be read first, the installer backs the radio up
   right after the installation, before anything else changes.

   On a radio installed over its programming cable the cable is slow, so the installer shows how long each part
   takes. The program memory is required, unless you use a stock file instead (below). **All channels and settings** is optional and off by default: the
   installer never changes them, and the whole data memory can take many minutes. If you have the radio's stock
   firmware file, **Use a Stock File Instead** skips the firmware backup, before or during the read. The installer
   then reads only the codec area and takes the codec from the file in the next step. The firmware in the radio is
   not backed up, but the stock file can restore the original firmware.

   **Without a firmware backup or a stock file, the installer has no codec to give the radio.** If you skip the
   firmware backup and select no supported stock file, **Install without the codec** is the only way on. The installer
   then warns, above the Install button, that this installation will not include the DMR voice codec: analog FM will
   work, but DMR voice will not until you install RadioFoundry again with a supported manufacturer firmware file.
   Installing again keeps your channels and settings.

   The same choice comes when the backup finds a firmware version whose codec RadioFoundry has not mapped yet. The
   backup is still complete; the codec step asks for a supported stock firmware file instead.

5. **Install.** Put the radio in its bootloader when asked ([your radio's keys](23-radios.md)). The installer writes
   the changed parts and restarts the radio. Do not unplug the radio until it says the installation is complete.
6. **Verify.** The installer reads what the radio reports: the version it wrote, and "DMR voice is ready" when the
   codec is in place.
7. **Channels.** If the radio still holds its original firmware's codeplug, the installer converts it to the
   RadioFoundry layout and writes it to the radio, then reads it back and shows how many channels, zones and contacts
   came over. This runs on its own once the backup holds the original codeplug. If you skipped the backup, the installer
   asks first: on some radios the converted codeplug replaces the original bytes. An update of RadioFoundry skips this
   step. If the installer can't read, convert or write the original codeplug, it says so and the radio starts without
   channels: convert the backup as described in [Backing up and restoring](#backing-up-and-restoring).

On Windows the bootloader needs a driver once ([Windows: the bootloader driver](#windows-the-bootloader-driver)).

Then continue with [First boot](03-first-boot.md).

### Coming from another open firmware

On a radio that runs an OpenGD77-family firmware, the installer can back up the whole radio through that firmware
before installing, and take the update key and the voice codec from the backup. You then need no stock firmware file.
The installer offers this only on radios where it works. On others the stock firmware file is needed as usual.

1. On **Current firmware**, choose **Another open firmware** and then **Check the Radio**, with the radio connected and
   on. Choosing **RadioFoundry** finds the other firmware too. The installer reports its version.
2. Choose **Back Up and Read**. The backup takes about a minute and a half. It only reads; nothing on the radio
   changes. Keep the file it saves.
3. The installer says whether it found the update key and the codec. If it found both, continue: the codec step uses
   the codec from your radio. If it found neither or only one, select your stock firmware file on the codec step. The
   same holds when you skip the backup or the radio reports another model: the codec step asks for the stock file.
   The open firmware's installer may have put a codec from another radio's firmware there, wherever it chose; the
   backup finds it, and your radio's section in [Radio Reference](23-radios.md) says which files count.
4. Install as usual through the bootloader.

If the radio stops responding during the backup, turn it off and on, then choose **Continue Reading**: the backup
continues where it stopped. **Save What Was Read** keeps what was read so far.

To go back, choose **Restore** with that backup and **Previous open firmware**. The firmware is written back; the
channels and settings are not. Write them with that firmware's own programming software if they are missing.

This path has not been tested on a real radio yet. Keep your stock firmware file at hand.

### Updating RadioFoundry

Start the radio normally with RadioFoundry running and choose **Update** in the web installer:

1. Connect the radio and press **Connect**. One port list shows the radio's USB port and programming cables.
2. The installer shows the radio, the installed version, the new version and the radio's build (Full, Amateur,
   Commercial or Custom). The update installs the same build of the new release.
   - **Edit Build** opens the build step, to change features before the update.
   - A preset build lists the features the new release adds to it.
   - A custom build lists the features added since it was made. Choose **On** or **Off** for each before updating.
   - Firmware too old to report its build gets the build step, starting from the closest match.
3. **Save a backup first** is optional and off. An update keeps the channels, settings, calibration and codec.
4. Press **Update**. The installer builds the image, writes it and checks the version.

The radio goes straight from RadioFoundry into the loader: no keys to hold, no stock file. The codeplug and the settings
stay: they live in flash that updating never touches. **Install** still works on a radio that runs RadioFoundry, with
every question asked.

### Backing up and restoring

**Back Up** saves a file with the radio's firmware, codec, channels and settings. **Restore** takes that file and
writes back the whole radio, only the firmware, or only the channels and settings. It can also return the radio to its
original firmware, from your stock firmware file. Any version of the manufacturer's file for your radio works, not only
the one the installer needs for the codec. A restore from the update mode works even when the firmware is damaged.

**Back Up** asks what the radio runs now, as the install does. **RadioFoundry** reads it over the debug link.
**Another open firmware** reads it through that firmware, on radios where the installer supports it. A radio on its
original firmware is not backed up here: choose Install, which backs it up right after the installation.

Each radio lists every backup option and the time it takes: the program memory, the factory calibration and all
channels and settings. **Calibration only** (or **Factory calibration**) reads just the factory calibration. On a radio
that keeps a calibration copy in its program memory, that option is named **Program Memory & Calibration**. On a radio
whose channels and settings are in NAND memory, **All channels and settings** reads the whole NAND, which can take
several minutes.

Without the program memory, a backup can't be restored to your initial radio firmware using these tools. Restore
writes back the program memory only; a backup's calibration and data memory are kept for the record. When you pick
such a backup, Restore says so and offers **Select a Firmware File**: pick a manufacturer firmware file for your
radio instead, in a version the installer supports. A whole program memory dump of the radio also works, even when its
firmware version has not been mapped yet: Restore puts it back as it is, never touching the bootloader. An install
can't use that dump's codec until the version is mapped, so the installer offers to send the dump to the RadioFoundry
team for mapping and asks for a supported manufacturer firmware file instead.

A backup you unpacked and zipped again (macOS unpacks downloads by itself) still restores.

If the backup holds the original firmware's codeplug, open the file in the
[Codeplug Editor](04-programming.md#the-codeplug-editor). It converts the channels, zones and contacts to the
RadioFoundry layout.

## Windows: the bootloader driver

Windows has no driver for the USB bootloader of radios that flash over USB, so neither the web installer nor rfoundry
can open it until you install one. Do this once per computer, with Zadig:

1. Download Zadig from [zadig.akeo.ie](https://zadig.akeo.ie) and run it. It needs no install.
2. Put the radio in its bootloader (your radio's keys are in its [Radio reference](23-radios.md) section) and plug it
   in.
3. In Zadig's list, pick **Digital Radio in USB mode**. The USB ID boxes show `0483` `DF11`. If it isn't listed,
   choose **Options > List All Devices**.
4. Choose **WinUSB** as the driver (to the right of the green arrow) and click **Install Driver** (or **Replace
   Driver**). Wait until Zadig says it succeeded.
5. Unplug the radio, put it back in its bootloader and connect again.

Only the bootloader needs this. The radio's serial port, once it runs RadioFoundry, works without a driver.

Vendor tools that use the same USB ID (the manufacturer's upgrade tool or CPS) need their own driver back. In Device
Manager, uninstall the device with **Delete the driver software for this device** checked, then replug.

## Install from the command line with a kit

When the web installer can't install, it offers a **manual install kit**: a zip with everything to install the firmware
you chose from a terminal on this computer, with rfoundry. Choose and build the firmware as usual; the kit takes it from
there.

| The installer offers it | Where |
|---|---|
| The browser has no WebUSB or Web Serial (Safari, Firefox) | The start page says so. The step that needs the radio shows the kit instead, with a picker for the stock firmware file |
| The bootloader connect or the write fails | A line under the error |
| The radio doesn't answer the cable within three minutes, or preparing it fails | A line under the error |

| In the kit | What |
|---|---|
| `README.txt` | Setup on Windows, macOS and Linux (Python, the bootloader driver, the serial port), how to put the radio in update mode, and the commands to check (`--dry-run`) and install, with the file names filled in |
| `firmware/` | The image and its manifest |
| `stock/` | Your stock firmware file, if you gave one. Otherwise the README says which file to put there |
| `rfoundry.py`, `tools/`, `stub/`, `requirements.txt` | rfoundry from the same version as the web installer. It runs from the folder; `pip` installs its packages in a virtual environment |

It needs Python 3.8 or later. The README says how to install Python and rfoundry's packages on each system, and the
driver a USB bootloader needs on Windows and Linux. The kit is put together in the browser; nothing is uploaded. A
radio that rfoundry can't install gets no kit: use Chrome or Edge on a computer. The [feature
matrix](../feature-matrix.md) and your radio's section of the [Radio reference](23-radios.md) say how each radio
installs.

## Advanced: rfoundry on the command line

Each release carries **rfoundry**, one file per system, `rfoundry-<version>-<system>`. You can also `pipx install` it
([tools/rfoundry/README.md](../../tools/rfoundry/README.md)).

| System | Once |
|---|---|
| Windows | Install the bootloader's driver ([above](#windows-the-bootloader-driver)). Windows may warn about the unsigned rfoundry file |
| macOS | In Terminal: `xattr -d com.apple.quarantine rfoundry-*` and `chmod +x rfoundry-*` (the file isn't signed) |
| Linux | `chmod +x rfoundry-*`. If rfoundry says access is denied, it prints the udev rule to add. To reach the radio's serial port once it runs RadioFoundry (`rfoundry probe`, updates), join the `dialout` group: `sudo usermod -aG dialout $USER`, then log in again |

The examples call it `rfoundry`, and the stock file `STOCK.bin`. If you'd rather not pass the stock file every time, let
rfoundry keep the key:

```
rfoundry key --stock STOCK.bin --save
```

### Flash

```
rfoundry flash --release latest --stock STOCK.bin
```

Some radios come in hardware versions one image can't cover, such as a UHF and a VHF board. They have one image per
version. rfoundry picks it from what a radio already running RadioFoundry reports, otherwise it asks, or takes
`--option BAND=vhf`. Your radio's section names its versions.

rfoundry downloads the newest release, checks it, decides about the codec ([below](#keep-the-codec-or-write-it)) and
prints a plan: which flash sectors it will erase and write, and which it leaves alone. Add `--dry-run` to see the plan
without touching the radio. Then it asks you to put the radio in its bootloader:

1. Turn the radio off.
2. Connect the cable to the radio and the computer.
3. Hold the radio's bootloader keys and turn it on. The screen stays dark.
4. Release the keys.

rfoundry finds the bootloader, asks once more, and writes. When it finishes, the radio restarts into RadioFoundry, and
rfoundry checks what the radio reports.

### Keep the codec, or write it

| The radio runs | rfoundry |
|---|---|
| RadioFoundry, and DMR voice works | Keeps the codec: it writes only RadioFoundry's own sectors. It asks the running radio over USB before you switch it to the bootloader (or run `rfoundry probe` first) |
| Anything else, or you're not sure | Writes the codec: from the copy it saved from this radio the last time it ran RadioFoundry, otherwise from the stock file (`--stock`) |

The stock firmware keeps the codec where RadioFoundry's own firmware goes, so a radio fresh from stock needs the stock
file once. After that, rfoundry keeps a copy of the codec on your computer.

If rfoundry keeps a codec that isn't actually in the radio, nothing breaks: RadioFoundry checks the codec before it
uses it. FM works, and DMR calls show "No codec: voice off". Flash again with `--stock` to fix it.

### Update

```
rfoundry flash --release latest
```

### Building it yourself

Developers build with `make BOARD=<board>` and flash the result with
`rfoundry flash build/<board>/rfoundry_<board>.bin --board <board>` (same codec choices). With the stock file in
`codecs/`, the build also writes `rfoundry_<board>_codec.bin`, which carries the codec: `rfoundry flash
build/<board>/rfoundry_<board>_codec.bin` flashes it with no `--board`, and `--stock` is needed only for the
bootloader key. See [building](../dev/building.md).

## No codec ("No codec: voice off")

DMR calls show on the screen but there's no audio: the radio has no usable codec. Run the installer again and give it
the stock firmware file, or from a terminal:

```
rfoundry flash --release latest --codec merge --stock STOCK.bin
```

## When the installer can't use a file

If a firmware file can't be used, the installer says what it found and what to do:

| It says | Do this |
|---|---|
| This File Is for Another Radio | Pick your radio's file, or open the installer for the radio the file is for |
| This Is Not the Manufacturer's Firmware File | Pick the manufacturer's original file. Another open firmware project's build can't be used |
| This File Is Not for the (your radio) | It is for another model, or a modified build. Pick the manufacturer's file for your radio |
| This Version's Codec Is Not Supported Yet | Pick one of the supported files listed under it, or send the file so the version can be added |
| The File Is Damaged or Incomplete | Download it again |
| The Radio Programming Utility Could Not Use This File | Nothing is wrong with the file. The installer reports the fault, or shows how to report it; you can send the file to help |

A version that isn't supported yet may still give the bootloader key; the notice says so. You can also go on without a
usable file: the installer then keeps the codec already in the radio and says, before writing, that DMR voice depends on
it.

**Technical details** shows the exact message. Sending the file is offered only for the manufacturer's firmware file,
and only when you click. An automatic error report names the file (name, size and checksum), never its contents.

## If something goes wrong

The bootloader lives in a part of the flash the installer never erases, so you can always start over: put the radio in
its bootloader and install again. If the radio stays dark after flashing, that's what to do. The web installer's help
page walks through the update mode. See also [Troubleshooting](20-troubleshooting.md#installing).

**Report a Problem** sits under every failure and on the Verify step. It sends what the installer decided and its last
steps. You can add a message, an email address for a reply and the files it lists (the stock firmware file, a backup,
the firmware read from the radio). A backup holds your channels, contacts and the radio's calibration: uncheck it to
leave it out. Attached files and your email address are deleted within 90 days.

**Programming cables:** cables with a CH340 chip can be unreliable for backups and installs. If one fails, try a cable
with an FTDI, CP210x or PL2303 chip. The installer warns when it sees a CH340 cable.

**When the cable can't prepare the radio:** a radio installed over its programming cable first gets a small loader in
its memory. When the radio turns on, it waits only about 1/20 of a second for the installer's answer, and some cables
and USB hubs are slower than that. So the installer keeps sending its answer while the radio starts (this used to be the
separate **Slow Cable Mode**; it is now always on). Turn the radio off, select **Radio Is Off**, then turn it on.
A radio already running RadioFoundry skips that step.

If the load fails ("Preparing the radio failed"), the radio starts normally instead, or it never answers, the installer
offers **Use Factory Update Mode**: the radio's own update mode, entered by holding keys while you turn the radio
on (your radio's "Installing" section in the [Radio reference](23-radios.md)). Nothing can be read from the radio that
way, so no backup is taken and the voice codec comes from a file: the stock firmware file, or a program memory backup
of the radio where the installer knows no stock file for it. Troubleshooting offers the same route ("Reinstall in
Factory Update Mode"), and Restore uses it too.

## Going back to the stock firmware

1. Put the radio in its bootloader.
2. In the web installer choose **Restore**, load your backup and pick the original firmware, with your stock firmware
   file (any version for your radio). If the installer backed up the original firmware before the first installation
   (see your radio's section), load that backup; without one, load the manufacturer's firmware file. Or run the manufacturer's firmware
   upgrade tool and load the stock file.
3. Write your codeplug backup with the manufacturer's CPS. The stock firmware can't read the codeplug RadioFoundry used.

Your radio's section in the [Radio reference](23-radios.md) says which way applies.
