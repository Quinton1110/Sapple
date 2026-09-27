#!/bin/sh
# Compile check without any voice data: fills the gitignored data locations with placeholders so that
# `xcodegen generate` and an unsigned simulator build succeed on a clean checkout. Nothing that already exists is
# touched. A build made this way compiles and links but CANNOT speak with any voice whose data is a placeholder
# (TruVoice's tables are zeros): use it to check that the code builds, never install or ship it.
#
#   tools/stub_data.sh            then   xcodegen generate
#   tools/stub_data.sh --clean    removes only the placeholders this script made (marked with .stub files)
set -eu
cd "$(dirname "$0")/.."

DIRS="VoiceData SAPI4Voices AnnaVoice OneCoreVoice NeuralVoices"
SDK_LIBS="core extension.embedded.tts extension.onnxruntime"

if [ "${1:-}" = "--clean" ]; then
  for d in $DIRS TruVoiceData NeuralSDK; do
    if [ -e "$d/.stub" ]; then rm -rf "$d"; echo "removed placeholder $d/"; fi
  done
  exit 0
fi

for d in $DIRS; do
  if [ ! -e "$d" ]; then mkdir -p "$d" && touch "$d/.stub" && echo "placeholder $d/ (empty)"; fi
done

# TruVoice: zero-filled definitions of every table the OpenTV engine refers to (the real ones: `make truvoice-data`).
if [ ! -e TruVoiceData ]; then
  tmp=$(mktemp -d)
  for c in Engine/opentv/src/engine/*.c Engine/opentv/src/port/*.c; do
    xcrun clang -std=gnu11 -fwrapv -fno-strict-aliasing -w -IEngine/opentv/include -IEngine/opentv/src \
      -IEngine/opentv/generated -c "$c" -o "$tmp/$(basename "$c" .c).o"
  done
  nm -u "$tmp"/*.o | grep -E '^_(g_|tv_data)' | sort -u > "$tmp/undef"
  nm -g -U "$tmp"/*.o | awk 'NF == 3 {print $3}' | sort -u > "$tmp/def"
  mkdir -p TruVoiceData && touch TruVoiceData/.stub
  comm -23 "$tmp/undef" "$tmp/def" | while read -r s; do
    printf '\t.globl %s\n\t.zerofill __DATA,__bss,%s,65536,4\n' "$s" "$s"
  done > TruVoiceData/tvdata.s
  echo "placeholder TruVoiceData/tvdata.s ($(grep -c globl TruVoiceData/tvdata.s) zero-filled tables)"
  rm -rf "$tmp"
fi

# The neural voices' Speech SDK: three empty dylibs with the names the targets embed (the iOS ones for the simulator).
if [ ! -e NeuralSDK ]; then
  mkdir -p NeuralSDK/ios NeuralSDK/macos && touch NeuralSDK/.stub
  src=$(mktemp -d)/stub.c && echo 'void sapple_sdk_placeholder(void) {}' > "$src"
  for n in $SDK_LIBS; do
    lib=libMicrosoft.CognitiveServices.Speech.$n.dylib
    xcrun --sdk iphonesimulator clang -target arm64-apple-ios18.0-simulator -dynamiclib \
      -install_name "@rpath/$lib" -o "NeuralSDK/ios/$lib" "$src"
    xcrun --sdk macosx clang -target arm64-apple-macos15.0 -dynamiclib \
      -install_name "@rpath/$lib" -o "NeuralSDK/macos/$lib" "$src"
  done
  echo "placeholder NeuralSDK/ (empty dylibs)"
fi
