"""Tests of the release packages' game-content check (check_package.py) and of the dump setup the launcher runs
(recomp/games/sm35/setup.py, versions.py). Synthetic files only: no game data."""
import io
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'recomp' / 'games' / 'sm35'))
import check_package  # noqa: E402
import setup  # noqa: E402
import versions  # noqa: E402


def fake_nso(build_id_hex: str) -> bytes:
    head = bytearray(0x100)
    head[:4] = b'NSO' + b'0'
    bid = bytes.fromhex(build_id_hex)
    head[0x40:0x40 + len(bid)] = bid
    return bytes(head)


class PackageCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.pkg = Path(self.tmp.name) / 'SM35Recomp-test'
        (self.pkg / 'tools').mkdir(parents=True)
        (self.pkg / 'tools' / 'gen.py').write_text('print("hello")\n')
        (self.pkg / 'README.txt').write_text('No game files are included.\n')

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, path):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = check_package.check(path)
        return rc, out.getvalue()

    def test_clean_package_passes(self):
        rc, out = self.run_check(self.pkg)
        self.assertEqual(rc, 0, out)

    def test_dump_files_fail(self):
        cases = {
            'extracted/exefs/x': b'data',                         # a set-up data folder
            'renamed.bin': fake_nso(versions.BUILD_IDS['1.0.2']['main']),  # an executable of the game
            'fonts/nintendo_udsg-r_std_003.bfttf': b'x',          # a console font
            'sm35game.dylib': b'\xcf\xfa\xed\xfe',                # the game code built from a dump
            'lib/libsm35game.so': b'\x7fELF',                       # (Android's name)
            'sm35.ini': b'[settings]\n',                           # a data folder's settings
            'gen/main_000.c': b'int x;\n',                        # recompiled chunks
            'k.txt': ('header_key = ' + '0' * 64 + '\n').encode(),  # key-shaped lines
            'data.dat': b'SA' + b'RC' + b'\0' * 16,               # a game archive by its magic
        }
        for rel, data in cases.items():
            with self.subTest(rel=rel):
                p = self.pkg / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(data)
                rc, out = self.run_check(self.pkg)
                self.assertEqual(rc, 1, out)
                self.assertIn(rel, out)
                p.unlink()

    def test_known_exefs_hash_fails(self):
        # a file whose SHA-256 is a known v1.0.2 ExeFS file's: simulated by putting our file's hash in the table
        p = self.pkg / 'blob.bin'
        p.write_bytes(b'not really the game')
        import hashlib
        h = hashlib.sha256(p.read_bytes()).hexdigest()
        check_package.KNOWN_SHA256[h] = 'a test file'
        try:
            rc, out = self.run_check(self.pkg)
        finally:
            del check_package.KNOWN_SHA256[h]
        self.assertEqual(rc, 1, out)

    def test_zip_is_checked(self):
        z = Path(self.tmp.name) / 'pkg.zip'
        with zipfile.ZipFile(z, 'w') as f:
            f.writestr('SM35Recomp/README.txt', 'ok')
            f.writestr('SM35Recomp/extracted/romfs/a', 'x')
        rc, out = self.run_check(z)
        self.assertEqual(rc, 1, out)

    def test_macos_zip_bundled_tools(self):
        # the .app one level deeper than the other packages' folders: zig's test certificates are still its own,
        # a certificate elsewhere in the app is not
        z = Path(self.tmp.name) / 'mac.zip'
        res = 'SM35Recomp.app/Contents/Resources'
        with zipfile.ZipFile(z, 'w') as f:
            f.writestr(f'{res}/zig/lib/std/crypto/codecs/asn1/der/testdata/all_types.der', b'0\x00')
            f.writestr(f'{res}/python/lib/python3.12/test/certdata/keycert.pem', 'x')
        rc, out = self.run_check(z)
        self.assertEqual(rc, 0, out)
        with zipfile.ZipFile(z, 'a') as f:
            f.writestr(f'{res}/extra/server.der', b'0\x00')
        rc, out = self.run_check(z)
        self.assertEqual(rc, 1, out)
        self.assertIn('extra/server.der', out)


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def exefs(self, name, main_id):
        d = self.dir / name
        d.mkdir()
        (d / 'main').write_bytes(fake_nso(main_id))
        (d / 'sdk').write_bytes(fake_nso(versions.BUILD_IDS['1.0.2']['sdk']))
        (d / 'subsdk0').write_bytes(fake_nso(versions.BUILD_IDS['1.0.2']['subsdk0']))
        (d / 'subsdk1').write_bytes(fake_nso(versions.BUILD_IDS['1.0.2']['subsdk1']))
        (d / 'rtld').write_bytes(b'x')
        (d / 'main.npdm').write_bytes(b'x')
        return d

    def test_versions(self):
        self.assertEqual(versions.version_of_build_id('main', versions.BUILD_IDS['1.0.2']['main'] + '00'), '1.0.2')
        self.assertEqual(versions.version_of_build_id('main', versions.BUILD_IDS['1.0.0']['main']), '1.0.0')
        self.assertIsNone(versions.version_of_build_id('main', '00' * 16))
        self.assertEqual(versions.display_version(versions.UPDATE_TITLE_ID, versions.UPDATE_VERSION), '1.0.2')

    def test_base_game_only_says_which_version(self):
        self.exefs('exefs', versions.BUILD_IDS['1.0.0']['main'])
        with self.assertRaises(setup.SetupError) as e, redirect_stdout(io.StringIO()):
            setup.setup_from_sources([str(self.dir)], str(self.dir / 'out'), None, None)
        self.assertEqual(e.exception.code, 'WRONG_VERSION')
        self.assertIn('1.0.0', e.exception.what)

    def test_modified_exefs_is_refused(self):
        # the right build ids, but not the known files (hashes differ)
        d = self.exefs('exefs', versions.BUILD_IDS['1.0.2']['main'])
        with self.assertRaises(setup.SetupError) as e:
            setup.check_exefs(str(d))
        self.assertEqual(e.exception.code, 'BAD_HASH')

    def test_nothing_found(self):
        (self.dir / 'empty').mkdir()
        with self.assertRaises(setup.SetupError) as e, redirect_stdout(io.StringIO()):
            setup.setup_from_sources([str(self.dir / 'empty')], str(self.dir / 'out'), None, None)
        self.assertEqual(e.exception.code, 'NO_EXEFS')

    def test_compressed_dump_is_named(self):
        p = self.dir / 'game.nsz'
        p.write_bytes(b'x')
        with self.assertRaises(setup.SetupError) as e:
            setup.scan_sources([str(p)])
        self.assertEqual(e.exception.code, 'COMPRESSED')

    def test_pfs0_and_cnmt_parsing(self):
        names = [b'a.nca', b'b.cnmt.nca']
        strings = b'\0'.join(names) + b'\0'
        data = [b'A' * 5, b'B' * 3]
        table, off = b'', 0
        for i, d in enumerate(data):
            table += struct.pack('<QQII', off, len(d), strings.index(names[i]), 0)
            off += len(d)
        p = self.dir / 'x.nsp'
        p.write_bytes(b'PFS0' + struct.pack('<II', len(names), len(strings)) + b'\0' * 4 + table + strings + b''.join(data))
        with open(p, 'rb') as f:
            e = setup.pfs0_entries(f)
        self.assertEqual([(n, s) for n, _, s in e], [('a.nca', 5), ('b.cnmt.nca', 3)])
        with open(p, 'rb') as f:
            f.seek(e[1][1])
            self.assertEqual(f.read(3), b'BBB')
        cnmt = bytearray(0x20 + 0x10 + 0x38)
        struct.pack_into('<QIB', cnmt, 0, versions.UPDATE_TITLE_ID, versions.UPDATE_VERSION, 0x81)
        struct.pack_into('<HH', cnmt, 0xE, 0x10, 1)
        rec = 0x30
        cnmt[rec + 0x20:rec + 0x30] = bytes(range(16))
        cnmt[rec + 0x30:rec + 0x36] = (1234).to_bytes(6, 'little')
        cnmt[rec + 0x36] = 1
        m = setup.parse_cnmt(bytes(cnmt))
        self.assertEqual((m['title_id'], m['version'], m['type']), (versions.UPDATE_TITLE_ID, versions.UPDATE_VERSION, 0x81))
        self.assertEqual(m['contents'], [(bytes(range(16)).hex(), 1234, 1)])

    def test_tool_output_is_masked(self):
        line = 'Titlekey (Decrypted) ' + 'ab' * 16
        self.assertEqual(setup.masked(line + '\nfine\n'), 'fine')
        self.assertEqual(setup.masked('value ' + 'cd' * 16), 'value <hex>')


if __name__ == '__main__':
    unittest.main()
