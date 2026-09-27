#!/bin/sh
# Renders samples/<slug>.wav for every SAPI 5 voice in Shared/Voices.swift, Microsoft Anna, and David / Zira / Mark
# (plain and emotion presets), through the same C
# bridges the app uses (build/mac/cv_test, build/mac/anna_test), so what you hear here is what the phone speaks at
# normal speed.
set -eu
cd "$(dirname "$0")/.."
mkdir -p samples
grep -E '^\s+ClassicVoiceDef\(slug: *"[a-z0-9_]+", *display:' Shared/Voices.swift |
sed -E 's/.*slug: "([^"]+)", *display: "([^"]+)", *spd: "([^"]+)", *effect: "([^"]+)", *basePitch: ([0-9.]+).*/\1|\2|\3|\4|\5/' |
while IFS='|' read -r slug display spd effect pitch; do
    name=$(printf '%s' "$display" | sed 's/^Microsoft //; s/ (SAPI 5)$//')
    printf '%-15s ' "$slug"
    ./build/mac/cv_test VoiceData say "$spd" "$effect" "$pitch" 0 0 \
        "This is $name, natively on iOS. The quick brown fox jumps over the lazy dog." "samples/$slug.wav"
done
# Microsoft Anna (Engine/anna + cva_bridge.c, data AnnaVoice/): build/mac/anna_test, same C as the phone
printf '%-15s ' anna
./build/mac/anna_test AnnaVoice say 0 0 \
    "This is Microsoft Anna, natively on iOS. The quick brown fox jumps over the lazy dog." samples/anna.wav
# Microsoft David, Zira and Mark, plain and in their emotion presets (Engine/onecore + cvo_bridge.c, data OneCoreVoice/):
# build/mac/onecore_test, same C as the phone. Slugs onecore_<voice>[_<emotion>] as in Shared/Voices.swift.
for voice in David Zira Mark; do
    for emotion in "" happy sad angry; do
        lower=$(printf '%s' "$voice" | tr 'A-Z' 'a-z')
        slug="onecore_$lower${emotion:+_$emotion}"
        name="Microsoft $voice${emotion:+ $(printf '%s' "$emotion" | awk '{print toupper(substr($0,1,1)) substr($0,2)}')}"
        printf '%-22s ' "$slug"
        ./build/mac/onecore_test OneCoreVoice say "$voice${emotion:+:$emotion}" 0 0 \
            "This is $name, natively on iOS. The quick brown fox jumps over the lazy dog." "samples/$slug.wav"
    done
done
