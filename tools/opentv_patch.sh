#!/bin/sh
# Regenerates Engine/patches/opentv-engine.patch: our changes to the vendored OpenTV engine (Engine/opentv) against a clean
# clone of github.com/RetroBunn/tv-decomp at the pinned commit (Engine/opentv-upstream, gitignored). Every vendored file is
# compared with the upstream file at the same path; generated/engine_struct.h against tools/gen_struct.py's output.
#   git clone https://github.com/RetroBunn/tv-decomp Engine/opentv-upstream
#   git -C Engine/opentv-upstream checkout b8897650b9e0dde14f95f67db3937e97858baa41
#   sh tools/opentv_patch.sh
set -eu
cd "$(dirname "$0")/.."
UP=Engine/opentv-upstream
test "$(git -C $UP rev-parse HEAD)" = b8897650b9e0dde14f95f67db3937e97858baa41
out=Engine/patches/opentv-engine.patch
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 $UP/tools/gen_struct.py $UP/src/engine.fields "$TMP/engine_struct.h" >/dev/null
cmp -s "$TMP/engine_struct.h" Engine/opentv/generated/engine_struct.h || { echo "generated/engine_struct.h differs" >&2; exit 1; }
: > $out
for f in $(cd Engine/opentv && find . -type f ! -path ./generated/\* | sed 's|^\./||' | sort); do
    diff -uN --label "a/$f" --label "b/Engine/opentv/$f" "$UP/$f" "Engine/opentv/$f" >> $out || true
done
echo "$out: $(grep -c '^+++ ' $out) files changed"
