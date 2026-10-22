# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
"""Host tests for odrflash's serial route (odrflash/serialflash.py): the DM-1801 Pro over its programming cable, through
the stub (stub/stubtool.py) or Baofeng's update mode (tools/serial_loader.py), and its release package (tools/release.py).
No serial port: the tools' own simulated radios stand in for the cable.

    python3 -m unittest discover -s tools/odrflash/tests -v        (tools/ci.sh runs this)
"""
import contextlib
import functools
import hashlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from odrflash import boards, cli, manifest as mf, serialflash as sf  # noqa: E402

B = boards.get("dm1801pro")
SLOT_OFF = 0x030E8000 - 0x0300C000		# the codec slot, from the application base
FLASH_OFF = 0xC000				# the application base in the program flash (stubtool's APP_START)
GIT = "e" * 40


def app_image(n, seed=1):
    rng = random.Random(seed)
    return struct.pack("<II", 0x0300C0C1, 0x12345678) + bytes(rng.getrandbits(8) for _ in range(n - 8))


def slot_image(n=0x2000, seed=9):
    rng = random.Random(seed)
    return struct.pack("<I", sf.SLOT_MAGIC) + bytes(rng.getrandbits(8) for _ in range(n - 4))


def run_cli(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = cli.main(argv)
    return rc, out.getvalue(), err.getvalue()


def odr(codec="dm1801pro-049", git=GIT[:8]):
    return {"port": "/dev/fake", "board": "DM-1801 Pro", "git": git, "codec": codec, "features": "", "uid": None}


class Tools(unittest.TestCase):
    def test_the_wrapped_tools_load_from_the_checkout(self):
        self.assertTrue(hasattr(sf.tool("serial_loader"), "Flasher"))
        self.assertTrue(hasattr(sf.tool("stubtool"), "Radio"))
        self.assertTrue(hasattr(sf.tool("hrc7000_codec"), "merge"))
        self.assertIn("dm1801pro", sf.tool("serial_loader").MODELS)

    def test_slot_magic_matches_the_tools(self):
        self.assertEqual(sf.SLOT_MAGIC, sf.tool("hrc7000_codec").SLOT_MAGIC)
        self.assertEqual(sf.SLOT_MAGIC, sf.tool("serial_loader").SLOT_MAGIC)
        self.assertEqual(B.codec.windows[0][0] - B.app_base, sf.tool("stubtool").APP_END - FLASH_OFF - 0x18000)

    def test_missing_tool_is_explained(self):
        with mock.patch.object(sf, "_ROOT", "/nonexistent"), mock.patch.dict(sf._loaded, {}, clear=True):
            with self.assertRaises(sf.SerialError) as e:
                sf.tool("stubtool")
        self.assertIn("stub/stubtool.py", str(e.exception))


class Images(unittest.TestCase):
    def test_split_plain_and_merged(self):
        app = app_image(5000)
        self.assertEqual(sf.split_slot(B, app), (app, None))
        whole = sf.contiguous(B, app, slot_image())
        self.assertEqual(len(whole), SLOT_OFF + 0x2000)
        self.assertEqual(sf.split_slot(B, whole), (app, slot_image()))
        self.assertEqual(sf.contiguous(B, app), app)

    def test_contiguous_refuses_overlaps(self):
        with self.assertRaises(sf.SerialError):
            sf.contiguous(B, b"\x01" * (SLOT_OFF + 1))
        with self.assertRaises(sf.SerialError):
            sf.contiguous(B, app_image(100), b"\x01" * (0x18000 + 1))

    def test_slot_from_a_program_flash_read(self):
        """hrc7000_codec.py builds the slot from a (here synthetic) 1 MB program flash read, checked by SHA-256."""
        dc = sf.tool("hrc7000_codec")
        real = [p for p in dc.PROFILES.values() if p["board"] == "dm1801pro"][0]
        rng = random.Random(3)
        code = struct.pack("<2I", *dc.CODEC_SIG) + bytes(rng.getrandbits(8) for _ in range(real["code"][1] - 8))
        tables = bytes(rng.getrandbits(8) for _ in range(real["tables"][1]))
        dump = bytearray(b"\xFF" * 0x100000)
        c0 = real["code"][0] - dc.PROGMEM_BASE
        dump[c0:c0 + len(code)] = code
        t0 = real["tables"][0] - dc.PROGMEM_BASE
        dump[t0:t0 + len(tables)] = tables
        prof = dict(real, code_sha256=hashlib.sha256(code).hexdigest(), tables_sha256=hashlib.sha256(tables).hexdigest(),
                    tables_head_crc=zlib.crc32(tables[:64]))
        app = app_image(4000)
        # the synthetic codec's map: one word that holds an address in the code
        struct.pack_into("<I", dump, c0 + 0x40, real["code"][2] + 0x100)
        code = bytes(dump[c0:c0 + len(code)])
        prof["code_sha256"] = hashlib.sha256(code).hexdigest()
        maps = dc.synthetic_map(prof, code, refs=((0x40, real["code"][2] + 0x100),))
        with mock.patch.dict(dc.PROFILES, {hashlib.sha256(code).hexdigest(): prof}, clear=True), \
                mock.patch.object(dc.codecmap, "load_maps", lambda *a: maps):
            slot, name = sf.slot_from_file(B, app, bytes(dump))
            with self.assertRaises(sf.SerialError) as e:
                sf.slot_from_file(B, app, bytes(dump[:-1]))
        self.assertIn("program flash read", str(e.exception))
        self.assertEqual(struct.unpack_from("<I", slot)[0], sf.SLOT_MAGIC)
        self.assertIn(code, slot)
        self.assertIn(tables, slot)
        self.assertEqual(name, real["name"])


class Decisions(unittest.TestCase):
    def test_route(self):
        self.assertEqual(sf.choose_route("auto", odr(), True)[0], sf.STUB)
        self.assertEqual(sf.choose_route("auto", odr(), False)[0], sf.BOOTLOADER)
        self.assertIn("CONFIG_STUB_ENTRY", sf.choose_route("auto", odr(), False)[1])
        self.assertEqual(sf.choose_route("auto", None, False)[0], sf.BOOTLOADER)
        self.assertEqual(sf.choose_route("bootloader", odr(), True)[0], sf.BOOTLOADER)
        self.assertEqual(sf.choose_route("stub", odr(), True)[0], sf.STUB)
        for p in (odr(), None):
            with self.assertRaises(sf.SerialError):
                sf.choose_route("stub", p, False)

    def test_codec(self):
        d = sf.decide_codec
        self.assertEqual(d(B, sf.STUB, "auto", False, False, odr())[:2][0], "keep")
        self.assertIsNone(d(B, sf.STUB, "auto", False, False, odr())[2])			# codec checked: no risk
        mode, _, risk, expect, _ = d(B, sf.STUB, "auto", False, False, odr("absent"))
        self.assertEqual((mode, expect), ("keep", False))
        self.assertIn("codec=absent", risk)
        self.assertEqual(d(B, sf.STUB, "auto", True, False, None)[::4], ("merge", "image"))
        self.assertEqual(d(B, sf.BOOTLOADER, "auto", True, False, None)[::4], ("merge", "image"))
        self.assertEqual(d(B, sf.BOOTLOADER, "auto", False, True, None)[::4], ("merge", "file"))
        self.assertEqual(d(B, sf.STUB, "keep", True, True, odr())[0], "keep")		# keep wins on the stub route
        with self.assertRaises(sf.SerialError) as e:
            d(B, sf.BOOTLOADER, "auto", False, False, odr())				# update mode erases the slot
        self.assertIn("erases the whole application area", str(e.exception))
        with self.assertRaises(sf.SerialError):
            d(B, sf.BOOTLOADER, "keep", True, False, None)
        with self.assertRaises(sf.SerialError):
            d(B, sf.STUB, "merge", False, False, odr())

    def test_describe(self):
        text = "\n".join(sf.describe(B, sf.STUB, app_image(9000), "keep"))
        self.assertIn("not written: the codec slot 0x030E8000..0x030FFFFF", text)
        self.assertIn("never erased or written: the bootloader 0x03000000..0x0300BFFF", text)
        text = "\n".join(sf.describe(B, sf.BOOTLOADER, app_image(9000), "merge"))
        self.assertIn("erases the whole application area 0x0300C000..0x030FFFFF", text)
        self.assertIn("9 blocks of 1 KB", text)


class StubRoute(unittest.TestCase):
    """flash_stub drives stubtool's real Radio against its simulated radio (FakeRadio). The ODR entry over the debug
    link is stubtool's own self-test; here the stub comes up through the simulated boot ROM instead."""

    def radio(self):
        st = sf.tool("stubtool")
        prog = bytearray(b"\xFF" * 0x100000)
        prog[:0x1000] = b"\xB0" * 0x1000					# the bootloader
        prog[FLASH_OFF:FLASH_OFF + 0x3000] = b"\x11" * 0x3000			# an older application
        old_slot = slot_image(0x4000, seed=5)
        prog[FLASH_OFF + SLOT_OFF:FLASH_OFF + SLOT_OFF + len(old_slot)] = old_slot
        stub = bytes(range(256)) * 7
        fake = st.FakeRadio({"progmem": bytes(prog), "q128": b"\xFF" * 0x1000000}, stub_len=len(stub))
        enter = lambda radio, image=None: radio.start(wait=2, stub=stub)	# noqa: E731
        return st, fake, old_slot, mock.patch.object(st.Radio, "odr_enter", enter)

    def test_keep_leaves_the_slot_and_the_bootloader(self):
        st, fake, old_slot, patch = self.radio()
        app = app_image(3 * 4096 + 100)
        with patch:
            written, same = sf.flash_stub("/dev/fake", sf.contiguous(B, app), log=lambda m: None,
                                          opener=lambda dev: fake)
        prog = fake.images["progmem"]
        self.assertEqual(prog[FLASH_OFF:FLASH_OFF + len(app)], app)
        self.assertEqual(prog[FLASH_OFF + SLOT_OFF:][:len(old_slot)], old_slot)
        self.assertEqual(prog[:0x1000], b"\xB0" * 0x1000)
        self.assertEqual((written, same), (4, 0))

    def test_merge_writes_the_slot(self):
        st, fake, old_slot, patch = self.radio()
        app, slot = app_image(5000), slot_image(0x3000, seed=6)
        with patch:
            sf.flash_stub("/dev/fake", sf.contiguous(B, app, slot), log=lambda m: None, opener=lambda dev: fake)
        prog = fake.images["progmem"]
        self.assertEqual(prog[FLASH_OFF + SLOT_OFF:][:len(slot)], slot)
        self.assertEqual(prog[FLASH_OFF:FLASH_OFF + len(app)], app)

    def test_a_failed_entry_is_a_serial_error(self):
        st = sf.tool("stubtool")

        def refuse(radio, image=None):
            raise st.DumpError("no answer from ODR's debug link")
        with mock.patch.object(st.Radio, "odr_enter", refuse):
            with self.assertRaises(sf.SerialError):
                sf.flash_stub("/dev/fake", app_image(100), log=lambda m: None, opener=lambda dev: mock.Mock())

    def test_stub_offered_reads_the_stub_info(self):
        st = sf.tool("stubtool")
        good = bytes([0]) + st.STUB_MAGIC.to_bytes(4, "little") + bytes(14)
        with mock.patch.object(st.Radio, "dl_request", lambda self, *a, **k: good):
            self.assertTrue(sf.stub_offered("/dev/fake", opener=lambda dev: mock.Mock()))
        with mock.patch.object(st.Radio, "dl_request", lambda self, *a, **k: bytes([3]) + bytes(18)):
            self.assertFalse(sf.stub_offered("/dev/fake", opener=lambda dev: mock.Mock()))

        def silent(self, *a, **k):
            raise st.DumpError("no answer")
        with mock.patch.object(st.Radio, "dl_request", silent):
            self.assertFalse(sf.stub_offered("/dev/fake", opener=lambda dev: mock.Mock()))


class BootloaderRoute(unittest.TestCase):
    """flash_bootloader drives serial_loader's real Flasher against its simulated loader (FakeBootloader)."""

    def setUp(self):
        self.df = sf.tool("serial_loader")
        self.fast = mock.patch.object(self.df, "Flasher", functools.partial(self.df.Flasher, step_timeout=0.05))

    def test_writes_the_image_through_the_dm1801pro_id(self):
        fake = self.df.FakeBootloader(model=self.df.MODELS["dm1801pro"])
        raw = sf.contiguous(B, app_image(5000), slot_image())
        with self.fast:
            sf.flash_bootloader("/dev/fake", raw, B, log=lambda m: None, opener=lambda dev: fake, name="odr.bin")
        self.assertEqual(fake.image(), raw)
        self.assertFalse(fake.errors)
        self.assertTrue(fake.done)

    def test_waits_for_update_mode(self):
        df = self.df

        class WakesUp(df.FakeBootloader):
            def __init__(self, quiet_writes):
                super().__init__(silent=True, model=df.MODELS["dm1801pro"])
                self.quiet = quiet_writes

            def write(self, b):
                if self.silent:
                    self.quiet -= 1
                    self.silent = self.quiet > 0
                    return
                super().write(b)
        fake = WakesUp(4)	# the first handshake (three tries) goes unanswered: the radio is switched on later
        raw = app_image(3000)
        with self.fast:
            sf.flash_bootloader("/dev/fake", raw, B, log=lambda m: None, opener=lambda dev: fake, wait_s=5)
        self.assertEqual(fake.image(), raw)

    def test_gives_up_and_names_the_step(self):
        fake = self.df.FakeBootloader(silent=True, model=self.df.MODELS["dm1801pro"])
        with self.fast, self.assertRaises(sf.SerialError) as e:
            sf.flash_bootloader("/dev/fake", app_image(3000), B, log=lambda m: None, opener=lambda dev: fake,
                                wait_s=0.2)
        self.assertIn("step 1", str(e.exception))

    def test_a_dm32_loader_stops_a_dm1801pro_flash(self):
        fake = self.df.FakeBootloader(model=self.df.MODELS["dm32"])	# another radio's loader ID
        with self.fast, self.assertRaises(sf.SerialError) as e:
            sf.flash_bootloader("/dev/fake", app_image(3000), B, log=lambda m: None, opener=lambda dev: fake)
        self.assertIn("step 3", str(e.exception))


class Command(unittest.TestCase):
    """odrflash flash for a DM-1801 Pro, with the cable side replaced by recorders."""

    def run_flash(self, argv, probe=None, offered=False, cable="/dev/fake"):
        calls = []

        def stub(dev, raw, log=None, progress=None, opener=None):
            calls.append(("stub", dev, raw))
            return 3, 1

        def boot(dev, raw, board, log=None, progress=None, wait_s=0, opener=None, name=""):
            calls.append(("bootloader", dev, raw))

        def find(port=None):
            if cable is None:
                raise sf.SerialError("no USB-serial programming cable found")
            return port or cable
        with mock.patch.object(sf, "find_cable", find), mock.patch.object(sf, "probe", lambda dev, log=None: probe), \
                mock.patch.object(sf, "stub_offered", lambda dev, opener=None: offered), \
                mock.patch.object(sf, "flash_stub", stub), mock.patch.object(sf, "flash_bootloader", boot), \
                mock.patch.object(cli.state, "forget_probe", lambda: None):
            rc, out, err = run_cli(["flash"] + argv + ["--no-reporting", "--no-verify"])
        return rc, out, err, calls

    def local(self, d, data, name="odr_dm1801pro.bin"):
        p = os.path.join(d, name)
        open(p, "wb").write(data)
        return p

    def test_stub_chosen_when_odr_offers_it(self):
        app = app_image(9000)
        with tempfile.TemporaryDirectory() as d:
            rc, out, err, calls = self.run_flash([self.local(d, app), "--board", "dm1801pro", "--yes"], odr(), True)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(calls, [("stub", "/dev/fake", app)])
        self.assertIn("route    STUB", out)
        self.assertIn("codec    KEEP", out)

    def test_bootloader_when_odr_lacks_stub_entry(self):
        app = app_image(9000)
        with tempfile.TemporaryDirectory() as d:
            merged = sf.contiguous(B, app, slot_image())
            rc, out, err, calls = self.run_flash([self.local(d, merged, "odr_dm1801pro_codec.bin"), "--board",
                                                  "dm1801pro", "--yes"], odr(), False)
        self.assertEqual(rc, 0, out + err)
        self.assertEqual(calls, [("bootloader", "/dev/fake", merged)])
        self.assertIn("CONFIG_STUB_ENTRY", out)
        self.assertIn("codec    MERGE", out)
        self.assertIn("update mode", out)

    def test_bootloader_refuses_an_image_without_the_codec(self):
        with tempfile.TemporaryDirectory() as d:
            rc, out, err, calls = self.run_flash([self.local(d, app_image(9000)), "--board", "dm1801pro", "--yes"])
        self.assertEqual(rc, 2)
        self.assertEqual(calls, [])
        self.assertIn("--stock", err)

    def test_route_forced(self):
        app = app_image(9000)
        with tempfile.TemporaryDirectory() as d:
            p = self.local(d, sf.contiguous(B, app, slot_image()))
            rc, out, err, calls = self.run_flash([p, "--board", "dm1801pro", "--yes", "--route", "bootloader"],
                                                 odr(), True)
            self.assertEqual((rc, calls[0][0]), (0, "bootloader"), out + err)
            rc, out, err, calls = self.run_flash([p, "--board", "dm1801pro", "--yes", "--route", "stub"])
            self.assertEqual((rc, calls), (2, []))
            self.assertIn("stub can't be entered", err)

    def test_dry_run_without_a_cable(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.local(d, sf.contiguous(B, app_image(9000), slot_image()))
            rc, out, err, calls = self.run_flash([p, "--board", "dm1801pro", "--dry-run"], cable=None)
        self.assertEqual((rc, calls), (0, []), out + err)
        self.assertIn("dry run: nothing was erased or written", out)
        self.assertIn("erases the whole application area", out)

    def test_wrong_radio_refused(self):
        other = dict(odr(), board="DM-32UV")
        with tempfile.TemporaryDirectory() as d:
            rc, out, err, calls = self.run_flash([self.local(d, app_image(9000)), "--board", "dm1801pro", "--yes"],
                                                 other, True)
        self.assertEqual((rc, calls), (2, []))
        self.assertIn("DM-32UV", err)

    def test_dm1701_takes_no_route(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.local(d, struct.pack("<II", 0x2001FFFC, 0x0800C1C1) + bytes(5000), "odr_dm1701.bin")
            rc, out, err, calls = self.run_flash([p, "--board", "dm1701", "--route", "stub", "--dry-run",
                                                  "--codec", "keep", "--no-probe"])
        self.assertEqual((rc, calls), (2, []))
        self.assertIn("--route is for", err)

    def test_release_package_flashes_over_the_stub(self):
        """tools/release.py --board dm1801pro, then odrflash flash on its output: the manifest is checked, the image
        is written as packaged and the slot stays."""
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        app = app_image(20000, seed=2)
        with tempfile.TemporaryDirectory() as d:
            bp = self.local(d, app)
            out = os.path.join(d, "dist")
            with contextlib.redirect_stdout(io.StringIO()):
                release.main(["--board", "dm1801pro", "--version", "v9.9.9", "--bin", bp, "--out", out, "--git", GIT,
                              "--features", "FM,DMR", "--toolchain", "csky-elf-gcc test"])
            img = os.path.join(out, "odr-dm1801pro-v9.9.9.bin")
            m = mf.load(os.path.join(out, "odr-dm1801pro-v9.9.9.json"))
            self.assertIs(mf.verify(m, open(img, "rb").read()), B)
            self.assertEqual((m["board"], m["sectors"], m["codec"]["included"], m["codec"]["profile"]),
                             ("dm1801pro", [0], False, "dm1801pro-049"))
            sums = open(os.path.join(out, "SHA256SUMS")).read()
            self.assertIn(hashlib.sha256(app).hexdigest() + "  odr-dm1801pro-v9.9.9.bin", sums)
            rc, so, se, calls = self.run_flash([img, "--yes"], odr(), True)
            self.assertEqual(rc, 0, so + se)
            self.assertEqual(calls, [("stub", "/dev/fake", app)])
            self.assertIn("SHA-256 verified", so)

    def test_release_refuses_a_merged_image(self):
        sys.path.insert(0, os.path.join(HERE, "..", ".."))
        import release
        with tempfile.TemporaryDirectory() as d:
            bp = self.local(d, sf.contiguous(B, app_image(20000), slot_image()), "odr_dm1801pro_codec.bin")
            with self.assertRaises(SystemExit) as e:
                release.main(["--board", "dm1801pro", "--version", "v9", "--bin", bp, "--out", os.path.join(d, "o"),
                              "--git", GIT, "--features", "", "--toolchain", ""])
        self.assertIn("codec slot", str(e.exception))


class Verify(unittest.TestCase):
    def check(self, answer, expect_codec=True, git=GIT[:8]):
        man = {"git": git}
        with contextlib.redirect_stdout(io.StringIO()) as out, contextlib.redirect_stderr(io.StringIO()):
            rc = cli.verify_serial("/dev/fake", B, man, expect_codec, wait_s=0, probe_fn=lambda dev: answer,
                                   sleep=lambda s: None)
        return rc, out.getvalue()

    def test_after_the_restart(self):
        self.assertEqual(self.check(odr())[0], 0)
        self.assertIn("DMR voice is ready", self.check(odr())[1])
        self.assertEqual(self.check(odr(git="00000000"))[0], 1)		# another build answers
        self.assertEqual(self.check(odr("absent"))[0], 1)
        self.assertEqual(self.check(odr("absent"), expect_codec=False)[0], 0)
        self.assertEqual(self.check(None)[0], 0)				# no answer: told what to look for


if __name__ == "__main__":
    unittest.main()
