# tools/nup: the .nup reader

A `.nup` is a decrypted Switch 2 dump in one file: one or more titles (base game, update), each with an `exefs/` and
a `romfs/` tree. `switchrecomp.py game.nup` runs these tools as their own program to unpack it into `<Game>_data/`.

    python -I tools/nup/nup.py game.nup                       list the titles
    python -I tools/nup/nup.py game.nup --verify              read-only check of every file's hash
    python -I tools/nup/nup.py game.nup --extract DIR [--title N] [--only exefs|romfs]

| File | What it is | License |
| --- | --- | --- |
| `nup.py`, `unpack.py` | The reader and the ZBIC -> plain NSO step, from [Hookshot](https://github.com/NotRexed/Hookshot) (`nx2/`) by NotRexed and its contributors. Changes here: zstd through `zstd_compat.py`, a free-space check before extracting | GPL-3.0-or-later (`LICENSE`) |
| `zstd_compat.py`, `zbic.py` | zstd on Python 3.14's `compression.zstd` or the `zstandard` module; the zbic library built with the bundled GCC | GPL-3.0-or-later |
| `zbic/` | Atmosphere-NX's single-file zstd with ZBIC support, via kinnay/zbic (`PROVENANCE.md`) | zstd BSD-or-GPLv2, ZBIC parts GPLv2 (`zbic/LICENSE`) |

These are separate programs that SwitchRecomp starts; none of their code is linked into the recompiler's runtime or
into a recompiled game. No game files, keys or firmware are included.
