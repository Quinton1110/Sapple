#!/bin/sh
# Generate TruVoiceData/tvdata.s: OpenTV's engine tables (Centigram's, from the pinned upstream's data/en/engine.tvdata)
# laid out as an assembly data image for the iOS / macOS build. `make truvoice-data` runs it.
#
# The tables are VOICE DATA: TruVoiceData/ is gitignored on main / mac and force-added on the two full-voices
# branches only. The engine's C (Engine/opentv) refers to each table by
# name; upstream's tools/gen_data.py reads which names the compiled objects need and writes an image that keeps every
# byte at the offset the original had (the engine reads past the end of some tables into the next), then
# tools/opentv_macho_asm.py makes it Mach-O. Deterministic: the result is checked against tools/truvoice_data.sha256.
set -e
cd "$(dirname "$0")/.."
UP=Engine/opentv-upstream
PIN=b8897650b9e0dde14f95f67db3937e97858baa41
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
mkdir -p "$TMP/obj" "$TMP/py"
for f in Engine/opentv/src/engine/*.c Engine/opentv/src/port/*.c; do
  clang -c -std=gnu11 -O2 -w -fwrapv -fno-strict-aliasing -DCV_NO_DEBUG_ENV -IEngine/opentv/src -IEngine/opentv/include \
    -IEngine/opentv/generated "$f" -o "$TMP/obj/$(basename "$f" .c).o"
done
# gen_data imports pefile at the top for the DLL path, which reading a .tvdata never touches.
echo "# stub: only the .tvdata path of gen_data.py is used" > "$TMP/py/pefile.py"
mkdir -p TruVoiceData
PYTHONPATH="$TMP/py" python3 "$UP/tools/gen_data.py" "$UP/data/en/engine.tvdata" Engine/opentv/src TruVoiceData/tvdata.s \
  "$TMP"/obj/*.o
python3 tools/opentv_macho_asm.py TruVoiceData/tvdata.s
(cd TruVoiceData && shasum -a 256 -c ../tools/truvoice_data.sha256 2>/dev/null | grep tvdata.s) ||
  { echo "TruVoiceData/tvdata.s does not match tools/truvoice_data.sha256 (engine sources changed? re-record it)" >&2; exit 1; }
ls -la TruVoiceData
