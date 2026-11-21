# Sapple for Android

The same voices as a text-to-speech engine for Android, so TalkBack and any app that speaks can use them. It runs the same C engines and bridges as the iPhone and Mac builds, compiled with the same flags. Only the Java around them is new.

What's in it: Sam, Mike and Mary (SAPI 5) with all their modes, the SAPI 4 voices, TruVoice, Anna, and the OneCore voices with their emotions. That's 71 voices. The Windows 11 natural voices aren't in it yet. They need Microsoft's embedded Speech SDK for Android, which is a separate piece of work.

## Voice data

None of the voice data is in this repo, and the app doesn't ship it. On the phone, the app imports it from a Sapple download the person already has. That can be the iPhone `.ipa`, the Mac `.zip`, or any zip with the data folders inside (`VoiceData`, `SAPI4Voices`, `AnnaVoice`, `OneCoreVoice`). It copies those folders into its own storage and leaves the file alone.

TruVoice is the exception. Its tables are compiled into the engine, as on iOS, so it's only in a build made with them (see below).

## Using it

1. Install the app and open it.
2. Tap "Import voices from a Sapple download" and pick the `.ipa` or `.zip`.
3. Tap the Voice button and choose the voice TalkBack should use. "Preview voice" plays it.
4. Tap "Open text-to-speech settings" and set Sapple as the preferred engine.

TalkBack asks for a language rather than a voice, so it gets the voice chosen in the app. Apps that ask for a voice by name get that voice. The names are the same slugs as on iOS (`sam`, `sapi4_mary`, `onecore_zira_happy` ...).

## Building

You need JDK 17 or later, the Android SDK (platform 36) and NDK 29.

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

Tested on the Android 16 emulator (arm64). Both the iPhone and Mac downloads imported all 71 voices. One voice from each engine was rendered, and Whisper transcribed every rendering correctly. The same tests pass on a Pixel 9 Pro running Android 17, including the language-only request TalkBack makes. It hasn't been tried with TalkBack speaking yet.
