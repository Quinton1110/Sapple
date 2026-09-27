#!/bin/sh
# Real-time factor per mode family on one warm voice: P-cores (default QoS) and background QoS
# (taskpolicy -b: efficiency cores, throttled - the worst case a phone extension can see).
#   tools/bench.sh [seconds per run] [ms|tv|all]   (uses build/cv4_test; default: Microsoft only)
cd "$(dirname "$0")/.."
SECS=${1:-4}
WHICH=${2:-ms}
run() {  # dir mode
  p=$(timeout 300 ./build/cv4_test "$1" bench "$2" "$SECS" | sed -E 's/.* ([0-9.]+)x real time.*/\1/')
  e=$(timeout 300 /usr/sbin/taskpolicy -b ./build/cv4_test "$1" bench "$2" "$SECS" | sed -E 's/.* ([0-9.]+)x real time.*/\1/')
  printf '%-44s P-core %6.2fx   background/E-core %6.2fx\n' "$2" "$p" "$e"
}
if [ "$WHICH" != tv ]; then
  for m in "Sam" "Mike" "Mary" "Mike (for Telephone)" "Mary in Hall" "Mike in Stadium" "Mike in Space" "RoboSoft One" "Male Whisper"; do
    run data/msttsl "$m"
  done
fi
if [ "$WHICH" != ms ]; then
  for v in "Adult Male #1" "Adult Male #2" "Adult Male #3" "Adult Male #4" "Adult Male #5" "Adult Male #6" "Adult Male #7" "Adult Male #8" "Adult Female #1" "Adult Female #2"; do
    run data/tv_enua "$v, American English (TruVoice)"
  done
fi
