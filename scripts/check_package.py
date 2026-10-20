#!/usr/bin/env python3
"""Fail if a player package holds anything from a game dump.

A release ships the launcher, the runtime and the recompiler, never the game: no Nintendo code (the dump's ExeFS, the
recompiled C or the game code library built from it), no game data (RomFS files, the console's fonts and system
data), no keys, no translated shaders. tools/package.sh and tools/bundle_macos.sh run this on what they made, and the
release workflow runs it again on the files it uploads.

    check_package.py <folder | .zip | .tar.gz | .app> ...

What it looks for, per file:
  - names and folders a set-up data folder has (extracted/, exefs/, romfs/, fonts/*.bfttf, sysdata/, <id>game.*,
    prod.keys, setup.json, launcher.ini, <id>.ini, generated chunks main_000.c / sdk_table.c)
  - the magic bytes of Switch / Wii U containers, executables and assets (scripts/check_no_proprietary.py's list,
    without the host executable formats a package is made of)
  - the SHA-256 of the v1.0.1 ExeFS files (recomp/games/sm35/versions.py: hashes of the player's own files)
  - lines shaped like console keys; text that looks like recompiled game code
  - a runtime executable with recompiled game functions linked in
"""
from __future__ import annotations

import hashlib
import io
import os
import re
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
GAME_DIR = ROOT / 'recomp' / 'games' / 'sm35'
sys.path.insert(0, str(GAME_DIR))
import check_no_proprietary as cnp  # noqa: E402
import versions  # noqa: E402

# the game's short name (game.mk GAME_ID): its executable <id>recomp, its code library <id>game.*, its <id>.ini
GAME_ID = re.search(r'^GAME_ID\s*:=\s*(\S+)', (GAME_DIR / 'game.mk').read_text(), re.M).group(1)

# a package is made of host executables and libraries: those formats are expected here
HOST_MAGICS = {'Mach-O binary', 'Mach-O universal binary', 'Windows PE binary'}
GAME_MAGICS = [m for m in cnp.MAGICS if m[2] not in HOST_MAGICS]
HOST_EXTENSIONS = {'.exe', '.dll', '.so', '.dylib', '.a', '.lib', '.o', '.obj', '.pyd'}
GAME_EXTENSIONS = (cnp.BLOCKED_EXTENSIONS - HOST_EXTENSIONS) | {'.bfttf', '.bfotf', '.cnmt', '.romfs', '.istorage'}
CERT_EXTENSIONS = {'.pem', '.key', '.crt', '.der', '.p12', '.pfx', '.csr'}
KNOWN_SHA256 = {h: f'the v1.0.1 ExeFS file "{n}"' for n, h in versions.EXEFS_SHA256.items()}
DATA_DIRS = {'extracted', 'exefs', 'romfs', 'sysdata', 'private', 'generated', 'mods-generated', '.gamebuild', 'save',
             'cache'}
DATA_FILES = {'prod.keys', 'title.keys', 'dev.keys', 'setup.json', 'launcher.ini', 'larecomp.ini', f'{GAME_ID}.ini',
              'main.npdm'}
GAMELIB = re.compile(rf'^(lib)?{GAME_ID}game\.(dylib|dll|so)(\.id)?$|^lasgame\.')
GEN_CHUNK = re.compile(r'^(main|sdk)_(\d{3}|table)\.c$')
GUEST_FN = re.compile(rb'f_7[0-9a-f]{9}\b')  # (recompiled functions are named by guest address: f_71xxxxxxxx)
TEXT_LIMIT = 5 * 1024 * 1024
HASH_LIMIT = 64 * 1024 * 1024


def check_member(rel: str, size: int, read) -> list[str]:
    """rel: path inside the package (with '/'); read(): the file's bytes."""
    problems = []
    parts = [p for p in rel.split('/') if p]
    name = parts[-1].lower() if parts else ''
    suffix = os.path.splitext(name)[1]
    # zig's own C library sources and headers; [:4]: the macOS .zip has them at SM35Recomp.app/Contents/Resources/zig/
    in_zig = 'zig' in parts[:4]
    in_python = 'python' in parts[:4]
    for d in parts[:-1]:
        if d in DATA_DIRS and not (in_zig or in_python):
            problems.append(f'inside a "{d}" folder (a data folder\'s, made from a dump)')
            break
    if name in DATA_FILES:
        problems.append('a file of a set-up data folder or a key file')
    if GAMELIB.match(name):
        problems.append('the game code library built from a dump')
    if GEN_CHUNK.match(name) and not in_zig:
        problems.append('a recompiled game code chunk')
    if suffix in GAME_EXTENSIONS and not (suffix in CERT_EXTENSIONS and (in_zig or in_python)):
        # (zig's and Python's own test certificates are theirs; a key or certificate anywhere else is not)
        problems.append(f'blocked file type {suffix}')
    if name.startswith('nintendo_') and suffix in {'.ttf', '.otf', '.bfttf'}:
        problems.append('a console font')
    if re.fullmatch(r'01000000000008[0-9a-f]{2}', name):
        problems.append('a system data archive folder')
    if size == 0:
        return problems
    data = None

    def blob():
        nonlocal data
        if data is None:
            data = read()
        return data

    head = blob()[:0x110] if size <= HASH_LIMIT else read(0x110)
    for off, magic, label in GAME_MAGICS:
        if head[off:off + len(magic)] == magic:
            problems.append(f'content looks like {label}')
    if cnp.wiiu_rpx(head):
        problems.append('content looks like a Wii U executable')
    if size <= HASH_LIMIT:
        h = hashlib.sha256(blob()).hexdigest()
        if h in KNOWN_SHA256:
            problems.append(f'is {KNOWN_SHA256[h]}')
    if size <= TEXT_LIMIT and not in_zig:
        try:
            text = blob().decode('utf-8')
        except UnicodeDecodeError:
            text = ''
        if text:
            if cnp.KEY_LINE.search(text):
                problems.append('contains lines shaped like console key entries')
            if len(cnp.RECOMP_LINE.findall(text)) > cnp.RECOMP_LIMIT:
                problems.append('looks like recompiled game code')
            if len(cnp.ASM_LINE.findall(text)) > cnp.ASM_LIMIT and not in_python and suffix not in {'.zig', '.s'}:
                problems.append('looks like AArch64 disassembly')
    # the runtime itself: no recompiled game functions linked in (they're built on the player's machine)
    if re.fullmatch(rf'{GAME_ID}recomp(\.exe)?', name) and size <= HASH_LIMIT:
        n = len(GUEST_FN.findall(blob()))
        if n > 100:
            problems.append(f'the runtime has {n} recompiled game functions in it')
    return problems


def read_file(p: Path, n: int = -1) -> bytes:
    with open(p, 'rb') as f:
        return f.read(n)


def walk_dir(top: Path):
    for root, dirs, files in os.walk(top):
        dirs.sort()
        for f in sorted(files):
            p = Path(root) / f
            if p.is_symlink():
                continue
            rel = p.relative_to(top).as_posix()
            yield rel, p.stat().st_size, (lambda n=-1, p=p: read_file(p, n))
        for d in dirs:
            if (Path(root) / d).is_symlink():
                yield (Path(root) / d).relative_to(top).as_posix() + '/', 0, lambda n=-1: b''


def walk_zip(path: Path):
    with zipfile.ZipFile(path) as z:
        for i in z.infolist():
            if i.is_dir():
                continue
            yield i.filename, i.file_size, (lambda n=-1, i=i: z.open(i).read(n))


def walk_tar(path: Path):
    with tarfile.open(path) as t:
        for m in t.getmembers():
            if not m.isfile():
                continue
            yield m.name, m.size, (lambda n=-1, m=m: t.extractfile(m).read(n))


def check(path: Path) -> int:
    if path.is_dir():
        items = walk_dir(path)
    elif path.suffix == '.zip':
        items = walk_zip(path)
    elif path.name.endswith(('.tar.gz', '.tgz', '.tar.xz', '.tar')):
        items = walk_tar(path)
    else:
        print(f'check_package: {path}: not a folder, .zip or .tar.gz')
        return 1
    n = bad = 0
    total = 0
    for rel, size, read in items:
        n += 1
        total += size
        for problem in check_member(rel, size, read):
            print(f'GAME CONTENT? {path.name}/{rel}: {problem}')
            bad += 1
    if bad:
        print(f'check_package: {path}: {bad} problem(s) in {n} files: this package must not be published')
        return 1
    print(f'check_package: {path.name}: {n} files, {total / 1e6:.0f} MB, nothing from a game dump')
    return 0


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    return max(check(Path(a)) for a in argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
