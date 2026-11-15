"""The few zstandard calls nup.py makes, on whichever zstd this Python has.

Python 3.14 (the one Install.bat puts in python\\) has zstd built in as compression.zstd. Older Pythons get the
zstandard wheel that tool/get_deps.py unpacks into tool/pylib. nup.py only needs:

    zstandard.ZstdDecompressor().decompressobj()   .decompress(data), .eof, .unused_data
    zstandard.ZstdError
    zstandard.get_frame_parameters(header)          raises on a header that is not a zstd frame

    from zstd_compat import zstandard
"""
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_PYLIB = os.path.join(os.path.dirname(os.path.dirname(_HERE)), "tool", "pylib")
if os.path.isdir(_PYLIB) and _PYLIB not in sys.path:
    sys.path.append(_PYLIB)

try:
    import zstandard  # noqa: F401  (the wheel, when it is there: what nup.py was written against)
except ImportError:
    try:
        from compression import zstd as _zstd
    except ImportError:
        raise SystemExit("Reading a .nup needs zstd: Python 3.14 or newer (Install.bat puts it in python\\), "
                         "or run Install.bat / Install.sh once so tool\\pylib gets the zstandard module.")

    class _Decompressor:
        def decompressobj(self):
            return _zstd.ZstdDecompressor()

    class _Namespace:
        ZstdError = _zstd.ZstdError

        @staticmethod
        def ZstdDecompressor():
            return _Decompressor()

        @staticmethod
        def get_frame_parameters(header):
            return _zstd.get_frame_info(header)

    zstandard = _Namespace()
