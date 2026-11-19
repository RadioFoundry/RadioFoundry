<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Installing

Install, update, back up, and restore RadioFoundry with the **web installer** on the project's web site,
[radiofoundry.org](https://radiofoundry.org/flash/index.html). It runs in a Chromium-based browser (Chrome, Edge) and
needs nothing installed. The files you load into it never leave your computer. Nothing in this chapter changes the
radio's calibration.

The command-line tool **rfoundry** does the same from a terminal. Use it for scripts, for a browser that can't talk to
a radio, or for a local build. For details, see [rfoundry on the command line](#advanced-rfoundry-on-the-command-line).

What differs from radio to radio (the original firmware file, the keys that start the bootloader, the install route)
is in your radio's "Installing" section of the [Radio reference](23-radios.md). To carry a codeplug over from the
manufacturer's firmware, see [Moving from the original firmware](18-moving-from-stock.md).

## What you need

| Item | Notes |
|---|---|
| A computer with Windows, macOS, or Linux, or an Android phone or tablet | On Android, use Chrome and a USB OTG adapter for the cable. For details, see the [FAQ](21-faq.md#can-i-use-a-phone) |
| A Chromium-based browser | For the web installer |
| Your radio's original firmware file | The manufacturer's file, in a version the installer recognizes. Your radio's "Installing" section names it and says where to find it |
| The radio's USB cable or programming cable | The cable that came with the radio |

The release firmware has everything except the AMBE codec. The installer either keeps the codec that's already in
your radio, or takes it from the original firmware file on your computer and writes it to the radio. The original
file also gives the installer the key that the radio's bootloader expects. Once the radio runs RadioFoundry, the
installer reads the key from the radio instead. The installer checks each file by its SHA-256 hash and refuses
unknown files.

Before you install, back up the radio's codeplug with the program you use now (the manufacturer's CPS, or qdmr).
RadioFoundry uses a different codeplug layout from the original firmware. If you go back to the original firmware
later, you need that backup. The web installer's own backup holds it too.

## Install with the web installer

To update a radio that already runs RadioFoundry, see [Updating RadioFoundry](#updating-radiofoundry).

1. Open the [web installer](https://radiofoundry.org/flash/index.html) and choose **Install**.
2. On **Radio**, pick your radio.
3. On **Build**, pick the features:

   - **Full** has every feature the radio supports. **Amateur** and **Commercial** are smaller presets. **Custom**
     lets you choose features one by one. The page marks features that the radio can't have.
   - **Radio language** sets the language the radio starts in. It starts as the page's language when the build has
     it. If you pick a language the build leaves out, the installer adds it to the build. You can change the language
     on the radio later, in **Settings > Display > Language**.
   - When your browser asks for a language that the build leaves out, the page offers to include it.
   - If the build is too big for the radio, the page says by how much. Turn some features off in **Custom**, or pick
     a smaller build, and build again.

   A custom build is made when you continue, and takes under a minute.
4. On **Current firmware**, say what the radio runs now:

   | Choice | When |
   |---|---|
   | **RadioFoundry** | The radio runs RadioFoundry. Connect the radio and turn it on; the installer reads the installed version |
   | **Original firmware** | The firmware installed at the factory |
   | **Another open firmware** | Another open firmware project, such as one of the OpenGD77 family |
   | **Other / Not sure** | Any other firmware, or you don't know what the radio runs |

   The installer asks for the original firmware file only when it needs the file for the codec or the key. If you
   choose **RadioFoundry** and the radio runs an OpenGD77-family firmware, the installer says so and offers
   **Another open firmware** instead. A RadioFoundry build without the debug link gets the same answer.

   On some radios, **Original firmware** and **Other / Not sure** offer extra steps before anything is written:

   - **Back Up Now** (recommended). Connect the radio, switched on normally, and pick **Everything** (several
     minutes) or **Channels and settings only** (under a minute). The installer reads the radio's factory data, the
     start of its program memory, and its memory chip, saves them as a `.zip` file on your computer, and restarts the
     radio. Nothing is written to the radio and nothing is sent anywhere. In update mode the radio gives less (part of
     the memory chip); the installer says so, and the installation can continue from there. Keep the file private: it
     holds your radio's calibration.
   - **Read Firmware Version**, on radios whose hardware revisions RadioFoundry can't always tell apart but whose
     original firmware version can. Connect the radio switched on normally, not in its bootloader. Reading changes
     nothing on the radio. After the installation, the installer stores the revision on the radio. If the radio
     already holds a different revision, the installer shows both and keeps yours (**Settings > Advanced >
     Hardware**). This step is optional. When the radio can't detect its revision, the installer asks for it after
     the installation, and the radio shows a reminder on its Home screen until you set it. The radio doesn't transmit
     until then.
5. On **Back up**, let the installer save a backup file (a `.zip` file) with the firmware, the channels and settings,
   and the calibration. Keep it. Where the original firmware can't be read first, the installer backs up the radio
   right after the installation, before anything else changes.

   On a radio installed over its programming cable, the cable is slow, so the installer shows how long each part
   takes:

   - The program memory is required, unless you use an original firmware file instead.
   - **All channels and settings** is optional and off by default. The installer never changes them, and reading the
     whole data memory can take many minutes.
   - If you have the radio's original firmware file, **Use a Stock File Instead** skips the firmware backup, before or
     during the read. The installer then reads only the codec area, and takes the codec from the file in the next
     step. The firmware in the radio isn't backed up, but the original file can restore it.

   Without a firmware backup or an original firmware file, the installer has no codec to give the radio. If you skip
   the firmware backup and select no supported file, **Install without the codec** is the only way to continue. The
   installer then warns, above the **Install** button, that this installation doesn't include the DMR voice codec:
   FM works, but DMR voice doesn't until you install RadioFoundry again with a supported manufacturer firmware file.
   Installing again keeps your channels and settings. When you press **Install**, a dialog asks once more: **Go Back
   and Add the Codec** returns to the step where you select the file, and **Install Anyway** installs without DMR
   voice.

   You get the same choice when the backup finds a firmware version whose codec RadioFoundry hasn't mapped. The backup
   is still complete; the codec step asks for a supported original firmware file instead.
6. On **Install**, put the radio in its bootloader when the installer asks. Your radio's keys are in the
   [Radio reference](23-radios.md). The installer writes the changed parts and restarts the radio. Don't unplug the
   radio until the installer says the installation is complete.
7. On **Verify**, the installer reads what the radio reports: the version it wrote, and "DMR voice is ready" when the
   codec is in place.
8. On **Channels**, the installer carries over the original firmware's codeplug, if the radio still holds one. It
   converts the codeplug to the RadioFoundry layout, writes it to the radio, reads it back, and shows how many
   channels, zones, and contacts came over.

   - This step runs by itself when the backup holds the original codeplug.
   - If you skipped the backup, the installer asks first, because on some radios the converted codeplug replaces the
     original bytes.
   - On radios that keep the original codeplug apart from RadioFoundry's, this step runs with or without a backup. It
     asks only before it replaces a RadioFoundry codeplug that's already on the radio.
   - An update of RadioFoundry skips this step.

   If the installer can't read, convert, or write the original codeplug, it says so, and the radio starts without
   channels. Convert the backup as described in [Backing up and restoring](#backing-up-and-restoring).

On Windows, the bootloader needs a driver once. For details, see
[Windows: the bootloader driver](#windows-the-bootloader-driver).

Then continue with [First boot](03-first-boot.md).

### Coming from another open firmware

On a radio that runs an OpenGD77-family firmware, the installer can back up the whole radio through that firmware
before it installs, and take the update key and the voice codec from the backup. You then need no original firmware
file. The installer offers this only on radios where it works. On other radios, it needs the original firmware file.

1. On **Current firmware**, with the radio connected and on, choose **Another open firmware**, then **Check the
   Radio**. Choosing **RadioFoundry** finds the other firmware too. The installer reports its version.
2. Choose **Back Up and Read**. The backup takes about a minute and a half. It only reads; nothing on the radio
   changes. Keep the file it saves.
3. Read what the installer found:

   - If it found the update key and the codec, continue. The codec step uses the codec from your radio.
   - If it found only one of them or neither, select your original firmware file on the codec step. The same applies
     when you skip the backup or the radio reports another model.

   The other firmware's installer might have put a codec from another radio's firmware on the radio, at an address
   of its choosing. The backup finds it. Your radio's section of the [Radio reference](23-radios.md) says which files
   count.
4. Install through the bootloader as usual.

If the radio stops responding during the backup, turn it off and on, then choose **Continue Reading**. The backup
continues where it stopped. **Save What Was Read** keeps what was read so far.

To go back, choose **Restore** with that backup and **Previous open firmware**. The installer writes back the
firmware, but not the channels and settings. If they're missing, write them with that firmware's own programming
software.

### Updating RadioFoundry

Start the radio normally, with RadioFoundry running, and follow these steps:

1. In the web installer, choose **Update**.
2. Connect the radio and press **Connect**. One port list shows the radio's USB port and any programming cables.
3. Check what the installer shows: the radio, the installed version, the new version, and the radio's build (Full,
   Amateur, Commercial, or Custom). The update installs the same build of the new release.

   - **Edit Build** opens the build step, so you can change features before the update.
   - A preset build lists the features that the new release adds to it.
   - A custom build lists the features added since it was made. Choose **On** or **Off** for each before you update.
     If one of your features now needs a new feature, the installer turns that feature on too and lists it.
   - If a feature of a custom build now needs something the radio doesn't have, the installer lists it with what it
     needs. Press **Disable These Features** to update without them, or **Edit Build** to change the build.
   - Firmware too old to report its build gets the build step, starting from the closest match.
   - The radio keeps the language it starts in. If an earlier update might have changed it by mistake, the installer
     asks whether to keep that language or start in English.
4. Optional: turn on **Save a backup first**. It's off by default, because an update keeps the channels, settings,
   calibration, and codec.
5. Press **Update**. The installer builds the image, writes it, and checks the version.

The radio goes straight from RadioFoundry into the loader: there are no keys to hold and no original firmware file to
give. The codeplug and the settings stay, because they're in flash that an update never touches. **Install** also works
on a radio that runs RadioFoundry, with every question asked.

### Backing up and restoring

**Back Up** saves a file with the radio's firmware, codec, channels, and settings. **Restore** takes that file and
writes back the whole radio, only the firmware, or only the channels and settings. Restore can also return the radio to
its original firmware from your original firmware file. Any version of the manufacturer's file for your radio works,
not only the one the installer needs for the codec. A restore from update mode works even when the firmware is
damaged.

**Back Up** asks what the radio runs now, as the install does:

- **RadioFoundry**: the installer reads the radio over the debug link.
- **Another open firmware**: the installer reads the radio through that firmware, on radios where it supports this.
- The original firmware: the radio isn't backed up here. Choose **Install** instead, which backs up the radio right
  after the installation.

Each radio lists every backup option and the time it takes: the program memory, the factory calibration, and all
channels and settings.

- **Calibration only** (or **Factory calibration**) reads only the factory calibration.
- On a radio that keeps a calibration copy in its program memory, that option is named **Program Memory &
  Calibration**.
- On a radio that keeps its channels and settings in NAND memory, **All channels and settings** reads the whole NAND,
  which can take several minutes.

Restore writes back only the program memory; it keeps a backup's calibration and data memory for the record. A backup
without the program memory can't restore the radio's original firmware. When you pick such a backup, Restore says so
and offers **Select a Firmware File**: pick a manufacturer firmware file for your radio, in a version the installer
supports. A dump of the radio's whole program memory also works, even when its firmware version isn't mapped yet.
Restore writes it back as it is and never touches the bootloader. An install can't use that dump's codec until the
version is mapped, so the installer offers to send the dump to the RadioFoundry team for mapping, and asks for a
supported manufacturer firmware file instead.

The backup is the `.zip` file itself, named after the radio's model with `_Backup_` and the date, not one of the files
inside it. A backup that you unpacked and zipped again still restores (macOS unpacks downloads by itself). When you
pick a file that isn't a backup, Restore says what it is and what to pick instead. An original firmware file goes on
to the restore of the original firmware, where the radio supports that without a backup.

If the backup holds the original firmware's codeplug, open the file in the
[Codeplug Editor](04-programming.md#the-codeplug-editor). It converts the channels, zones, and contacts to the
RadioFoundry layout.

## Windows: the bootloader driver

Windows has no driver for the USB bootloader of radios that flash over USB, so neither the web installer nor rfoundry
can open the bootloader until you install one. Do this once on each computer, with Zadig:

1. Download Zadig from [zadig.akeo.ie](https://zadig.akeo.ie) and run it. It needs no installation.
2. Put the radio in its bootloader and plug it in. Your radio's keys are in its section of the
   [Radio reference](23-radios.md).
3. In Zadig's list, pick **Digital Radio in USB mode**. The USB ID boxes show `0483` `DF11`. If it isn't listed,
   choose **Options > List All Devices**.
4. To the right of the green arrow, choose **WinUSB** as the driver, and click **Install Driver** (or **Replace
   Driver**). Wait until Zadig says it succeeded.
5. Unplug the radio, put it back in its bootloader, and connect it again.

Only the bootloader needs this driver. The radio's serial port, once it runs RadioFoundry, works without one.

The manufacturer's upgrade tool and CPS use the same USB ID and need their own driver back. To restore it, in Device
Manager, uninstall the device with **Delete the driver software for this device** selected, then plug the radio in
again.

## Install from the command line with a kit

When the web installer can't install, it offers a **manual install kit**: a `.zip` file with everything you need to
install the firmware you chose, from a terminal on this computer, with rfoundry. Choose and build the firmware as
usual; the kit takes it from there.

| The installer offers the kit when | Where |
|---|---|
| The browser has no WebUSB or Web Serial (Safari, Firefox) | The start page says so. The step that needs the radio shows the kit instead, with a picker for the original firmware file |
| Connecting to the bootloader, or the write, fails | A line under the error |
| The radio doesn't answer the cable within three minutes, or preparing it fails | A line under the error |

| In the kit | What it is |
|---|---|
| `README.txt` | Setup on Windows, macOS, and Linux (Python, the bootloader driver, the serial port), how to put the radio in update mode, and the commands to check (`--dry-run`) and install, with the file names filled in |
| `firmware/` | The image and its manifest |
| `stock/` | Your original firmware file, if you gave one. Otherwise the README says which file to put there |
| `rfoundry.py`, `tools/`, `stub/`, `requirements.txt` | rfoundry, from the same version as the web installer. It runs from the folder; `pip` installs its packages in a virtual environment |

The kit needs Python 3.8 or later. Its README says how to install Python and rfoundry's packages on each system, and
which driver a USB bootloader needs on Windows and Linux. The browser puts the kit together; nothing is uploaded. A
radio that rfoundry can't install gets no kit: use Chrome or Edge on a computer. Your radio's section of the
[Radio reference](23-radios.md) says how it installs.

## Advanced: rfoundry on the command line

Each release carries **rfoundry**, one file for each system, named `rfoundry-<version>-<system>`. A typical install
and a later update look like this:

```sh
rfoundry flash --release latest --stock STOCK.bin   # first install, with your original firmware file
rfoundry flash --release latest                     # update a radio that runs RadioFoundry
```

rfoundry downloads the release, checks it, decides whether to keep or write the codec, and prints a plan before it
writes. `--dry-run` shows the plan without touching the radio. Some radios come in hardware versions that one image
can't cover, such as a UHF and a VHF board. rfoundry picks the image from what a radio that runs RadioFoundry reports.
Otherwise it asks, or takes `--option BAND=vhf`. For the one-time setup on each system, the options,
and the codec choices, see [rfoundry](../../tools/rfoundry/README.md). To build and flash your own image, see
[Building](../dev/building.md).

## If something goes wrong

The bootloader lives in a part of the flash that the installer never erases, so you can always start over: put the
radio in its bootloader and install again. If the screen stays dark after flashing, do that. For every installer
error and its fix, see [Troubleshooting](20-troubleshooting.md#installing). To send the developers what went wrong,
use **Report a Problem**, described in [Reporting a problem](20-troubleshooting.md#reporting-a-problem).

## Going back to the original firmware

Your radio's section of the [Radio reference](23-radios.md) says which of these ways applies.

1. Put the radio in its bootloader.
2. Write the original firmware in one of these ways:

   - In the web installer, choose **Restore**. Load the backup the installer made before the first installation, and
     pick the original firmware. Without that backup, load the manufacturer's firmware file (any version for your
     radio).
   - Run the manufacturer's firmware upgrade tool and load the original firmware file.
3. Write your codeplug backup with the manufacturer's CPS. The original firmware can't read the codeplug that
   RadioFoundry used.

On a radio installed over its programming cable, a restore writes only the parts of the radio's memory that differ
from the backup. When it finishes, it says how many parts it wrote and how many already matched. When the radio
still holds the codec as the backup has it, the installer says that it didn't write the codec space.
