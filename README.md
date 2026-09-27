# Sapple

The Microsoft-era speech voices, from SAM through the Windows 11 natural voices, as system voices on iPhone, iPad and Mac.

## About

Sapple puts the old and not-so-old Windows voices on Apple devices as real system voices, so they work with VoiceOver, Spoken Content and any app that uses the system speech voices. That covers the SAPI 4 and SAPI 5 voices (Sam, Mike, Mary and the rest), the TruVoice voices from Microsoft Agent, Anna from Windows Vista and 7, the Windows 10 and 11 OneCore voices, and the Windows 11 natural voices. Everything runs on the device and works offline.

Most of the engines are C reconstructions of the original Windows engines, which their authors checked against the originals down to the sample. The natural voices run on Microsoft's own Speech SDK. The voice data itself is not in this repository (see About the voices).

## Just want to use it?

Pre-built downloads are coming soon: an IPA for the iPhone and iPad, an AltStore source, and a Mac app. Until then you'll need to build it yourself (see below). Once there's an IPA, either of these will install it.

AltStore (stays installed, refreshes itself over Wi-Fi):

1. Install AltServer on your Mac or PC from [altstore.io](https://altstore.io/), then use it to put AltStore on your iPhone.
2. Open AltStore on your iPhone, go to My Apps, tap the plus button, and pick the Sapple IPA.
3. Sign in with your Apple ID when asked.
4. Wait for it to install, then open Sapple once.
5. Keep AltServer running so AltStore can refresh the app before it expires.

Sideloadly (quick one time install):

1. Install Sideloadly on your Mac or PC from [sideloadly.io](https://sideloadly.io/).
2. Connect your iPhone with a cable and open Sideloadly.
3. Drag the IPA in, enter your Apple ID, and click Start.
4. On your iPhone, open Settings, then General, then VPN and Device Management, and trust your developer profile.
5. Open Sapple once.

With a free Apple ID the app stops working after 7 days and has to be reinstalled. A paid Apple Developer account keeps it running for a year.

On the Mac, put Sapple in Applications and open it once so macOS picks up the voices. Leave it there, since macOS finds the voices through the app.

## Picking the voices

On iPhone and iPad, go to Settings, Accessibility, VoiceOver, Speech, Voice (or Settings, Accessibility, Spoken Content, Voices) and look under English for the Sapple voices. You can also add them to the VoiceOver rotor from the same Speech screen.

On the Mac, the voices show up in VoiceOver Utility under Speech, and in System Settings, Accessibility, Spoken Content, System voice.

The Sapple app itself lets you preview every voice and set the speed, and has a singing mode for Sam, Mike and Mary.

## Building from source

You need a Mac with Xcode and [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`). The Xcode project is generated from `project.yml`. The app targets iOS 18 and macOS 15 and up.

The voice data isn't included, so first put your own copies in these folders at the top of the repo. They're all ignored by git.

```
VoiceData/      SAPI 5: Sam, Mike, Mary (.spd, .sdf) and the two .LXA lexicons
SAPI4Voices/    SAPI 4: msttssyn.dll and its .vce / .cfg voice files    (make -C SAPI4 bundle)
TruVoiceData/   tvdata.s, generated from the TruVoice engine tables        (make truvoice-data)
AnnaVoice/      Anna: the eleven M1033DSK.* files from Windows 7            (make anna-data)
OneCoreVoice/   Windows 10 / 11 OneCore voices and language data            (make onecore-data)
NeuralVoices/   Windows 11 natural voice packages and their model key       (make neural-stage)
NeuralSDK/      Microsoft's embedded Speech SDK libraries, ios/ and macos/  (make neural-stage)
```

The make targets stage or check each folder against the SHA-256 lists in `tools/` and `SAPI4/tools/`, so you can tell whether your copy matches the one the engines were checked against.

Then:

```
xcodegen generate
xcodebuild -project ClassicVoices.xcodeproj -scheme ClassicVoices -configuration Release \
  -destination 'generic/platform=iOS' -derivedDataPath build -allowProvisioningUpdates \
  DEVELOPMENT_TEAM=<your team ID> build
```

To sign with your own account, change `DEVELOPMENT_TEAM`, the bundle identifiers in `project.yml` and the app group in the entitlements files. The Mac targets (`ClassicVoices-macOS`) sign with a Developer ID certificate; the comments in `project.yml` explain.

To check the code compiles without any voice data, run `tools/stub_data.sh` first. It fills the folders with placeholders (empty folders, zeroed TruVoice tables, empty libraries). That build can't speak, so don't install it. `tools/stub_data.sh --clean` removes the placeholders.

The Makefile also builds each engine for the Mac with sanitizers and renders samples, for example `make test`, `make anna-test`, `make onecore-test`, `make truvoice-test`, `make sapi4-test` and `make neural-test`.

The build produces Sapple.app on iOS and macOS. Inside the code the project, its targets and the extension (ClassicVoicesExtension.appex) are still called ClassicVoices, and the bundle identifiers are unchanged.

## Project layout

```
App/          the app: voice list, preview, speed, singing, self-test
Extension/    the speech synthesis extension that registers the voices with the system
Shared/       code both use: voice list, SSML, rate and pitch, one C bridge per engine
Engine/       the engines, vendored from their upstream projects, with our patches in Engine/patches/
SAPI4/        an x86 interpreter that runs the original SAPI 4 DLLs, kept as the reference for testing
Tests/        SSML and pause tests
tools/        data staging, hash checks, table and patch generators
```

## About the voices

- SAM, Mike and Mary (SAPI 5): engine from [ms-sam-mike-mary-decomp](https://github.com/KamiKitsune420/ms-sam-mike-mary-decomp) by KamiKitsune420. Voice data by Microsoft.
- The SAPI 4 voices: Microsoft's 1999 engine `msttssyn.dll`, decompiled to C in a separate project of mine, sapi4-decomp. The engine still reads its data from the original DLL and voice files, which are Microsoft's.
- TruVoice (the Microsoft Agent voices): engine from [OpenTV](https://github.com/RetroBunn/tv-decomp) by RetroBunn. The engine tables are Centigram's, whose TruVoice business went to Lernout & Hauspie and later ScanSoft / Nuance.
- Anna: engine from [ms-ana-decomp](https://github.com/KamiKitsune420/ms-ana-decomp) by KamiKitsune420. Voice data by Microsoft.
- David, Zira, Mark, Hazel, George and Susan (OneCore): engine from [ms-david-zira-decomp](https://github.com/KamiKitsune420/ms-david-zira-decomp) by KamiKitsune420, plus Eva and Sarah, which I added. Voice data by Microsoft.
- Jenny, Aria, Guy, Sonia and Ryan (Windows 11 natural voices): these run on Microsoft's embedded Speech SDK. Voices and SDK by Microsoft.

None of the voice data, original engine files or SDKs are in this repo, and this project doesn't license them. They belong to their owners.

## License

No license has been picked for Sapple's own code yet, so for now it's all rights reserved. The vendored engines keep their own licenses (MIT for the four from GitHub; sapi4-decomp has none yet). NOTICE has the details, including the pinned upstream commits.
