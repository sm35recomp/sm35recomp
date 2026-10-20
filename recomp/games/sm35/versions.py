"""The known-good Super Mario Bros. 35 v1.0.1 dumps: identifiers of the player's own files, never the files.

setup.py checks a dump against these before anything is built from it; the launcher (runtime/ui/launcher.mm) runs
setup.py, and scripts/check_package.py uses the hashes to make sure no release package carries the game's code.

- NSO build ids (module ids) name a build of the code: the recomp's addresses, patches and findings are for v1.0.1's.
- SHA-256 of the extracted ExeFS files: the same for every v1.0.1 dump (NSP, XCI, an emulator's extraction), since the
  update ships the whole ExeFS. Taken from the owner's dump (2026-10-20).
- Title ids, versions and the content ids of the program NCAs (Nintendo's own content hashes, as listed in each
  title's metadata), to name what an NSP / XCI holds before anything is extracted.
"""

TITLE = 'Super Mario Bros. 35'
BASE_TITLE_ID = 0x0100277011F1A000
UPDATE_TITLE_ID = 0x0100277011F1A800
# title versions as the titles' metadata gives them (a patch counter: 1.0.1 is the first update), and what they are
UPDATE_VERSION = 65536  # 0x10000
TITLE_VERSIONS = {
    (BASE_TITLE_ID, 0): '1.0.0',
    (UPDATE_TITLE_ID, 65536): '1.0.1',
}

# NSO build ids (hex of the 0x20-byte module id, trailing zeros dropped) by version. Only main changed in 1.0.1.
BUILD_IDS = {
    '1.0.1': {
        'main': 'ef1e5ea4171b94b0f22a6c565bb300175a6bf5ee',
        'sdk': '95832210b36121e88b4f9b7b66239dade9c82a67',
        'subsdk0': '62802903bbb1e41dfef55e6ba53c9b3e90b8c57c',
        'subsdk1': '3ab92c3444d4ece2f52417b5d8ee86eb115212a3',
    },
    # (recognised, to say what's wrong: the base game without the update)
    '1.0.0': {
        'main': '43c0f80d3d1a7bd427282f5422b6657c11841db1',
    },
}

# SHA-256 of the v1.0.1 ExeFS files
EXEFS_SHA256 = {
    'main': '9f45b6d0ea9583ef0b341527a229e0fb0371833c6e00e446050bf86bba115131',
    'sdk': '71a49827250b6bc2883ac44e5d42f4d20110a95d930c2387311c7ff415eb4534',
    'subsdk0': '2e9a8e5e053fa7eda02bd0556fcff34e107b3a82828933b14502bd9037a1a397',
    'subsdk1': 'b515e170ed8799af63e0f2e1a58b344ec8c78605976a2148374d13e6ced93d30',
    'rtld': 'b09545d23efe76925191e9ded2c5ca81179f07fc823162c2b367a63b82d8d9fc',
    'main.npdm': '74f75ea151bec385a691d355643476394e01281367d8a850ca8e452b6b5db33b',
}

# Program NCA content ids (the first half of the NCA's SHA-256) of the known titles
PROGRAM_NCA_IDS = {
    'efbee7326c394bb3419a8950b2ce3259': 'base game 1.0.0',
    '5962e62ec406cccdb3f62e92d718bfb2': 'update 1.0.1',
}

# subsdk0 is GLSLC (the runtime shader compiler) and subsdk1 the movie library (runtime/hle/movie.c stands in for it)
EXEFS_FILES = ('main', 'sdk', 'subsdk0', 'subsdk1', 'rtld', 'main.npdm')

# The RomFS is small (about 370 MB, 172 files with the update): what a complete one has at least
ROMFS_MIN_FILES = 150
ROMFS_MIN_BYTES = 256 << 20


def version_of_build_id(module, build_id):
    """'1.0.1', '1.0.0', or None for a build id of another version (or another game)."""
    for ver, ids in BUILD_IDS.items():
        want = ids.get(module)
        if want and build_id and build_id.startswith(want):
            return ver
    return None


def display_version(title_id, v):
    """A title version by its name ('1.0.1'), or its number for an unknown one ('v131072')."""
    return TITLE_VERSIONS.get((title_id, v), f'v{v}')
