# Sapple for Android

The same voices as a text-to-speech engine for Android, so TalkBack and any app that speaks can use them. It runs the same C engines and bridges as the iPhone and Mac builds, compiled with the same flags. Only the Java around them is new.

What's in it: Sam, Mike and Mary (SAPI 5) with all their modes and the six 1.2 presets, the SAPI 4 voices, TruVoice, Anna, the OneCore voices with their emotions, and the seven Windows 11 natural voices (Jenny, Aria, Guy, Sonia, Ryan, Neerja and Prabhat). That's 84 voices. It runs on 64-bit and 32-bit Android phones.

The natural voices run on Microsoft's embedded Speech SDK 1.33, the same version the iPhone and Mac builds use, through the same bridge (`cvn_bridge.c`). The build downloads the SDK's Android libraries from Maven Central and packs four of them into the app. None of it is in git.

## Voice data

None of the voice data is in this repo, and the app doesn't ship it. On the phone, the app imports it from a Sapple download the person already has. That can be the iPhone `.ipa`, the Mac `.zip`, or any zip with the data folders inside (`VoiceData`, `SAPI4Voices`, `AnnaVoice`, `OneCoreVoice`, `NeuralVoices`). It copies those folders into its own storage and leaves the file alone.

TruVoice is the exception. Its tables are compiled into the engine, as on iOS, so it's only in a build made with them (see below).

## Using it

1. Install the app and open it.
2. Tap "Import voices from a Sapple download" and pick the `.ipa` or `.zip`.
3. Tap the Voice button and choose the voice TalkBack should use. "Preview voice" plays it.
4. Tap "Open text-to-speech settings" and set Sapple as the preferred engine.

If something goes wrong, "Copy diagnostics" puts a short report on the clipboard to paste into a bug report: the app and Android versions, whether the app is running as 32- or 64-bit, how many voices of each kind are on the phone, and the last failure. It never includes anything you had spoken.

TalkBack asks for a language rather than a voice, so it gets the voice chosen in the app. Apps that ask for a voice by name get that voice. The names are the same slugs as on iOS (`sam`, `sapi4_mary`, `onecore_zira_happy` ...).

## Building

You need JDK 17 or later, the Android SDK (platform 36) and NDK 29. The build makes one APK with `armeabi-v7a`, `arm64-v8a` and `x86_64` code.

```
cd Android
./gradlew assembleDebug
```

For TruVoice, generate its tables first. They're voice data, so they're gitignored like the rest:

```
git clone https://github.com/RetroBunn/tv-decomp Engine/opentv-upstream
git -C Engine/opentv-upstream checkout b8897650b9e0dde14f95f67db3937e97858baa41
tools/truvoice_data_android.sh        # writes TruVoiceData/tvdata-elf.s
```

The CMake build picks it up when it's there and leaves TruVoice out when it isn't. After adding or removing it, delete `Android/app/.cxx` so CMake looks again.

## Tests

`app/src/androidTest` speaks through Android's own TextToSpeech API, bound to this engine, the way TalkBack reaches it. It uses one voice from each engine and checks that each one produces real audio. It needs voice data on the device. `ImportTest` imports a real download pushed to `/data/local/tmp/sapple-import.zip` and checks that every voice comes through.

```
adb push Sapple-1.1.1.ipa /data/local/tmp/sapple-import.zip
./gradlew connectedDebugAndroidTest
```

Note that `connectedDebugAndroidTest` uninstalls the app afterwards. To keep the rendered WAVs (`files/tts-test` in the app's storage), install both APKs and run `adb shell am instrument -w com.quinton.sapple.test/androidx.test.runner.AndroidJUnitRunner` instead.

Tested on the Android 16 emulator (arm64). Both the iPhone and Mac 1.1.1 downloads imported all 84 voices. Thirteen voices were rendered, at least one from each engine: Sam, Mike in Hall, SAPI 4 Mary, Anna, David, Zira Happy, Hazel, Eva, Peter, Jack, Jenny, Sonia and Prabhat. Whisper transcribed every one correctly, and so did the language-only request TalkBack makes. Before the natural voices and 32-bit code were added, the same tests also passed on a Pixel 9 Pro running Android 17. This version hasn't been on a real phone yet. The 32-bit code builds but hasn't run anywhere, since the emulator is 64-bit only. It hasn't been tried with TalkBack speaking yet.

Sandroid (averlice/sandroid) got the natural voices and 32-bit builds working on Android first. Its build showed which SDK libraries the offline voices need. This port was written separately, with no code taken from it.
