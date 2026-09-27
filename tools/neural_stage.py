#!/usr/bin/env python3
"""Stages the neural voices (Jenny, Aria, Guy, Sonia, Ryan) and Microsoft's embedded Speech SDK for the app:

  NeuralVoices/<Voice>/   the voice's own files (models, INIs, Tokens.xml, phones / punctuation tables)
  NeuralVoices/en-GB/     the en-GB voices' language data (identical in Sonia's and Ryan's packages; their 23 MB
                          MSTTSLocEnUS.dat is left out, see EN_GB_LEFT_OUT)
  NeuralVoices/model.key  the key the voice models are encrypted with
  NeuralSDK/ios/          the three SDK dylibs as the NeuralVoice app bundled them (arm64, iOS)
  NeuralSDK/macos/        the same three, re-tagged for macOS (vtool) and ad-hoc signed (Xcode signs them again)

Source: ~/code/NeuralVoice (Quinton's earlier neural voice app), or the folder given as the first argument:
  MicrosoftVoices/MicrosoftWindows.Voice.<locale>.<Voice>.1_<version>_x64__cw5n1h2txyewy/  - the Windows 11 Narrator
    natural-voice packages (Microsoft Store Appx), unpacked
  MicrosoftSpeechSDK/*.dylib                                  - Speech SDK 1.33 embedded TTS for iOS
  Extension/Bridge/MSTTSBridge.c                              - holds the model key (MSTTS_MODEL_KEY)

Checked before anything is copied (nothing is ever run):
  - each package's AppxSignature.p7x verifies (openssl smime -verify -noverify: the signature over its content; the
    chain is NOT validated to a trusted root) and its signer is CN=Microsoft Windows, O=Microsoft Corporation;
  - the signed content carries the SHA-256 of the package's AppxBlockMap.xml (after the "AXBM" tag);
  - every file staged from a package matches the SHA-256 its block map lists (b4:FileHash, else the 64 KiB blocks);
  - the files left to OneCoreVoice/ are byte-identical there (the en-US language data and domain files; five en-GB
    domain files), so the app bundles them once.
The Appx metadata (block map, signature, manifest) goes to _installers/neural/<package>/ (gitignored, not bundled).
Everything staged is Microsoft's: gitignored on the code-only branches, force-added on the full branches only.
Writes tools/neural_data.sha256 when run with --record; otherwise compares with it."""
import base64, glob, hashlib, os, re, shutil, subprocess, sys, xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOICES = [("Jenny", "en-US", "1033.INI"), ("Aria", "en-US", "1033.INI"), ("Guy", "en-US", "1033.INI"),
          ("Sonia", "en-GB", "2057.INI"), ("Ryan", "en-GB", "2057.INI")]
# name in the package -> name in OneCoreVoice/ (cvn_bridge.c links them under the package's name)
ONECORE_EN_US = {"MSTTSLocEnUS.dat": "MSTTSLocEnUS.dat", "EnUS.address.dat": "enUS.Address.dat",
                 "EnUS.companyname.dat": "enUS.CompanyName.dat", "EnUS.computer.dat": "enUS.Computer.dat",
                 "EnUS.media.dat": "enUS.Media.dat", "EnUS.message.dat": "enUS.Message.dat",
                 "EnUS.name.dat": "enUS.Name.dat"}
ONECORE_EN_GB = {"EnGB.address.dat": "EnGB.Address.dat", "EnGB.cityname.dat": "EnGB.CityName.dat",
                 "EnGB.companyname.dat": "EnGB.CompanyName.dat", "EnGB.computer.dat": "enGB.Computer.dat",
                 "EnGB.message.dat": "enGB.Message.dat"}
EN_GB_SHARED = ["MSTTSLocEnGB.dat", "EnGB.name.dat"]
# The en-GB packages also carry a 23 MB MSTTSLocEnUS.dat (not the en-US voices' one). Left out: measured 2026-09-27, 450
# lines of the OneCore upstream's corpora through Sonia and 200 through Ryan render bit-identical without it.
EN_GB_LEFT_OUT = ["MSTTSLocEnUS.dat"]
APPX_META = ["AppxBlockMap.xml", "AppxSignature.p7x", "AppxManifest.xml"]
SDK = ["libMicrosoft.CognitiveServices.Speech.core.dylib",
       "libMicrosoft.CognitiveServices.Speech.extension.embedded.tts.dylib",
       "libMicrosoft.CognitiveServices.Speech.extension.onnxruntime.dylib"]
BM = "{http://schemas.microsoft.com/appx/2010/blockmap}"
B4 = "{http://schemas.microsoft.com/appx/2021/blockmap}"


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def fail(msg):
    sys.exit("neural_stage: " + msg)


def check_package(pkg, scratch):
    """Signature -> block map -> every file. Returns {file name: sha256 hex} for the files the block map lists."""
    p7x = open(os.path.join(pkg, "AppxSignature.p7x"), "rb").read()
    if p7x[:4] != b"PKCX":
        fail(pkg + ": AppxSignature.p7x has no PKCX header")
    der = os.path.join(scratch, "sig.p7s")
    content = os.path.join(scratch, "sig.content")
    open(der, "wb").write(p7x[4:])
    r = subprocess.run(["openssl", "smime", "-verify", "-inform", "DER", "-in", der, "-noverify", "-out", content],
                       capture_output=True, text=True)
    if r.returncode != 0 or "Verification successful" not in r.stderr:
        fail(pkg + ": signature does not verify: " + r.stderr.strip())
    certs = subprocess.run(["openssl", "pkcs7", "-inform", "DER", "-in", der, "-print_certs", "-noout"],
                           capture_output=True, text=True).stdout
    if "O=Microsoft Corporation, CN=Microsoft Windows" not in certs:
        fail(pkg + ": signer is not Microsoft Windows")
    signed = open(content, "rb").read()
    bm_bytes = open(os.path.join(pkg, "AppxBlockMap.xml"), "rb").read()
    at = signed.find(b"AXBM")
    if at < 0 or signed[at + 4:at + 36] != hashlib.sha256(bm_bytes).digest():
        fail(pkg + ": the signature does not cover this AppxBlockMap.xml")
    out = {}
    for f in ET.fromstring(bm_bytes).iter(BM + "File"):
        name = f.get("Name").replace("\\", "/")
        path = os.path.join(pkg, name)
        if not os.path.isfile(path):
            continue
        data = open(path, "rb").read()
        if len(data) != int(f.get("Size")):
            fail(path + ": size differs from the block map")
        fh = f.find(B4 + "FileHash")
        if fh is not None:
            if hashlib.sha256(data).digest() != base64.b64decode(fh.get("Hash")):
                fail(path + ": SHA-256 differs from the block map")
        else:
            for i, b in enumerate(f.iter(BM + "Block")):
                if hashlib.sha256(data[i * 65536:(i + 1) * 65536]).digest() != base64.b64decode(b.get("Hash")):
                    fail(path + ": block %d differs from the block map" % i)
        out[name] = hashlib.sha256(data).hexdigest()
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    record = "--record" in sys.argv
    src = os.path.expanduser(args[0] if args else "~/code/NeuralVoice")
    onecore = os.path.join(ROOT, "OneCoreVoice")
    if not os.path.isfile(os.path.join(onecore, "MSTTSLocEnUS.dat")):
        fail("OneCoreVoice/ must be staged first (make onecore-data)")
    scratch = os.path.join(ROOT, "build", "neural-stage")
    os.makedirs(scratch, exist_ok=True)
    nv = os.path.join(ROOT, "NeuralVoices")
    shutil.rmtree(nv, ignore_errors=True)
    os.makedirs(os.path.join(nv, "en-GB"))
    en_gb = {}
    for name, locale, ini in VOICES:
        pkgs = glob.glob(os.path.join(src, "MicrosoftVoices", "MicrosoftWindows.Voice.%s.%s.1_*" % (locale, name)))
        if len(pkgs) != 1:
            fail("expected one package for %s, found %d" % (name, len(pkgs)))
        pkg = pkgs[0]
        listed = check_package(pkg, scratch)
        dst = os.path.join(nv, name)
        os.makedirs(dst)
        shared = ONECORE_EN_US if locale == "en-US" else ONECORE_EN_GB
        for f in sorted(os.listdir(pkg)):
            p = os.path.join(pkg, f)
            if not os.path.isfile(p) or f in APPX_META:
                continue
            if f not in listed:
                fail(p + ": not in the package's block map")
            if f in shared:
                if sha(os.path.join(onecore, shared[f])) != listed[f]:
                    fail("%s differs from OneCoreVoice/%s" % (p, shared[f]))
            elif locale == "en-GB" and f in EN_GB_LEFT_OUT:
                continue
            elif locale == "en-GB" and f in EN_GB_SHARED:
                if en_gb.setdefault(f, listed[f]) != listed[f]:
                    fail(p + ": differs from the other en-GB voice's copy")
                shutil.copyfile(p, os.path.join(nv, "en-GB", f))
            else:
                shutil.copyfile(p, os.path.join(dst, f))
        if not os.path.isfile(os.path.join(dst, ini)):
            fail(name + ": no " + ini)
        meta = os.path.join(ROOT, "_installers", "neural", os.path.basename(pkg))
        os.makedirs(meta, exist_ok=True)
        for f in APPX_META:
            shutil.copyfile(os.path.join(pkg, f), os.path.join(meta, f))
        print("%-6s %s: signature, block map and %d files verified" % (name, os.path.basename(pkg), len(listed)))
    bridge = open(os.path.join(src, "Extension", "Bridge", "MSTTSBridge.c")).read()
    m = re.search(r'#define MSTTS_MODEL_KEY\s*\\?\s*"([^"]+)"', bridge)
    if not m:
        fail("no MSTTS_MODEL_KEY in MSTTSBridge.c")
    open(os.path.join(nv, "model.key"), "w").write(m.group(1) + "\n")

    sdk = os.path.join(ROOT, "NeuralSDK")
    shutil.rmtree(sdk, ignore_errors=True)
    os.makedirs(os.path.join(sdk, "ios"))
    os.makedirs(os.path.join(sdk, "macos"))
    for f in SDK:
        shutil.copyfile(os.path.join(src, "MicrosoftSpeechSDK", f), os.path.join(sdk, "ios", f))
        mac = os.path.join(sdk, "macos", f)
        subprocess.run(["xcrun", "vtool", "-set-build-version", "macos", "15.0", "15.0", "-replace", "-output", mac,
                        os.path.join(sdk, "ios", f)], check=True, capture_output=True)
        subprocess.run(["codesign", "-f", "-s", "-", mac], check=True, capture_output=True)

    lines = []
    for base in ("NeuralVoices", "NeuralSDK"):
        for dirpath, _, files in sorted(os.walk(os.path.join(ROOT, base))):
            for f in sorted(files):
                p = os.path.join(dirpath, f)
                lines.append("%s  %s" % (sha(p), os.path.relpath(p, ROOT)))
    lines.sort(key=lambda l: l.split("  ", 1)[1])
    table = os.path.join(ROOT, "tools", "neural_data.sha256")
    if record:
        open(table, "w").write("\n".join(lines) + "\n")
        print("recorded %d hashes in tools/neural_data.sha256" % len(lines))
    else:
        want = open(table).read().split("\n")
        if [l for l in want if l] != lines:
            fail("staged files differ from tools/neural_data.sha256")
        print("all %d staged files match tools/neural_data.sha256" % len(lines))


if __name__ == "__main__":
    main()
