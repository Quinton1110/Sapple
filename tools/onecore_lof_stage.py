#!/usr/bin/env python3
"""Stages the other English OneCore voices into OneCoreVoice/: Microsoft Catherine and James (en-AU), Linda and Richard
(en-CA), with their language data and domain files, and (2026-09-30) Microsoft Matilda, the hidden en-AU neural voice.

Source: Microsoft's own server, the Windows 11 24H2 "Languages and Optional Features" image (LOF, 26100.1):
  https://software-static.download.prss.microsoft.com/dbazure/888969d5-f34g-4e03-ac9d-1f9786c66749/26100.1.240331-1435.ge_release_amd64fre_CLIENT_LOF_PACKAGES_OEM.iso
  (6,251,841,536 bytes = the server's Content-Length), kept in _installers/lof/ with source_url.txt, headers.txt and
  iso.sha256. Read with 7-Zip only, nothing run: LanguagesAndOptionalFeatures/Microsoft-Windows-LanguageFeatures-
  TextToSpeech-<locale>-Package~31bf3856ad364e35~amd64~~.cab, then its amd64_microsoft-windows-t..peech-<locale>-onecore_*
  folder (the files Windows installs to Windows/Speech_OneCore/Engines/TTS/<locale>).

What is taken, and how it is named in OneCoreVoice/ (the engine opens these names; iOS is case-sensitive):
  - per voice M<lcid><Voice>.APM, .BEP where one ships (James, Richard), .INI;
  - the locale's MSTTSLoc<xx>.DAT as MSTTSLoc<xx>.dat, and the domain files its MSTTSLoc<xx>.INI [Domain] lists, named as
    that INI names them (zf1_dat.c DOM_ENAU / DOM_ENCA).
  The INIs of these two locales are UTF-16 (Windows reads either); the engine's INI readers read 8-bit text, so they are
  staged as the same text in ASCII - the ONLY change to any file (their hashes differ from the image's; both are recorded
  below). Left out: the HMM voices' .HEQ files (log-F0 equalisation: not read by the reconstructed engine for them), the
  keyboard / NUS clips (tagged domains only), and the hidden en-CA Eva.
  - Matilda (en-AU neural, like Sarah): her six voice files M3081Matilda.{APM,BEP,HEQ,INI,nnm,tdat} with the package's own
    mixed-case names (eva_voice.c accepts either case), byte-identical except the INI (UTF-16 -> ASCII as above); her
    .HEQ IS read (the neural back end loads it). Left out: M3081Matilda.{voiceAssistant,tbtdirection}.* (recorded clips for
    the two tagged domains, 17 MB), as for Sarah. She uses MSTTSLocEnAU.dat and the en-AU domain files staged here.
Not staged, because the engine cannot speak them yet: Heera and Ravi (en-IN: 40th-order LSF models with multi-band
excitation models) and Sean (en-IE: multi-band excitation, an extended acoustic model M6153Sean.atm and the APM feature
Phone.Syllable.Accent) - none of that is reconstructed.

  tools/onecore_lof_stage.py [LOF.iso]      extract, check and copy; then run `make onecore-data`
"""
import hashlib, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(ROOT, "_installers", "lof", "LOF.iso")
ISO_SHA = "see _installers/lof/iso.sha256"
LOCALES = {
    "en-au": ("3081", "EnAU", {"Catherine": False, "James": True}),
    "en-ca": ("4105", "EnCA", {"Linda": False, "Richard": True}),
}
# the neural voices: their files, named exactly as in the package (only the INI is converted)
NEURAL = {"en-au": {"Matilda": ("APM", "BEP", "HEQ", "INI", "nnm", "tdat")}}
# name in the package -> name in OneCoreVoice/ (the INI's own spelling)
DOMAINS = {
    "en-au": {"EnAU.Name.DAT": "EnAU.Name.dat", "EnAU.Message.DAT": "EnAU.Message.dat",
              "EnAU.Computer.DAT": "EnAU.Computer.dat", "EnAU.Address.DAT": "EnAU.Address.dat",
              "EnAU.CompanyName.DAT": "EnAU.CompanyName.dat", "EnAU.CityName.DAT": "EnAU.CityName.dat"},
    "en-ca": {"EnCA.Address.DAT": "enCA.Address.dat", "EnCA.Name.DAT": "enCA.Name.dat",
              "EnCA.Message.DAT": "enCA.Message.dat", "EnCA.Computer.DAT": "enCA.Computer.dat",
              "EnCA.Media.DAT": "enCA.Media.dat", "EnCA.CompanyName.DAT": "enCA.CompanyName.dat"},
}


def sha(b):
    return hashlib.sha256(b).hexdigest()


def fail(msg):
    sys.exit("onecore_lof_stage: " + msg)


def ascii_ini(b):
    if b[:2] == b"\xff\xfe":
        return b[2:].decode("utf-16-le").encode("ascii")
    return b


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else ISO
    dst = os.path.join(ROOT, "OneCoreVoice")
    if not os.path.isfile(os.path.join(dst, "MSTTSLocEnUS.dat")):
        fail("OneCoreVoice/ must be staged first")
    tmp = tempfile.mkdtemp(prefix="lof-", dir=os.path.join(ROOT, "build") if os.path.isdir(os.path.join(ROOT, "build")) else None)
    try:
        for loc, (lcid, tag, voices) in LOCALES.items():
            cab = "LanguagesAndOptionalFeatures/Microsoft-Windows-LanguageFeatures-TextToSpeech-%s-Package~31bf3856ad364e35~amd64~~.cab" % loc
            subprocess.run(["7z", "e", "-y", "-o" + tmp, iso, cab], check=True, capture_output=True)
            x = os.path.join(tmp, loc)
            subprocess.run(["7z", "x", "-y", "-o" + x, os.path.join(tmp, os.path.basename(cab))], check=True, capture_output=True)
            src = [os.path.join(x, d) for d in os.listdir(x) if d.startswith("amd64_") and "-onecore_" in d
                   and os.path.isdir(os.path.join(x, d))]
            if len(src) != 1:
                fail("%s: expected one onecore folder in the package" % loc)
            src = src[0]
            files = {f.lower(): f for f in os.listdir(src)}
            plan = []  # (package name, staged name, convert)
            for v in voices:
                for ext in ("APM", "BEP", "INI"):
                    f = files.get(("M%s%s.%s" % (lcid, v, ext)).lower())
                    if f is None:
                        if ext == "BEP":
                            continue
                        fail("%s: no M%s%s.%s" % (loc, lcid, v, ext))
                    plan.append((f, "M%s%s.%s" % (lcid, v, ext), ext == "INI"))
            for v, exts in NEURAL.get(loc, {}).items():
                for ext in exts:
                    f = files.get(("M%s%s.%s" % (lcid, v, ext)).lower()) or fail("%s: no M%s%s.%s" % (loc, lcid, v, ext))
                    if f != "M%s%s.%s" % (lcid, v, ext):
                        fail("%s: %s is named %s in the package" % (loc, "M%s%s.%s" % (lcid, v, ext), f))
                    plan.append((f, f, ext == "INI"))
            f = files.get(("MSTTSLoc%s.DAT" % tag).lower()) or fail("%s: no language data" % loc)
            plan.append((f, "MSTTSLoc%s.dat" % tag, False))
            for pkg_name, staged in DOMAINS[loc].items():
                f = files.get(pkg_name.lower()) or fail("%s: no %s" % (loc, pkg_name))
                plan.append((f, staged, False))
            for f, staged, conv in plan:
                b = open(os.path.join(src, f), "rb").read()
                out = ascii_ini(b) if conv else b
                open(os.path.join(dst, staged), "wb").write(out)
                print("%-24s %10d  %s%s" % (staged, len(out), sha(b)[:16],
                                            ("  (UTF-16 -> ASCII, staged %s)" % sha(out)[:16]) if out != b else ""))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("staged; now: make onecore-data (hashes in tools/onecore_data.sha256)")


if __name__ == "__main__":
    main()
