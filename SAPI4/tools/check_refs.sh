#!/bin/sh
# Re-render every tetyys.com reference in samples/ref (Microsoft) and samples/ref_tv (L&H TruVoice) and
# require bit-identical PCM.
# Reference files are named <mode>_<hello|long>.wav (mode lowercase, spaces -> _).
set -e
cd "$(dirname "$0")/.."
BIN=${BIN:-build/sapi4_speak}
HELLO="Hello, my name is Microsoft Sam."
LONG="The quick brown fox jumps over the lazy dog, and then it runs away into the forest."
fail=0
for ref in samples/ref/*.wav; do
  base=$(basename "$ref" .wav)
  kind=${base##*_}
  slug=${base%_*}
  case "$slug" in
    sam) mode="Sam" ;; mike) mode="Mike" ;; mary) mode="Mary" ;; robosoft_one) mode="RoboSoft One" ;;
    *) echo "skip $ref (unknown mode)"; continue ;;
  esac
  if [ "$kind" = hello ]; then text=$HELLO; else text=$LONG; fi
  out="samples/check_${base}.wav"
  timeout 120 "$BIN" -d data/msttsl -m "$mode" -o "$out" "$text" 2>/dev/null
  line=$(python3 tools/wavcmp.py "$out" "$ref")
  echo "$line"
  echo "$line" | grep -q "max|diff| 0 " || fail=1
done
# L&H TruVoice (tv_enua.dll): tetyys.com references fetched 2026-09-22 (samples/ref_tv/requests.log).
# tetyys sends tagged text; the app sends plain text - both must match. <file>|<mode>|<text>|<options>
FOX="The quick brown fox jumps over the lazy dog, and then it runs away into the forest."
NUM="On March 3rd, 2021, I paid \$45.99 for 3 items at 10:30 AM; call 555-1234. Dr. Smith's e-mail isn't working!"
TV="American English (TruVoice)"
while IFS='|' read -r file mode text opts; do
  [ -f "samples/ref_tv/$file" ] || { echo "missing samples/ref_tv/$file"; fail=1; continue; }
  case "$text" in FOX) t=$FOX ;; NUM) t=$NUM ;; esac
  for tag in -t ""; do
    out="samples/check_tv_${file%.wav}${tag:+_tagged}.wav"
    # shellcheck disable=SC2086
    timeout 120 "$BIN" -d data/tv_enua -m "$mode, $TV" $opts $tag -o "$out" "$t" 2>/dev/null
    line=$(python3 tools/wavcmp.py "$out" "samples/ref_tv/$file")
    echo "$line"
    echo "$line" | grep -q "max|diff| 0 " || fail=1
  done
done <<EOF2
adult_male_2_fox.wav|Adult Male #2|FOX|
adult_female_1_fox.wav|Adult Female #1|FOX|
adult_male_1_fox.wav|Adult Male #1|FOX|
adult_male_2_num.wav|Adult Male #2|NUM|
adult_male_2_fox_p140_s157.wav|Adult Male #2|FOX|-p 140 -s 157
EOF2
[ $fail = 0 ] && echo "ALL BIT-IDENTICAL" || { echo "MISMATCH"; exit 1; }
