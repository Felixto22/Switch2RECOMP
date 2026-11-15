"""Recompiling straight from a .nup, with no game: a small .nup is made here (two titles, a packed file, a game .zs file,
a nested folder), then

  - tools/nup/nup.py --verify accepts it and --extract writes the files byte for byte,
  - switchrecomp.py <file>.nup --no-build unpacks the newest title into <Title>_data (exefs + romfs) and translates it,
  - a second run reuses what is there (nothing written again), and a full drive is refused before writing.

    python3 tests/test_nup.py            (Python 3.14: compression.zstd; older: the zstandard module)
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NUP = os.path.join(ROOT, 'tools', 'nup', 'nup.py')
RECOMP = os.path.join(ROOT, 'switchrecomp.py')


def zstd_compress(data):
    try:
        from compression import zstd
        return zstd.compress(data)
    except ImportError:
        import zstandard
        return zstandard.ZstdCompressor().compress(data)


def nso(code_words, name=b'test'):
    """a plain (uncompressed) NSO: .text = the words, then a small .rodata and .data"""
    text = b''.join(struct.pack('<I', w) for w in code_words)
    text += b'\0' * (-len(text) % 0x1000)
    ro = b'\0' * 0x1000
    data = b'\0' * 0x1000
    h = bytearray(0x100)
    h[:4] = b'NSO0'
    flags = 8 | 16 | 32   # (the segment hashes are there and checked)
    struct.pack_into('<I', h, 0xC, flags)
    foff = 0x100 + len(name)
    moff = 0
    segs = []
    for seg in (text, ro, data):
        segs.append((foff, moff, len(seg)))
        foff += len(seg)
        moff += len(seg)
    for i, (fo, mo, sz) in enumerate(segs):
        struct.pack_into('<III', h, 0x10 + 0x10 * i, fo, mo, sz)
        struct.pack_into('<I', h, 0x60 + 4 * i, sz)
    struct.pack_into('<I', h, 0x1C, 0x100)
    struct.pack_into('<I', h, 0x2C, len(name))
    struct.pack_into('<I', h, 0x3C, 0x1000)   # bss
    h[0x40:0x60] = hashlib.sha256(text).digest()   # (a build id)
    for i, seg in enumerate((text, ro, data)):
        h[0xA0 + 0x20 * i:0xC0 + 0x20 * i] = hashlib.sha256(seg).digest()
    return bytes(h) + name + text + ro + data


def npdm(name):
    d = bytearray(0x80)
    d[:4] = b'META'
    d[0xC] = 1
    d[0xE] = 44
    struct.pack_into('<I', d, 0x1C, 0x100000)
    d[0x20:0x20 + len(name)] = name
    struct.pack_into('<I', d, 0x70, 0x80)
    aci = bytearray(0x40)
    aci[:4] = b'ACI0'
    struct.pack_into('<Q', aci, 0x10, 0x0400000000123000)
    return bytes(d + aci)


def write_nup(path, titles):
    """titles: [(title id, version, {'exefs': {path: bytes}, 'romfs': {path: (bytes, packed)}})]"""
    dirs, files, kids, strings = [], [], [], bytearray()
    blobs = []

    def s(name):
        off = len(strings)
        strings.extend(name.encode('utf-8'))
        return off, len(name.encode('utf-8'))

    def tree(entries):
        """a dict {name: subtree dict | (bytes, packed)} -> its dir index"""
        me = len(dirs)
        dirs.append(None)
        children = []
        for name in sorted(entries):
            v = entries[name]
            if isinstance(v, dict):
                children.append(('d', tree(v), name))
            else:
                data, packed = v
                stored = zstd_compress(data) if packed else data
                blobs.append(stored)
                files.append([name, len(data), len(blobs) - 1, len(stored), 1 if packed else 0,
                              hashlib.sha256(data).digest()])
                children.append(('f', len(files) - 1, name))
        dirs[me] = (children,)
        return me

    def nest(flat):
        root = {}
        for p, v in flat.items():
            at = root
            parts = p.split('/')
            for part in parts[:-1]:
                at = at.setdefault(part, {})
            at[parts[-1]] = v
        return root

    trecs = []
    for tid, ver, parts in titles:
        ex = {k: (v, False) for k, v in parts['exefs'].items()}
        root_i = tree({'exefs': nest(ex), 'romfs': nest(parts['romfs'])})
        # the root's children: exefs then romfs (sorted names)
        ch = dirs[root_i][0]
        trecs.append((tid, root_i, ch[0][1], ch[1][1], ver))
    nd, nf = len(dirs), len(files)
    dir_recs, file_recs = [], []
    for d in dirs:
        children = d[0]
        first = len(kids)
        for kind, idx, name in children:
            kids.append(idx if kind == 'd' else nd + idx)
        dir_recs.append([first, len(children)])
    # names: a dir's own name is the name its parent gave it
    dir_names = {0: ''}
    for d in dirs:
        for kind, idx, name in d[0]:
            if kind == 'd': dir_names[idx] = name
    for i in range(nd):
        dir_names.setdefault(i, '')
    dir_name_ref = [s(dir_names[i]) for i in range(nd)]
    file_name_ref = [s(f[0]) for f in files]
    nt = len(trecs)
    tables = 0x30 + 0x40 * nt + 24 * nd + 80 * nf + 4 * len(kids)
    soff = tables
    toc_size = soff + len(strings)
    blob_offset = (0x80 + toc_size + 15) & ~15
    offs, at = [], blob_offset
    for b in blobs:
        offs.append(at)
        at = (at + len(b) + 15) & ~15
    total = at
    toc = bytearray(b'TOC\x01')
    toc += struct.pack('<IQQQQ', nt, nd, nf, soff, len(strings))
    toc += b'\0' * (0x30 - len(toc))
    for tid, root_i, ex_i, ro_i, ver in trecs:
        t = bytearray(0x40)
        struct.pack_into('<QIIII', t, 0, tid, root_i, ex_i, ro_i, ver)
        toc += t
    for i in range(nd):
        no, nl = dir_name_ref[i]
        toc += struct.pack('<IIIIQ', no, nl, dir_recs[i][0], dir_recs[i][1], 0)
    for i, f in enumerate(files):
        no, nl = file_name_ref[i]
        r = bytearray(80)
        struct.pack_into('<IIQQQ', r, 0, no, nl, f[1], offs[f[2]] if f[1] else 0, f[3] if f[1] else 0)
        r[32:64] = f[5]
        r[64] = f[4] if f[1] else 0
        toc += r
    toc += struct.pack('<%dI' % len(kids), *kids)
    toc += strings
    assert len(toc) == toc_size
    h = bytearray(0x80)
    h[:4] = b'NUP\x01'
    struct.pack_into('<QQ', h, 8, 0x80, toc_size)
    h[24:56] = hashlib.sha256(toc).digest()
    struct.pack_into('<Q', h, 0x38, blob_offset)
    struct.pack_into('<Q', h, 0x40, total)
    with open(path, 'wb') as f:
        f.write(h + toc)
        f.write(b'\0' * (blob_offset - f.tell()))
        for o, b in zip(offs, blobs):
            f.seek(o)
            f.write(b)
        f.truncate(total)


RET = 0xD65F03C0
MOV_X0_0 = 0xD2800000
BASE = {'exefs': {'main': nso([MOV_X0_0, RET], b'base'), 'main.npdm': npdm(b'Application'), 'rtld': nso([RET], b'rtld')},
        'romfs': {'Data/levels.bin': (os.urandom(70000), False), 'Data/Model/hero.zs': (zstd_compress(b'hero' * 5000), False),
                  'readme.txt': (b'base v0', False)}}
UPDATE = {'exefs': {'main': nso([MOV_X0_0, MOV_X0_0, RET], b'update'), 'main.npdm': npdm(b'Application')},
          'romfs': {'Data/levels.bin': (os.urandom(90000), True), 'Data/Model/hero.zs': (zstd_compress(b'hero2' * 5000), False),
                    'Data/deep/a/b/c/name with spaces.dat': (b'x' * 1234, True), 'readme.txt': (b'update v65536', False)}}


class NupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='s2r-nup-')
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.nup = os.path.join(self.tmp, "Fire Emblem- Test's Weave.nup")
        write_nup(self.nup, [(0x0400000000123000, 0, BASE), (0x0400000000123800, 65536, UPDATE)])

    def run_py(self, *args, ok=True):
        r = subprocess.run([sys.executable, '-I', *args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=self.tmp)
        if ok and r.returncode:
            self.fail(r.stdout)
        return r

    def check_tree(self, root, parts):
        for rel, v in parts['romfs'].items():
            with open(os.path.join(root, 'romfs', *rel.split('/')), 'rb') as f:
                self.assertEqual(f.read(), v[0], rel)
        for rel, v in parts['exefs'].items():
            with open(os.path.join(root, 'exefs', rel), 'rb') as f:
                self.assertEqual(f.read(), v, rel)

    def test_verify_and_extract(self):
        r = self.run_py(NUP, self.nup, '--verify')
        self.assertIn('0 bad', r.stdout)
        out = os.path.join(self.tmp, 'x')
        self.run_py(NUP, self.nup, '--extract', out)
        self.check_tree(out, UPDATE)
        # the update has no rtld: it comes from the base title
        with open(os.path.join(out, 'exefs', 'rtld'), 'rb') as f:
            self.assertEqual(f.read(), BASE['exefs']['rtld'])

    def test_recompile_from_nup(self):
        games = os.path.join(self.tmp, 'games')
        r = self.run_py(RECOMP, self.nup, '--no-build', '--outdir', games)
        data = os.path.join(games, 'Fire Emblem- Tests Weave_data')
        self.assertTrue(os.path.isdir(data), r.stdout)
        self.check_tree(data, UPDATE)
        self.assertTrue(os.path.isfile(os.path.join(games, 'Fire Emblem- Tests Weave.ini')))
        self.assertTrue(os.path.isdir(os.path.join(games, 'Fire Emblem- Tests Weave_build')))
        self.assertFalse([n for n in os.listdir(games) if n.startswith('.nup_')], 'the staging folder stays behind')
        # again: the romfs is already there, nothing of it is written a second time
        before = {p: os.stat(os.path.join(dp, p)).st_mtime_ns for dp, _, fs in os.walk(os.path.join(data, 'romfs')) for p in fs}
        r = self.run_py(RECOMP, self.nup, '--no-build', '--outdir', games)
        after = {p: os.stat(os.path.join(dp, p)).st_mtime_ns for dp, _, fs in os.walk(os.path.join(data, 'romfs')) for p in fs}
        self.assertEqual(before, after, r.stdout)
        self.assertIn('0.0 GB to write', r.stdout)

    def test_base_title_on_request(self):
        games = os.path.join(self.tmp, 'games')
        self.run_py(RECOMP, self.nup, '--no-build', '--outdir', games, '--nup-title', '0', '-o', 'Base')
        self.check_tree(os.path.join(games, 'Base_data'), BASE)

    def test_full_drive_is_refused(self):
        out = os.path.join(self.tmp, 'y')
        code = ("import shutil, runpy, sys, collections; U = collections.namedtuple('U', 'total used free');"
                "shutil.disk_usage = lambda p: U(1 << 40, 1 << 40, 10 << 20); sys.argv = sys.argv[1:];"
                "runpy.run_path(sys.argv[0], run_name='__main__')")
        r = self.run_py('-c', code, NUP, self.nup, '--extract', out, ok=False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('Not enough room', r.stdout)
        self.assertFalse(os.path.exists(os.path.join(out, 'romfs')))


if __name__ == '__main__':
    unittest.main(verbosity=2)
