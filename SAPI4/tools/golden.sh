#!/bin/sh
# Interpreter regression: renders all 19 modes x 2 texts plus speed/pitch variants (46 WAVs) and
# compares the PCM hashes with tools/golden_hashes.txt, recorded through sapi4_tts on the step-1
# interpreter (bit-exact against the genuine engine, instruction-exact against Unicorn). The step-1
# driver itself gives the same 46 hashes except mary_in_hall_slowlow: it passed pitch 60 straight to
# the engine, while s4_set_pitch clamps to the engine's reported minimum (84 for Mary).
#   tools/golden.sh [binary]      exit 0 = all 46 identical (then the 27 TruVoice renders, below)
set -u
cd "$(dirname "$0")/.."
BIN=${1:-build/sapi4_speak}
OUT=samples/golden_check
rm -rf "$OUT" && mkdir -p "$OUT"
T1="The quick brown fox jumps over the lazy dog, and then it runs away into the forest."
T2="On March 3rd, 2021, I paid \$45.99 for 3 items at 10:30 AM; call 555-1234. Dr. Smith's e-mail isn't working!"
"$BIN" -d data/msttsl -l | sed -E 's/ +speaker=.*//' | while read -r mode; do
  slug=$(echo "$mode" | tr 'A-Z ' 'a-z_' | tr -d '()')
  timeout 120 "$BIN" -d data/msttsl -m "$mode" -o "$OUT/${slug}_1.wav" "$T1" 2>/dev/null
  timeout 120 "$BIN" -d data/msttsl -m "$mode" -o "$OUT/${slug}_2.wav" "$T2" 2>/dev/null
done
for mode in "Sam" "Mary in Hall" "Male Whisper" "Mike (for Telephone)"; do
  slug=$(echo "$mode" | tr 'A-Z ' 'a-z_' | tr -d '()')
  timeout 120 "$BIN" -d data/msttsl -m "$mode" -s 300 -p 140 -o "$OUT/${slug}_fastpitch.wav" "$T1" 2>/dev/null
  timeout 120 "$BIN" -d data/msttsl -m "$mode" -s 60 -p 60 -o "$OUT/${slug}_slowlow.wav" "$T1" 2>/dev/null
done
python3 tools/pcmhash.py "$OUT"/*.wav > "$OUT/hashes.txt"
if diff tools/golden_hashes.txt "$OUT/hashes.txt"; then echo "GOLDEN: all $(wc -l < tools/golden_hashes.txt | tr -d ' ') renders identical"; else echo "GOLDEN: MISMATCH"; exit 1; fi

# L&H TruVoice (tv_enua.dll): 10 modes x 2 texts, speed/pitch extremes on three voices, and the third
# utterance on one warm engine (the engine's state carries over, as in the app). Recorded 2026-09-22 on
# the interpreter that is bit-exact against every tetyys.com TruVoice reference (make check).
# GOLDEN_RECORD=1 tools/golden.sh  rewrites tools/golden_tv_hashes.txt instead of comparing.
OUT2=samples/golden_check_tv
rm -rf "$OUT2" && mkdir -p "$OUT2"
"$BIN" -d data/tv_enua -l | sed -E 's/ +speaker=.*//' | while read -r mode; do
  slug=$(echo "$mode" | sed -E 's/, American English \(TruVoice\)//' | tr 'A-Z ' 'a-z_' | tr -d '#')
  timeout 120 "$BIN" -d data/tv_enua -m "$mode" -o "$OUT2/${slug}_1.wav" "$T1" 2>/dev/null
  timeout 120 "$BIN" -d data/tv_enua -m "$mode" -o "$OUT2/${slug}_2.wav" "$T2" 2>/dev/null
done
for v in "Adult Male #2" "Adult Female #1" "Adult Male #6"; do
  slug=$(echo "$v" | tr 'A-Z ' 'a-z_' | tr -d '#')
  timeout 120 "$BIN" -d data/tv_enua -m "$v, American English (TruVoice)" -s 250 -p 300 -o "$OUT2/${slug}_fastpitch.wav" "$T1" 2>/dev/null
  timeout 120 "$BIN" -d data/tv_enua -m "$v, American English (TruVoice)" -s 50 -p 50 -o "$OUT2/${slug}_slowlow.wav" "$T1" 2>/dev/null
done
timeout 120 "$BIN" -d data/tv_enua -m "Adult Male #3, American English (TruVoice)" -r 3 -o "$OUT2/adult_male_3_warm3.wav" "$T2" 2>/dev/null
python3 tools/pcmhash.py "$OUT2"/*.wav > "$OUT2/hashes.txt"
if [ "${GOLDEN_RECORD:-0}" = 1 ]; then cp "$OUT2/hashes.txt" tools/golden_tv_hashes.txt; echo "GOLDEN TruVoice: recorded $(wc -l < tools/golden_tv_hashes.txt | tr -d ' ') hashes"; exit 0; fi
if diff tools/golden_tv_hashes.txt "$OUT2/hashes.txt"; then echo "GOLDEN TruVoice: all $(wc -l < tools/golden_tv_hashes.txt | tr -d ' ') renders identical"; else echo "GOLDEN TruVoice: MISMATCH"; exit 1; fi
