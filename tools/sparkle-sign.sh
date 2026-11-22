#!/bin/bash
# Xcode build phase of the Mac app (project.yml), after Sparkle.framework and the login items are embedded (its input
# files make Xcode order it so), before Xcode signs the app itself, so the app's seal covers what this changes:
#   1. Sparkle's nested helpers (Autoupdate, Updater.app, the two XPC services) come ad-hoc signed in the Swift
#      package; Developer ID distribution and notarization need them signed with our identity, the hardened runtime
#      and a secure timestamp, inside out (Sparkle's documentation, "Code signing"). The app has no network access of
#      its own, so Downloader.xpc does the fetching (SUEnableDownloaderService) and is signed with its sandbox and
#      network entitlements (tools/sparkle-downloader.entitlements; the package's copy carries none).
#   2. Xcode also embeds the package in every login item that links it (Contents/Library/LoginItems/*.app); they use
#      the app's copy instead (their run path is @executable_path/../../../../../Frameworks), so that copy is removed
#      and the login item re-signed with its own entitlements. One Sparkle, one set of signed helpers.
# Skipped for unsigned or ad-hoc builds.
set -euo pipefail
ID="${EXPANDED_CODE_SIGN_IDENTITY:-}"
if [ -z "$ID" ] || [ "$ID" = "-" ] || [ "${CODE_SIGNING_ALLOWED:-YES}" = "NO" ]; then
  echo "sparkle-sign: no signing identity, leaving Sparkle as it is"; exit 0
fi
APP="$TARGET_BUILD_DIR/$WRAPPER_NAME"
F="$APP/Contents/Frameworks/Sparkle.framework"
[ -d "$F" ] || { echo "sparkle-sign: $F not found" >&2; exit 1; }
sign() { codesign -f -s "$ID" -o runtime --timestamp "$@"; }
sign "$F/Versions/B/XPCServices/Installer.xpc"
sign --entitlements "$SRCROOT/tools/sparkle-downloader.entitlements" "$F/Versions/B/XPCServices/Downloader.xpc"
sign "$F/Versions/B/Autoupdate"
sign "$F/Versions/B/Updater.app"
sign "$F"
for item in "$APP/Contents/Library/LoginItems/"*.app; do
  [ -d "$item" ] || continue
  if [ -d "$item/Contents/Frameworks/Sparkle.framework" ]; then
    rm -rf "$item/Contents/Frameworks/Sparkle.framework"
    rmdir "$item/Contents/Frameworks" 2>/dev/null || true
  fi
  sign --preserve-metadata=entitlements "$item"
done
echo "sparkle-sign: Sparkle.framework, its helpers and the login items signed with $ID"
