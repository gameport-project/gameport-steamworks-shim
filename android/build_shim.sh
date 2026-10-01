#!/usr/bin/env bash
# Builds the Steamworks shim for Android (arm64): recompiles the sources (with logcat tracing) and
# relinks libsteamclient.so into /tmp/gp-shim-build/lib. Then run ../gameport-app/scripts/stage_shim.sh
# to copy it into the GamePort app, where it is injected into the games it patches.
#
# Usage: ANDROID_NDK=<ndk> GP_PROTOBUF_SRC=<protobuf v35.1 sources> android/build_shim.sh
# (GP_PROTOBUF_BUILD, default /tmp/pb-android-build, is where protobuf and Abseil were built for Android.)
set -euo pipefail
ndk="${ANDROID_NDK:?set ANDROID_NDK to an Android NDK folder}"
: "${GP_PROTOBUF_SRC:?set GP_PROTOBUF_SRC to a protobuf v35.1 source checkout (temporary until the shim has its own CMake build)}"

SRC="$(cd "$(dirname "$0")/.." && pwd)"
CXX="$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin | head -1)/aarch64-linux-android29-clang++"
PB="${GP_PROTOBUF_BUILD:-/tmp/pb-android-build}"
PB_INC="$GP_PROTOBUF_SRC/src"
ABSL_INC="$PB/_deps/absl-src"
UTF8_INC="$GP_PROTOBUF_SRC/third_party/utf8_range"
OBJ=/tmp/gp-shim-build/obj4
OUT=/tmp/gp-shim-build/lib

mkdir -p "$OBJ" "$OUT"
rm -f "$OBJ"/*.o
cd "$SRC"
for f in dll/*.cpp dll/net.pb.cc; do
  [[ "$f" == "dll/wrap.cpp" ]] && continue
  out="$OBJ/$(basename "$f" | sed 's/\.[^.]*$/.o/')"
  "$CXX" -c -fPIC -o "$out" "$f" -Idll -I"$PB_INC" -I"$ABSL_INC" -I"$UTF8_INC" \
    -g0 -ffile-prefix-map="$SRC=gameport-steamworks-shim" -ffile-prefix-map="$GP_PROTOBUF_SRC=protobuf" -ffile-prefix-map="$HOME=~" -Wno-return-type -Wno-format -std=c++17 -DEMU_RELEASE_BUILD -DSTEAMCLIENT_DLL -DGP_LOGCAT
done
echo "compiled $(ls "$OBJ"/*.o | wc -l) objects"

ABSL_LIBS=()
while IFS= read -r lib; do ABSL_LIBS+=("$lib"); done < <(find "$PB/_deps/absl-build" -iname "*.a")

"$CXX" -shared -static-libstdc++ -Wl,--no-undefined -o "$OUT/libsteamclient.so" \
  "$OBJ"/*.o \
  -Wl,--start-group \
  "$PB/libprotobuf-lite.a" \
  "$PB/third_party/utf8_range/libutf8_validity.a" \
  "$PB/third_party/utf8_range/libutf8_range.a" \
  "${ABSL_LIBS[@]}" \
  -Wl,--end-group \
  -llog -landroid -lm -ldl

echo "shim linked OK"
nm -D "$OUT/libsteamclient.so" | grep -w CreateInterface
