#!/bin/bash
# Builds and publishes a Sapple release: the iOS IPA, the notarized Mac zip, the AltStore / SideStore source and the
# downloads page, in one go.
#
#   tools/release.sh VERSION "What's new" [--dry-run] [--skip-build] [--yes]
#   make release VERSION=1.1 NOTES="What's new"          (add DRYRUN=1 for a dry run)
#
#   VERSION        the new version, e.g. 1.1 (must be higher than every version already in the source)
#   "What's new"   one or two sentences; AltStore shows them with the update
#   --dry-run      build everything and write the new source.json and page locally (build/release/VERSION/), show what
#                  would change on the server, but notarize nothing, upload nothing and leave project.yml as it was
#   --skip-build   reuse the IPA and Mac zip already in build/release/VERSION/ (to resume after a failed upload)
#   --yes          publish without asking at the confirmation prompt
#
# What it does:
#   1. Checks: the voice data folders are present, the tree has no uncommitted changes to tracked files, the server is
#      reachable, and VERSION is not already in the live source.json and is higher than its newest entry.
#   2. Sets MARKETING_VERSION = VERSION and CURRENT_PROJECT_VERSION = one more than the highest build number in
#      project.yml or the source, in project.yml.
#   3. Builds the iOS app unsigned, ad-hoc signs app and extension WITH their entitlements (so AltStore sees the app
#      group), zips Payload/ into Sapple-VERSION.ipa.
#   4. Builds the Mac app (Developer ID, arm64), checks its entitlements, notarizes, staples, zips Sapple-VERSION-mac.zip.
#   5. Reads the version and build back OUT of both built Info.plists and stops if they differ from what was asked for,
#      so the source can never disagree with the app.
#   6. Writes the new source.json (the new entry on top of `versions`; older entries and every other field kept) and the
#      downloads page (links, sizes, versions, SHA-256), validates both.
#   7. Backs up source.json and index.html on the server, uploads the IPA and zip (to temporary names, hash-checked, then
#      renamed into place), then the source and the page the same way. Old IPAs stay, so older entries keep working.
#   8. Fetches the public source.json and checks the new entry, then downloads the IPA and checks its size and SHA-256.
#   9. Updates the download links in README.md. Commit project.yml and README.md afterwards.
#
# Settings come from tools/release.local (gitignored; see the list below) or the environment:
#   RELEASE_SSH         ssh destination of the web server (e.g. an ssh alias)
#   RELEASE_DIR         folder on the server that holds the downloads (e.g. /var/www/html/sapple)
#   RELEASE_URL         public URL of that folder, ending in / (e.g. https://example.com/sapple/)
#   RELEASE_BACKUP_DIR  folder on the server for backups of source.json / index.html (default ~/sapple-backups)
#   NOTARY_KEY          path to the App Store Connect API key (.p8) used for notarization
#   NOTARY_KEY_ID       its key ID
#   NOTARY_ISSUER       its issuer ID
#
# Not done yet (future step): Sparkle updates for the Mac app. Mac users download the new zip from the page.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

usage() { sed -n '5,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

VERSION="" NOTES="" DRY=0 SKIPBUILD=0 YES=0
for a in "$@"; do
  case "$a" in
    --dry-run) DRY=1 ;;
    --skip-build) SKIPBUILD=1 ;;
    --yes) YES=1 ;;
    -h|--help) usage ;;
    -*) echo "release: unknown option $a" >&2; usage ;;
    *) if [ -z "$VERSION" ]; then VERSION="$a"; elif [ -z "$NOTES" ]; then NOTES="$a"; else usage; fi ;;
  esac
done
[ -n "$VERSION" ] && [ -n "$NOTES" ] || usage
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || { echo "release: VERSION must look like 1.1 or 1.1.2" >&2; exit 2; }

[ -f tools/release.local ] && . tools/release.local
: "${RELEASE_SSH:?set RELEASE_SSH in tools/release.local}"
: "${RELEASE_DIR:?set RELEASE_DIR in tools/release.local}"
: "${RELEASE_URL:?set RELEASE_URL in tools/release.local}"
RELEASE_BACKUP_DIR="${RELEASE_BACKUP_DIR:-sapple-backups}"
case "$RELEASE_URL" in */) ;; *) RELEASE_URL="$RELEASE_URL/" ;; esac

OUT="build/release/$VERSION"
IPA="Sapple-$VERSION.ipa"
MACZIP="Sapple-$VERSION-mac.zip"
mkdir -p "$OUT"
LOG="$OUT/release.log"
say() { echo "== $*" | tee -a "$LOG"; }
die() { echo "release: $*" | tee -a "$LOG" >&2; exit 1; }
remote() { ssh -o BatchMode=yes -o ConnectTimeout=15 "$RELEASE_SSH" "$@"; }

# ---------------------------------------------------------------------------------------------------- 1. checks
say "Sapple $VERSION ($([ $DRY = 1 ] && echo dry run || echo release)) $(date)"
for d in VoiceData SAPI4Voices AnnaVoice OneCoreVoice TruVoiceData NeuralVoices NeuralSDK; do
  [ -d "$d" ] && [ -n "$(ls -A "$d")" ] || die "$d/ is missing or empty: a release needs every voice data folder (README, Building from source)"
done
[ -f "$(ls TruVoiceData/*.s 2>/dev/null | head -1)" ] || die "TruVoiceData/ has no tables (make truvoice-data)"
if [ $DRY = 0 ] && [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  die "tracked files have uncommitted changes; commit or stash them first (git status)"
fi
for t in xcodegen xcodebuild python3 ditto codesign curl; do command -v $t >/dev/null || die "$t not found"; done
if [ $DRY = 0 ]; then
  : "${NOTARY_KEY:?set NOTARY_KEY in tools/release.local}" "${NOTARY_KEY_ID:?}" "${NOTARY_ISSUER:?}"
  [ -f "$NOTARY_KEY" ] || die "NOTARY_KEY $NOTARY_KEY not found"
fi

remote "test -d '$RELEASE_DIR' && test -f '$RELEASE_DIR/source.json' && test -f '$RELEASE_DIR/index.html'" \
  || die "cannot reach $RELEASE_SSH:$RELEASE_DIR/{source.json,index.html}"
remote "cat '$RELEASE_DIR/source.json'" > "$OUT/source.old.json"
remote "cat '$RELEASE_DIR/index.html'" > "$OUT/index.old.html"

NEWBUILD=$(python3 - "$OUT/source.old.json" "$VERSION" <<'EOF'
import json, re, sys
src = json.load(open(sys.argv[1])); want = sys.argv[2]
v = lambda s: tuple(int(x) for x in s.split("."))
app = src["apps"][0]
versions = app.get("versions", [])
if any(e.get("version") == want for e in versions) or app.get("version") == want:
    sys.exit("release: version %s is already in the source; pick a new version" % want)
if versions and v(want) <= v(versions[0]["version"]):
    sys.exit("release: %s is not higher than the newest version in the source (%s)" % (want, versions[0]["version"]))
builds = [int(e.get("buildVersion", 0)) for e in versions]
yml = open("project.yml").read()
m = re.search(r'CURRENT_PROJECT_VERSION:\s*"?(\d+)"?', yml)
builds.append(int(m.group(1)) if m else 0)
print(max(builds) + 1)
EOF
) || exit 1
say "version $VERSION, build $NEWBUILD"

# ---------------------------------------------------------------------------------------------------- 2. bump
cp project.yml "$OUT/project.yml.before"
restore_yml() { cp "$OUT/project.yml.before" project.yml; }
[ $DRY = 1 ] && trap restore_yml EXIT
python3 - "$VERSION" "$NEWBUILD" <<'EOF'
import re, sys
y = open("project.yml").read()
y, a = re.subn(r'(MARKETING_VERSION:\s*)"[^"]*"', r'\g<1>"%s"' % sys.argv[1], y, count=1)
y, b = re.subn(r'(CURRENT_PROJECT_VERSION:\s*)"[^"]*"', r'\g<1>"%s"' % sys.argv[2], y, count=1)
if a != 1 or b != 1:
    sys.exit("release: MARKETING_VERSION / CURRENT_PROJECT_VERSION not found in project.yml")
open("project.yml", "w").write(y)
EOF

# ---------------------------------------------------------------------------------------------------- 3-4. build
build_ok() { grep -qE '\*\* BUILD SUCCEEDED \*\*' "$1" || die "build failed, see $1"; }
if [ $SKIPBUILD = 1 ]; then
  [ -f "$OUT/$IPA" ] && [ -f "$OUT/$MACZIP" ] || die "--skip-build: $OUT/$IPA and $OUT/$MACZIP must exist"
  say "reusing $OUT/$IPA and $OUT/$MACZIP"
else
  xcodegen generate >> "$LOG" 2>&1 || die "xcodegen failed"

  say "iOS build (unsigned, then ad-hoc signed with entitlements)"
  rm -rf build/release/ios-dd "$OUT/Payload" "$OUT/$IPA"
  xcodebuild -project ClassicVoices.xcodeproj -scheme ClassicVoices -configuration Release \
    -destination 'generic/platform=iOS' -derivedDataPath build/release/ios-dd CODE_SIGNING_ALLOWED=NO build \
    > "$OUT/xcodebuild-ios.log" 2>&1 || true
  build_ok "$OUT/xcodebuild-ios.log"
  mkdir -p "$OUT/Payload"
  cp -Rp build/release/ios-dd/Build/Products/Release-iphoneos/Sapple.app "$OUT/Payload/"
  codesign -f -s - --entitlements Extension/Ext.entitlements "$OUT/Payload/Sapple.app/PlugIns/ClassicVoicesExtension.appex" >> "$LOG" 2>&1
  codesign -f -s - --entitlements App/App.entitlements "$OUT/Payload/Sapple.app" >> "$LOG" 2>&1
  for b in "$OUT/Payload/Sapple.app" "$OUT/Payload/Sapple.app/PlugIns/ClassicVoicesExtension.appex"; do
    codesign -d --entitlements - "$b" 2>/dev/null | grep -q "group.com.quinton.classicvoices" || die "$b: app group missing from its entitlements"
  done
  (cd "$OUT" && ditto -c -k --norsrc --keepParent Payload "$IPA")
  rm -rf "$OUT/Payload"

  say "Mac build (Developer ID, arm64)"
  rm -rf build/release/mac-dd "$OUT/$MACZIP"
  xcodebuild -project ClassicVoices.xcodeproj -scheme ClassicVoices-macOS -configuration Release \
    -destination 'platform=macOS,arch=arm64' -derivedDataPath build/release/mac-dd build \
    > "$OUT/xcodebuild-mac.log" 2>&1 || true
  build_ok "$OUT/xcodebuild-mac.log"
  MACAPP=build/release/mac-dd/Build/Products/Release/Sapple.app
  codesign --verify --deep --strict "$MACAPP" || die "Mac app does not verify"
  if codesign -d --entitlements - "$MACAPP" 2>/dev/null | grep -q get-task-allow; then
    die "Mac app carries get-task-allow; the notary service would reject it"
  fi
  if [ $DRY = 1 ]; then
    say "dry run: not notarizing"
  else
    say "notarizing (can take a few minutes)"
    ditto -c -k --keepParent "$MACAPP" "$OUT/notarize.zip"
    xcrun notarytool submit "$OUT/notarize.zip" --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID" \
      --issuer "$NOTARY_ISSUER" --wait --timeout 30m > "$OUT/notarize.log" 2>&1 || true
    cat "$OUT/notarize.log" >> "$LOG"
    grep -q "status: Accepted" "$OUT/notarize.log" || die "notarization was not accepted, see $OUT/notarize.log"
    rm -f "$OUT/notarize.zip"
    xcrun stapler staple "$MACAPP" >> "$LOG" 2>&1 || die "stapling failed"
    xcrun stapler validate "$MACAPP" >> "$LOG" 2>&1 || die "staple does not validate"
    /usr/sbin/spctl -a -vvv -t exec "$MACAPP" 2>&1 | tee -a "$LOG" | grep -q "source=Notarized Developer ID" \
      || die "Gatekeeper does not accept the Mac app"
  fi
  ditto -c -k --keepParent "$MACAPP" "$OUT/$MACZIP"
fi

# ---------------------------------------------------------------------------------------------------- 5. read back
python3 - "$OUT/$IPA" "$OUT/$MACZIP" "$VERSION" "$NEWBUILD" > "$OUT/built.env" <<'EOF' || exit 1
import plistlib, sys, zipfile
ipa, maczip, want_v, want_b = sys.argv[1:]
def info(zpath, member):
    return plistlib.loads(zipfile.ZipFile(zpath).read(member))
i = info(ipa, "Payload/Sapple.app/Info.plist")
m = info(maczip, "Sapple.app/Contents/Info.plist")
for name, p in (("IPA", i), ("Mac zip", m)):
    got = (p["CFBundleShortVersionString"], p["CFBundleVersion"])
    if got != (want_v, want_b):
        sys.exit("release: the %s says version %s build %s, expected %s build %s" % (name, got[0], got[1], want_v, want_b))
print("BUILT_VERSION=%s" % i["CFBundleShortVersionString"])
print("BUILT_BUILD=%s" % i["CFBundleVersion"])
print("MIN_IOS=%s" % i.get("MinimumOSVersion", "18.0"))
EOF
. "$OUT/built.env"
say "built: version $BUILT_VERSION build $BUILT_BUILD (read from both Info.plists), iOS $MIN_IOS+"

# ---------------------------------------------------------------------------------------------------- 6. source + page
python3 - "$OUT" "$IPA" "$MACZIP" "$BUILT_VERSION" "$BUILT_BUILD" "$MIN_IOS" "$NOTES" "$RELEASE_URL" <<'EOF' || exit 1
import datetime, hashlib, json, os, re, sys
out, ipa, maczip, ver, build, minos, notes, url = sys.argv[1:]
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()
ipa_size, mac_size = os.path.getsize(os.path.join(out, ipa)), os.path.getsize(os.path.join(out, maczip))
ipa_sha, mac_sha = sha(os.path.join(out, ipa)), sha(os.path.join(out, maczip))
now = datetime.datetime.now().astimezone().replace(microsecond=0).isoformat()

src = json.load(open(os.path.join(out, "source.old.json")))
app = src["apps"][0]
entry = {"version": ver, "buildVersion": build, "date": now, "localizedDescription": notes,
         "downloadURL": url + ipa, "size": ipa_size, "sha256": ipa_sha, "minOSVersion": minos}
app["versions"] = [entry] + app.get("versions", [])
# the older single-version fields, if the source carries them
for k, val in (("version", ver), ("versionDate", now), ("versionDescription", notes), ("downloadURL", url + ipa),
               ("size", ipa_size)):
    if k in app:
        app[k] = val
text = json.dumps(src, indent=2, ensure_ascii=False) + "\n"
chk = json.loads(text)["apps"][0]
assert chk["versions"][0]["version"] == ver and chk["versions"][0]["size"] == ipa_size
for k in ("name", "bundleIdentifier", "developerName", "iconURL", "localizedDescription", "versions"):
    assert k in chk, "source.json lost " + k
for e in chk["versions"]:
    for k in ("version", "date", "downloadURL", "size"):
        assert k in e, "a versions entry lacks " + k
open(os.path.join(out, "source.json"), "w").write(text)

page = open(os.path.join(out, "index.old.html")).read()
cut = page.index('<h2 id="mac">')
ios, mac = page[:cut], page[cut:]
def mb(n): return "%d MB (%s bytes)" % (round(n / 1e6), format(n, ","))
def sub(pat, rep, s, what, n=1):
    s2, k = re.subn(pat, rep, s)
    if k != n:
        sys.exit("release: downloads page: expected %d %s, found %d" % (n, what, k))
    return s2
ios = sub(r'href="Sapple-[0-9.]+\.ipa"', 'href="%s"' % ipa, ios, "IPA link")
ios = sub(r'Download Sapple [0-9.]+ for iPhone', 'Download Sapple %s for iPhone' % ver, ios, "IPA button")
ios = sub(r'<dd>Sapple-[0-9.]+\.ipa, [^<]*</dd>', '<dd>%s, %s</dd>' % (ipa, mb(ipa_size)), ios, "IPA file line")
ios = sub(r'<dt>Version</dt><dd>[0-9.]+, build \d+\.', '<dt>Version</dt><dd>%s, build %s.' % (ver, build), ios, "IPA version line")
ios = sub(r'(<dt>SHA-256</dt><dd><code>)[0-9a-f]{64}', r'\g<1>' + ipa_sha, ios, "IPA SHA-256")
mac = sub(r'href="Sapple-[0-9.]+-mac\.zip"', 'href="%s"' % maczip, mac, "Mac link")
mac = sub(r'Download Sapple [0-9.]+ for Mac', 'Download Sapple %s for Mac' % ver, mac, "Mac button")
mac = sub(r'<dd>Sapple-[0-9.]+-mac\.zip, [^<]*</dd>', '<dd>%s, %s</dd>' % (maczip, mb(mac_size)), mac, "Mac file line")
mac = sub(r'<dt>Version</dt><dd>[0-9.]+, build \d+\.', '<dt>Version</dt><dd>%s, build %s.' % (ver, build), mac, "Mac version line")
mac = sub(r'(<dt>SHA-256</dt><dd><code>)[0-9a-f]{64}', r'\g<1>' + mac_sha, mac, "Mac SHA-256")
open(os.path.join(out, "index.html"), "w").write(ios + mac)
open(os.path.join(out, "artifacts.env"), "w").write(
    "IPA_SIZE=%d\nIPA_SHA=%s\nMAC_SIZE=%d\nMAC_SHA=%s\n" % (ipa_size, ipa_sha, mac_size, mac_sha))
EOF
. "$OUT/artifacts.env"

echo; echo "---- source.json changes"; diff "$OUT/source.old.json" "$OUT/source.json" || true
echo "---- downloads page changes"; diff "$OUT/index.old.html" "$OUT/index.html" || true
cat <<EOF

About to publish Sapple $VERSION (build $NEWBUILD) to $RELEASE_SSH:$RELEASE_DIR
  $IPA      $IPA_SIZE bytes  sha256 $IPA_SHA
  $MACZIP  $MAC_SIZE bytes  sha256 $MAC_SHA
  What's new: $NOTES
  Public: ${RELEASE_URL}source.json
EOF
if [ $DRY = 1 ]; then
  say "dry run: nothing uploaded; files in $OUT; project.yml restored"
  exit 0
fi
if [ $YES = 0 ]; then
  read -r -p "Publish? [y/N] " ans
  [ "$ans" = y ] || [ "$ans" = Y ] || die "not published"
fi

# ---------------------------------------------------------------------------------------------------- 7. upload
STAMP=$(date +%Y%m%d-%H%M%S)
say "backing up the live source.json and index.html to $RELEASE_BACKUP_DIR/ on the server"
remote "mkdir -p $RELEASE_BACKUP_DIR && cp -p '$RELEASE_DIR/source.json' $RELEASE_BACKUP_DIR/source.json.$STAMP && cp -p '$RELEASE_DIR/index.html' $RELEASE_BACKUP_DIR/index.html.$STAMP"
put() {  # put LOCAL NAME SHA256: upload to .NAME.tmp, check its hash, rename into place
  scp -q -o BatchMode=yes "$1" "$RELEASE_SSH:$RELEASE_DIR/.$2.tmp" || die "upload of $2 failed"
  remote "cd '$RELEASE_DIR' && echo '$3  .$2.tmp' | sha256sum -c --quiet - && chmod 644 '.$2.tmp' && mv -f '.$2.tmp' '$2'" \
    || die "$2: hash on the server does not match"
}
hash_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
say "uploading $IPA"; put "$OUT/$IPA" "$IPA" "$IPA_SHA"
say "uploading $MACZIP"; put "$OUT/$MACZIP" "$MACZIP" "$MAC_SHA"
say "uploading source.json and index.html"
put "$OUT/source.json" source.json "$(hash_of "$OUT/source.json")"
put "$OUT/index.html" index.html "$(hash_of "$OUT/index.html")"

# ---------------------------------------------------------------------------------------------------- 8. verify
say "verifying the public source and the IPA download"
curl -fsS -H 'Cache-Control: no-cache' "${RELEASE_URL}source.json?v=$STAMP" -o "$OUT/source.live.json" || die "cannot fetch the public source.json"
python3 - "$OUT/source.live.json" "$VERSION" "$NEWBUILD" "${RELEASE_URL}$IPA" "$IPA_SIZE" <<'EOF' || exit 1
import json, sys
e = json.load(open(sys.argv[1]))["apps"][0]["versions"][0]
want = (sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
got = (e["version"], e["buildVersion"], e["downloadURL"], e["size"])
if got != want:
    sys.exit("release: the public source's newest entry is %s, expected %s" % (got, want))
EOF
curl -fsS "${RELEASE_URL}$IPA" -o "$OUT/downloaded.ipa" || die "cannot download ${RELEASE_URL}$IPA"
[ "$(stat -f %z "$OUT/downloaded.ipa")" = "$IPA_SIZE" ] || die "downloaded IPA size differs from the source's"
[ "$(hash_of "$OUT/downloaded.ipa")" = "$IPA_SHA" ] || die "downloaded IPA SHA-256 differs"
rm -f "$OUT/downloaded.ipa"
curl -fsS "${RELEASE_URL}" | grep -q "Download Sapple $VERSION for iPhone" || die "the downloads page does not show $VERSION"
say "live: source.json lists $VERSION build $NEWBUILD; the IPA downloads with $IPA_SIZE bytes and the right SHA-256"

# ---------------------------------------------------------------------------------------------------- 9. README
if [ -f README.md ]; then
  sed -i '' -E "s/Sapple-[0-9.]+\.ipa/$IPA/g; s/Sapple-[0-9.]+-mac\.zip/$MACZIP/g" README.md
fi
say "done. Commit project.yml (and README.md): git commit -am \"Sapple $VERSION\""
