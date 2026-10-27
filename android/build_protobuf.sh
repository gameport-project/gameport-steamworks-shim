#!/usr/bin/env bash
# Builds protobuf (lite runtime) and Abseil for Android arm64 into $GP_PROTOBUF_BUILD (default /tmp/pb-android-build),
# which android/build_shim.sh links the shim against. build_shim.sh runs this itself when that folder is missing.
# Abseil is downloaded by protobuf's own CMake at the version it pins, so this needs network access the first time.
#
# Usage: ANDROID_NDK=<ndk> GP_PROTOBUF_SRC=<protobuf v35.1 sources> android/build_protobuf.sh
# Needs cmake and ninja (the Android SDK ships both under <sdk>/cmake/<version>/bin).
set -euo pipefail
ndk="${ANDROID_NDK:?set ANDROID_NDK to an Android NDK folder}"
src="${GP_PROTOBUF_SRC:?set GP_PROTOBUF_SRC to a protobuf v35.1 source checkout}"
out="${GP_PROTOBUF_BUILD:-/tmp/pb-android-build}"

if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
  sdk_bin="$(ls -d "$(dirname "$(dirname "$ndk")")"/cmake/*/bin 2>/dev/null | tail -1 || true)"
  [[ -n "$sdk_bin" ]] && export PATH="$sdk_bin:$PATH"
fi
command -v cmake >/dev/null && command -v ninja >/dev/null || { echo "cmake and ninja are required (put the Android SDK's cmake/<version>/bin on PATH)" >&2; exit 1; }

cmake -S "$src" -B "$out" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_CXX_STANDARD=17 -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_CONFORMANCE=OFF -Dprotobuf_BUILD_EXAMPLES=OFF \
  -Dprotobuf_BUILD_PROTOC_BINARIES=OFF -Dprotobuf_BUILD_LIBUPB=OFF \
  -Dprotobuf_FORCE_FETCH_DEPENDENCIES=ON -Dprotobuf_INSTALL=OFF \
  -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$src=protobuf"
cmake --build "$out" --target libprotobuf-lite utf8_validity utf8_range
echo "protobuf and Abseil built in $out"
