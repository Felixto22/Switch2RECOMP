#!/bin/bash
# Cross-builds, on Linux with mingw-w64 (win32 threads), the Windows libraries the runtime links when they are there
# (the release's prebuilt .a files are not in the repository: *.a is ignored), then the runtime itself:
#   runtime/opus/win/libopus.a          Opus 1.5.2 (hwopus)
#   runtime/oh264/win/libdecoder.a ...  OpenH264 2.4.1, decoder (NVDEC H.264 videos)
#   runtime/vp9/win/libvpx.a            libvpx 1.14.1, VP9 decoder only (WebM movies; runtime/vp9/win/README.md)
#   runtime/spvc/win/libspvc.a          SPIRV-Cross vulkan-sdk-1.4.341.0 (C API 0.68, the headers in runtime/spvc)
#   build/nvn/libnvn.a                  renderer=vulkan (tools/build_nvn.py)
#   runtime/edenvk/win/libedenvk.a ...  renderer=eden (runtime/edenvk, needs the Boost headers)
#   bin/runtime_win64_win32.exe
# Needs: gcc-mingw-w64-x86-64-win32 g++-mingw-w64-x86-64-win32 cmake ninja-build nasm glslang-tools libboost-dev git python3
#   tools/build_win_libs.sh [work folder, default ../s2r-deps]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd); R=$ROOT/runtime; D=${1:-$ROOT/../s2r-deps}; X=x86_64-w64-mingw32
mkdir -p "$D"; cd "$D"
clone(){ rm -rf "$2"; git -c advice.detachedHead=false clone -q --depth 1 -b "$1" "$3" "$2"; }
if [ ! -f $R/opus/win/libopus.a ]; then echo "opus"
  clone v1.5.2 opus https://github.com/xiph/opus
  cmake -S opus -B opus/b -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$X-gcc -DCMAKE_BUILD_TYPE=Release \
    -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DBUILD_SHARED_LIBS=OFF >/dev/null
  ninja -C opus/b >/dev/null; mkdir -p $R/opus/win; cp opus/b/libopus.a $R/opus/win/; fi
if [ ! -f $R/oh264/win/libdecoder.a ]; then echo "openh264"
  clone v2.4.1 openh264 https://github.com/cisco/openh264
  make -C openh264 -j"$(nproc)" OS=mingw_nt ARCH=x86_64 CC=$X-gcc CXX=$X-g++ AR=$X-ar USE_ASM=Yes libraries >/dev/null
  mkdir -p $R/oh264/win; cp openh264/libdecoder.a openh264/libcommon.a $R/oh264/win/; fi
if [ ! -f $R/vp9/win/libvpx.a ]; then echo "libvpx"
  clone v1.14.1 libvpx https://github.com/webmproject/libvpx
  (cd libvpx && CROSS=$X- ./configure --target=x86_64-win64-gcc --enable-static --disable-shared --disable-vp8 \
    --disable-vp9-encoder --enable-vp9-decoder --disable-examples --disable-tools --disable-docs \
    --disable-unit-tests --disable-multithread --as=nasm >/dev/null && make -j"$(nproc)" >/dev/null)
  cp libvpx/libvpx.a $R/vp9/win/; fi
if [ ! -f $R/spvc/win/libspvc.a ]; then echo "SPIRV-Cross"
  clone vulkan-sdk-1.4.341.0 spvc https://github.com/KhronosGroup/SPIRV-Cross
  grep -q "SPVC_C_API_VERSION_MINOR 68" spvc/spirv_cross_c.h || { echo "SPIRV-Cross: not C API 0.68"; exit 1; }
  cmake -S spvc -B spvc/b -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$X-gcc -DCMAKE_CXX_COMPILER=$X-g++ \
    -DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF \
    -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF >/dev/null
  ninja -C spvc/b >/dev/null
  rm -rf spvc/m && mkdir spvc/m && (cd spvc/m && for a in ../b/libspirv-cross-*.a; do $X-ar x "$a"; done && \
    mkdir -p $R/spvc/win && $X-ar rcs $R/spvc/win/libspvc.a *.obj); fi
cd "$ROOT"
mkdir -p external/deps; [ -e external/deps/Vulkan-Headers ] || ln -s ../../runtime/edenvk/deps/vulkan-headers external/deps/Vulkan-Headers
echo "NVN layer"; python3 -I tools/build_nvn.py --jobs "$(nproc)"
if [ ! -f $R/edenvk/win/libedenvk.a ]; then echo "edenvk (takes a while)"
  mkdir -p "$D/boost"; ln -sfn /usr/include/boost "$D/boost/boost"
  cmake -S $R/edenvk -B $R/edenvk/buildw -G Ninja -DCMAKE_TOOLCHAIN_FILE=$R/edenvk/mingw.cmake -DEVK_BOOST_INCLUDE="$D/boost" >/dev/null
  ninja -C $R/edenvk/buildw >/dev/null
  mkdir -p $R/edenvk/win; find $R/edenvk/buildw \( -name libedenvk.a -o -name libsirit.a \) -exec cp {} $R/edenvk/win/ \;; fi
echo "runtime"
python3 -I switchrecomp.py --build-runtime --target win64 --backend win32 --cc $X-gcc
