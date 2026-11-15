"""ZBIC decompression (Switch firmware 22.0.0+'s zstd variant) through zbic/, built to zbic.dll (zbic.so) on first use.

    import zbic; plain = zbic.decompress(data)

The library is built with the GCC that Install.bat / Install.sh put in gcc\\, or with the system's gcc / clang, or
with the compiler in the CC environment variable. It is a separate tool: none of it goes into a recompiled game.
"""
import ctypes
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))          # the SwitchRecomp folder
SRC = os.path.join(HERE, "zbic")
DLL = os.path.join(SRC, "zbic.dll" if os.name == "nt" else "zbic.so")
_lib = None


def _compiler():
    cands = []
    if os.environ.get("CC"):
        cands.append(os.environ["CC"])
    if os.name == "nt":
        cands += [os.path.join(ROOT, "gcc", "Library", "bin", "x86_64-w64-mingw32-gcc.exe"), "gcc", "clang"]
    else:
        cands += [os.path.join(ROOT, "gcc", "bin", "x86_64-conda-linux-gnu-gcc"), "gcc", "clang", "cc"]
    for c in cands:
        if os.path.isfile(c) or shutil.which(c):
            return c
    raise SystemExit("Reading this game's code needs a C compiler once (to build zbic for firmware 22+ games). "
                     "Run Install.bat (Windows) or Install.sh (Linux) once: it downloads GCC into the gcc folder.")


def _build():
    # (a zbic library that is already there is used as it is: a zip can change file dates, so they are not compared)
    if os.path.exists(DLL):
        return
    cc = _compiler()
    srcs = [os.path.join(SRC, "zstd.c"), os.path.join(SRC, "zbic_api.c")]
    cmd = [cc, "-O2", "-shared", "-DZSTD_ZBIC_SUPPORT=1", "-DZSTD_MULTITHREAD=0", "-I", SRC, "-o", DLL + ".part", *srcs]
    if os.name == "nt":
        cmd += ["-static-libgcc", "-static"]   # (no MinGW DLLs needed next to it)
    else:
        cmd += ["-fPIC"]
    print("Building zbic (only the first time)...", flush=True)
    env = dict(os.environ)
    bindir = os.path.dirname(cc)
    if bindir:   # (the bundled GCC finds its own programs and DLLs through PATH)
        env["PATH"] = bindir + os.pathsep + env.get("PATH", "")
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", env=env)
    if r.returncode or not os.path.exists(DLL + ".part"):
        raise SystemExit("Could not build zbic with %s:\n%s" % (cc, r.stdout[-3000:]))
    os.replace(DLL + ".part", DLL)


def lib():
    global _lib
    if _lib is None:
        _build()
        _lib = ctypes.CDLL(DLL)
        _lib.zbic_content_size.restype = ctypes.c_longlong
        _lib.zbic_content_size.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        _lib.zbic_decompress.restype = ctypes.c_longlong
        _lib.zbic_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
    return _lib


def is_zbic(data):
    return data[:4] == b"ZBIC"


def decompress(data, size=None):
    L = lib()
    if size is None:
        size = L.zbic_content_size(data, len(data))
        if size < 0:
            raise ValueError("ZBIC: no content size in the frame header")
    out = ctypes.create_string_buffer(size)
    n = L.zbic_decompress(out, size, data, len(data))
    if n != size:
        raise ValueError("ZBIC: decompression failed (%d of %d bytes)" % (n, size))
    return out.raw


if __name__ == "__main__":
    lib()
    print(DLL)
    sys.exit(0)
