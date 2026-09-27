#!/bin/sh
# Regenerates Engine/patches/onecore-engine.patch: our changes to the vendored OneCore engine (Engine/onecore) against a
# clean clone of github.com/KamiKitsune420/ms-david-zira-decomp at the pinned commit (Engine/ms-david-zira-decomp,
# gitignored). Every vendored file is compared; unchanged files produce no output; files that are ours alone (eva_*: Microsoft
# Eva) appear as new files (diff -N).
#   git clone https://github.com/KamiKitsune420/ms-david-zira-decomp Engine/ms-david-zira-decomp
#   git -C Engine/ms-david-zira-decomp checkout 3009a84894fdb37e1f808469cc97ab25006096e1
#   sh tools/onecore_patch.sh
set -eu
cd "$(dirname "$0")/.."
UP=Engine/ms-david-zira-decomp
test "$(git -C $UP rev-parse HEAD)" = 3009a84894fdb37e1f808469cc97ab25006096e1
out=Engine/patches/onecore-engine.patch
: > $out
for f in $(cd Engine/onecore && ls | sort); do
    if [ "$f" = LICENSE ]; then cmp -s $UP/LICENSE Engine/onecore/LICENSE || { echo "LICENSE differs" >&2; exit 1; }; continue; fi
    diff -uN --label "a/src/$f" --label "b/Engine/onecore/$f" "$UP/src/$f" "Engine/onecore/$f" >> $out || true
done
echo "$out: $(grep -c '^+++ ' $out) files changed"
