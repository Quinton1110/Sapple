#!/bin/sh
# Generate TruVoiceData/tvdata-elf.s: the TruVoice engine tables (Centigram's data, from the pinned OpenTV upstream's
# data/en/engine.tvdata) as an ELF data image for the Android build (Android/app/src/main/cpp/CMakeLists.txt picks it
# up when present). The same steps as tools/truvoice_data.sh, without the Mach-O rewrite: gen_data.py already writes
# ELF-flavoured assembly, and the symbol names come from objects built by the NDK's clang and read by its llvm-nm.
#
# VOICE DATA: TruVoiceData/ is gitignored. Never commit the output.
set -e
cd "$(dirname "$0")/.."
UP=Engine/opentv-upstream
PIN=b8897650b9e0dde14f95f67db3937e97858baa41
NDK=${ANDROID_NDK_HOME:-$(ls -d "${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}"/ndk/* 2>/dev/null | sort | tail -1)}
BIN=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin | head -1)
[ -x "$BIN/clang" ] || { echo "no NDK clang under $NDK (set ANDROID_NDK_HOME)" >&2; exit 1; }
if [ ! -f "$UP/data/en/engine.tvdata" ]; then
  echo "no $UP: git clone https://github.com/RetroBunn/tv-decomp $UP && git -C $UP checkout $PIN" >&2
  exit 1
fi
if [ -d "$UP/.git" ] && [ "$(git -C "$UP" rev-parse HEAD)" != "$PIN" ]; then
  echo "warning: $UP is at $(git -C "$UP" rev-parse --short HEAD), not the pinned $PIN" >&2
fi
(cd "$UP/data/en" && shasum -a 256 -c ../../../../tools/truvoice_data.sha256 2>/dev/null | grep engine.tvdata) ||
  { echo "engine.tvdata does not match tools/truvoice_data.sha256" >&2; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/obj" "$TMP/py" "$TMP/bin"
for f in Engine/opentv/src/engine/*.c Engine/opentv/src/port/*.c; do
  "$BIN/clang" --target=aarch64-linux-android26 -c -std=gnu11 -O2 -w -fwrapv -fno-strict-aliasing -DCV_NO_DEBUG_ENV \
    -IEngine/opentv/src -IEngine/opentv/include -IEngine/opentv/generated "$f" -o "$TMP/obj/$(basename "$f" .c).o"
done
ln -s "$BIN/llvm-nm" "$TMP/bin/nm"   # gen_data.py runs `nm`
echo "# stub: only the .tvdata path of gen_data.py is used" > "$TMP/py/pefile.py"
mkdir -p TruVoiceData
PATH="$TMP/bin:$PATH" PYTHONPATH="$TMP/py" python3 "$UP/tools/gen_data.py" "$UP/data/en/engine.tvdata" \
  Engine/opentv/src TruVoiceData/tvdata-elf.s "$TMP"/obj/*.o
ls -la TruVoiceData/tvdata-elf.s
