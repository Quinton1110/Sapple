//
//  Voices.swift
//  The voices this app registers with iOS. Shared by the app (preview) and the extension.
//
//  The set mirrors the original Microsoft voices of 1999-2001: the three SAPI 5 voices (Sam, Mike,
//  Mary) and the SAPI 4 voice modes that the engine's sam4fx reconstruction provides. Base voices and
//  pitches of the RoboSoft and Whisper voices follow the SAPI 4 defaults (RoboSoft Three = Mike's
//  113 Hz, Four = Mary's 169 Hz, One 75 Hz, Two 120 Hz, Six 100 Hz). RoboSoft Five is left out: in
//  this reconstruction it would be identical to RoboSoft Two (same preset, same pitch). The
//  "(for Telephone)" voices are a different band-limited model, not an effect, so they are not here.
//
//  The second list, SAPI4_VOICES, is different in kind: the 1999 Microsoft SAPI 4 engine (msttssyn.dll), since
//  2026-09-27 decompiled to C (Engine/sapi4, reading the DLL's data at run time; before that the DLL itself ran in the
//  app's x86 interpreter, SAPI4/emu - same PCM), the 19 modes the original enumerates,
//  named by the engine's own mode names plus "(SAPI 4)". Their slugs all start with "sapi4_", which no
//  SAPI 5 slug does, so the two sets can never collide.
//
//  The third list, TRUVOICE_VOICES, is Lernout & Hauspie / Centigram TruVoice American English (the voices of
//  Microsoft Agent / BonziBuddy), since 2026-09-26 on OpenTV (Engine/opentv, the engine decompiled to C; before that
//  the original 1998 SAPI 4 DLL tv_enua.dll in the same interpreter): the 10 modes tv_enua.dll enumerated, "Adult Male #1".."#8" and "Adult Female #1", "#2". Their engine names all read
//  "<mode>, American English (TruVoice)"; shown since 2026-09-27 by speaker name alone, "Peter" ... (before: "Peter (TruVoice)", and "<mode> (TruVoice)") (Quinton, 2026-09-25 14:51: not Microsoft's, so
//  no "Microsoft " and the engine's own name as the suffix; was "<name> (SAPI 4)"). Slugs start with "truvoice_".
//
//  The fourth, ANNA_VOICE, is Microsoft Anna, the default voice of Windows Vista and 7 (SAPI 5.3, the "TTS20"
//  unit-selection engine): Engine/anna, a C reconstruction (its author reports it bit-exact with the original
//  engine) reading Microsoft's own voice data. Listed with the SAPI 5 voices.
//
//  The fifth, ONECORE_VOICES, is Microsoft David, Zira and Mark, the voices of Windows 10 and 11 (the "OneCore" statistical
//  engine): Engine/onecore, a C reconstruction (its author reports it bit-exact with the original engine) reading
//  Microsoft's own voice data (OneCoreVoice). Each voice also comes in the three hidden emotion presets its voice file
//  carries and Windows never plays: happy, sad, angry. Picker section "Windows 10". Slugs start with "onecore_", which
//  no other slug does (a later port of the older SAPI 5 "Microsoft David Desktop" could then never collide).
//  The same engine also speaks Microsoft Hazel, George and Susan, the British English (en-GB) OneCore voices, from the
//  en-GB language data (Quinton asked for Hazel 2026-09-25, and George and Susan if they passed the same test). Their
//  voice files carry no [EmotionRecipe], so they come plain only, and they are declared en-GB so VoiceOver lists them
//  under English (United Kingdom). Same picker section.
//  And Microsoft Eva (2026-09-25, Quinton: "Yes"), the neural "Cortana" voice Windows ships but never lists: the same front
//  end, her own neural acoustic model and prosody models (Engine/onecore/eva_*.c). Her INI carries [EmotionRecipe] happy /
//  sad / angry like David's, so she comes in the same four forms. Same picker section.
//  And Microsoft Sarah (2026-09-25, Quinton: "Yes, to Sarah"), the British English neural voice Windows also ships but never
//  lists: the same back end as Eva with her 16-bit feed-forward network, frame skipping, duration equalisation and
//  five-band excitation; no prosody models and no [EmotionRecipe], so she comes plain only, en-GB. Same picker section.
//
//  The sixth, NEURAL_VOICES, is the Windows 11 Narrator "natural" voices - Jenny, Aria and Guy (en-US), Sonia and Ryan
//  (en-GB), Neerja and Prabhat (en-IN, 2026-09-30) - on Microsoft's own embedded (offline) Speech SDK engine (cvn_bridge.c; the SDK's dylibs in NeuralSDK, the
//  voice models in NeuralVoices). They come from Quinton's earlier separate app, NeuralVoice (Quinton, 2026-09-27: "I'd
//  like to retire that app and just have them be options in this app"). Picker section "Neural". Slugs start with
//  "neural_", which no other slug does. (The old app's identifiers were com.quinton.neuralvoice.<name> - another app, so a
//  voice picked there has to be picked again here.)
//
//  DISPLAY NAMES (Quinton, 2026-09-27): no "Microsoft " in any voice name. The SAPI voices keep their engine suffix, on
//  matching base names so the pairs sit side by side in VoiceOver's list ("Sam (SAPI 5)" / "Sam (SAPI 4)", "Mike in Hall
//  (SAPI 5)" / "Mike in Hall (SAPI 4)"; his choice, 2026-09-21). Anna is "Anna"; the Windows 10 voices "David", "Zira" ...
//  with the emotion word last, "Mark Happy" (2026-09-25); the neural voices "Jenny", "Aria" ...; the TruVoice voices just
//  their speaker's name, "Peter" - including "Alex" (Quinton, 2026-09-27 13:03, on the clash with Apple's Alex: "That's
//  fine because they'd be under the app's section"; see TRUVOICE_DISPLAY_NAMES). No two voices of this app may share a
//  name (make ssml-test checks it). Display names may change; slugs may not.
//
//  ⚠️ The slugs are part of the system voice identifiers VoiceOver stores. NEVER rename or reuse one.
//

import AVFoundation

enum ClassicEngineKind: Hashable {
    case sapi5      // Engine/sam: the Sam / Mike / Mary C reconstruction (+ sam4fx effect modes)
    case sapi4      // Engine/sapi4: Microsoft's SAPI 4 engine (msttssyn.dll) decompiled to C (cv4n_bridge; was the DLL in SAPI4/emu)
    case truvoice   // Engine/opentv: L&H / Centigram TruVoice decompiled to C (cvt_bridge; was tv_enua.dll in SAPI4/emu)
    case anna       // Engine/anna: Microsoft Anna (Vista / 7 TTS20) reconstructed in C, Microsoft's data (AnnaVoice)
    case onecore    // Engine/onecore: Microsoft David / Zira / Mark / Hazel / George / Susan / Eva / Sarah / Catherine / James / Linda / Richard / Matilda (Windows 10 / 11), data OneCoreVoice
    case neural     // Microsoft's embedded Speech SDK (NeuralSDK, dlopen'ed) + cvn_bridge.c: Jenny / Aria / Guy / Sonia / Ryan / Neerja / Prabhat, data NeuralVoices
}

struct ClassicVoiceDef: Hashable {
    let slug: String        // stable identifier suffix
    let display: String     // shown in VoiceOver's voice list and in the app
    let spd: String         // Sam | Mike | Mary  (<spd>.spd + <spd>.sdf in VoiceData)
    let effect: String      // "none" or a SAPI 4 mode understood by sam4fx_lookup
    let basePitch: Double   // Hz; 0 keeps the voice's own pitch from its .sdf
    let female: Bool
    var engine: ClassicEngineKind = .sapi5
    var sapi4Mode: String = ""   // .sapi4 / .truvoice: the engine's own mode name ("Mike in Hall")
    var neuralVoice: String = ""  // .neural: "Jenny", "Aria", "Guy", "Sonia", "Ryan", "Neerja", "Prabhat" (NeuralVoices/<name>)
    var oneCoreVoice: String = "" // .onecore: "David", "Zira", "Mark", "Eva" (M1033<name>.*), "Hazel", "George", "Susan", "Sarah" (M2057<name>.*), "Catherine", "James", "Matilda" (M3081<name>.*), "Linda", "Richard" (M4105<name>.*)
    var emotion: String = ""      // .onecore: "" (normal), "happy", "sad" or "angry" (the voice file's [EmotionRecipe])
    var language: String = "en-US" // BCP 47; what VoiceOver lists the voice under (the en-GB OneCore voices: "en-GB")

    var identifier: String { CLASSIC_VOICE_ID_PREFIX + "." + slug }
    var gender: AVSpeechSynthesisVoiceGender { female ? .female : .male }
}

/// A voice of the original SAPI 4 engine. `mode` must be exactly the name the engine enumerates.
private func sapi4Voice(_ slug: String, _ display: String, mode: String, female: Bool) -> ClassicVoiceDef {
    ClassicVoiceDef(slug: slug, display: display, spd: "", effect: "none", basePitch: 0, female: female,
                    engine: .sapi4, sapi4Mode: mode)
}

/// A voice of the L&H TruVoice engine; `name` is the speaker's name, `mode` the engine's mode name without
/// ", American English (TruVoice)" (what cvt_bridge opens). Shown as TRUVOICE_DISPLAY_NAMES says.
private func truVoice(_ slug: String, _ name: String, mode: String, female: Bool) -> ClassicVoiceDef {
    ClassicVoiceDef(slug: slug, display: TRUVOICE_DISPLAY_NAMES[name] ?? name, spd: "", effect: "none", basePitch: 0,
                    female: female, engine: .truvoice, sapi4Mode: mode + ", American English (TruVoice)")
}

/// ⚙️ THE ONE PLACE a TruVoice voice's shown name can differ from its speaker's name. Empty = every TruVoice voice shows
/// as just its name, "Peter" ... "Alex" (Quinton, 2026-09-27 13:03: the TruVoice Alex stays "Alex" although Apple has an
/// Alex - "That's fine because they'd be under the app's section"). To give one a suffix again, e.g.:
/// ["Alex": "Alex (TruVoice)"].
let TRUVOICE_DISPLAY_NAMES: [String: String] = [:]

let CLASSIC_VOICE_ID_PREFIX = "com.quinton.classicvoices"

let CLASSIC_VOICES: [ClassicVoiceDef] = [
    ClassicVoiceDef(slug: "sam",            display: "Sam (SAPI 5)",    spd: "Sam",  effect: "none",      basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "mike",           display: "Mike (SAPI 5)",   spd: "Mike", effect: "none",      basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "mary",           display: "Mary (SAPI 5)",   spd: "Mary", effect: "none",      basePitch: 0,   female: true),
    ClassicVoiceDef(slug: "mike_hall",      display: "Mike in Hall (SAPI 5)",    spd: "Mike", effect: "hall",      basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "mike_stadium",   display: "Mike in Stadium (SAPI 5)", spd: "Mike", effect: "stadium",   basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "mike_space",     display: "Mike in Space (SAPI 5)",   spd: "Mike", effect: "space",     basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "mary_hall",      display: "Mary in Hall (SAPI 5)",    spd: "Mary", effect: "hall",      basePitch: 0,   female: true),
    ClassicVoiceDef(slug: "mary_stadium",   display: "Mary in Stadium (SAPI 5)", spd: "Mary", effect: "stadium",   basePitch: 0,   female: true),
    ClassicVoiceDef(slug: "mary_space",     display: "Mary in Space (SAPI 5)",   spd: "Mary", effect: "space",     basePitch: 0,   female: true),
    ClassicVoiceDef(slug: "robosoft1",      display: "RoboSoft One (SAPI 5)",    spd: "Sam",  effect: "robosoft1", basePitch: 75,  female: false),
    ClassicVoiceDef(slug: "robosoft2",      display: "RoboSoft Two (SAPI 5)",    spd: "Sam",  effect: "robosoft2", basePitch: 120, female: false),
    ClassicVoiceDef(slug: "robosoft3",      display: "RoboSoft Three (SAPI 5)",  spd: "Mike", effect: "robosoft3", basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "robosoft4",      display: "RoboSoft Four (SAPI 5)",   spd: "Mary", effect: "robosoft4", basePitch: 0,   female: true),
    ClassicVoiceDef(slug: "robosoft6",      display: "RoboSoft Six (SAPI 5)",    spd: "Sam",  effect: "robosoft6", basePitch: 100, female: false),
    ClassicVoiceDef(slug: "male_whisper",   display: "Male Whisper (SAPI 5)",    spd: "Mike", effect: "whisper",   basePitch: 0,   female: false),
    ClassicVoiceDef(slug: "female_whisper", display: "Female Whisper (SAPI 5)",  spd: "Mary", effect: "whisper",   basePitch: 0,   female: true),
] + [ANNA_VOICE] + ONECORE_VOICES + SAPI4_VOICES + TRUVOICE_VOICES + NEURAL_VOICES

/// Microsoft Anna (Windows Vista / 7, SAPI 5.3): one voice, the engine's own. Slug "anna" is permanent.
let ANNA_VOICE = ClassicVoiceDef(slug: "anna", display: "Anna", spd: "", effect: "none", basePitch: 0,
                                 female: true, engine: .anna)

/// The OneCore voices whose INIs carry the [EmotionRecipe] presets happy / sad / angry.
let ONECORE_EMOTION_VOICES: Set<String> = ["David", "Zira", "Mark", "Eva"]

/// Microsoft David, Zira and Mark (Windows 10 / 11 OneCore, en-US), each plain and in its three hidden emotion presets
/// (12 voices), then Microsoft Hazel, George and Susan (en-GB, no emotion presets in their files: 3 voices), then
/// Microsoft Eva (en-US, the neural "Cortana" voice, 2026-09-25: plain and in her INI's three presets, 4 voices), then
/// Microsoft Sarah (en-GB, the British neural voice, 2026-09-25: plain only, her INI has no presets), then Microsoft
/// Catherine and James (en-AU) and Linda and Richard (en-CA) (2026-09-29, from the Windows 11 language packages: plain only,
/// no presets in their INIs; VoiceOver lists Catherine and James under English (Australia), and Linda and Richard under
/// English (US): iOS's VoiceOver voice picker has no English (Canada) section, so an "en-CA" tag hid them (Quinton,
/// 2026-09-30). The tag is only what the system lists them under; the engine still uses their en-CA LCID 4105 and data),
/// then Microsoft Matilda (en-AU, the Australian neural voice Windows ships but never lists, 2026-09-30: plain only, her INI
/// has no presets; LCID 3081 and the en-AU data Catherine and James use).
/// Only David, Zira, Mark and Eva have emotion presets (ONECORE_EMOTION_VOICES), whatever their language tag.
/// Slugs "onecore_<voice>" and "onecore_<voice>_<emotion>" are permanent.
let ONECORE_VOICES: [ClassicVoiceDef] = [("David", false, "en-US"), ("Zira", true, "en-US"), ("Mark", false, "en-US"),
                                         ("Hazel", true, "en-GB"), ("George", false, "en-GB"), ("Susan", true, "en-GB"),
                                         ("Eva", true, "en-US"), ("Sarah", true, "en-GB"),
                                         ("Catherine", true, "en-AU"), ("James", false, "en-AU"),
                                         ("Linda", true, "en-US"), ("Richard", false, "en-US"),
                                         ("Matilda", true, "en-AU")]
    .flatMap { name, female, language in
        (ONECORE_EMOTION_VOICES.contains(name) ? ["", "happy", "sad", "angry"] : [""]).map { emotion in
            ClassicVoiceDef(slug: "onecore_" + name.lowercased() + (emotion.isEmpty ? "" : "_" + emotion),
                            display: name + (emotion.isEmpty ? "" : " " + emotion.capitalized),
                            spd: "", effect: "none", basePitch: 0, female: female, engine: .onecore,
                            oneCoreVoice: name, emotion: emotion, language: language)
        }
    }

/// The original SAPI 4 engine's 19 modes (genders are the engine's own: RoboSoft Four, Five and Six
/// are built on its female voice).
let SAPI4_VOICES: [ClassicVoiceDef] = [
    sapi4Voice("sapi4_sam",             "Sam (SAPI 4)",                 mode: "Sam",                  female: false),
    sapi4Voice("sapi4_mike",            "Mike (SAPI 4)",                mode: "Mike",                 female: false),
    sapi4Voice("sapi4_mary",            "Mary (SAPI 4)",                mode: "Mary",                 female: true),
    sapi4Voice("sapi4_mike_telephone",  "Mike for Telephone (SAPI 4)",  mode: "Mike (for Telephone)", female: false),
    sapi4Voice("sapi4_mary_telephone",  "Mary for Telephone (SAPI 4)",  mode: "Mary (for Telephone)", female: true),
    sapi4Voice("sapi4_mike_hall",       "Mike in Hall (SAPI 4)",        mode: "Mike in Hall",         female: false),
    sapi4Voice("sapi4_mike_stadium",    "Mike in Stadium (SAPI 4)",     mode: "Mike in Stadium",      female: false),
    sapi4Voice("sapi4_mike_space",      "Mike in Space (SAPI 4)",       mode: "Mike in Space",        female: false),
    sapi4Voice("sapi4_mary_hall",       "Mary in Hall (SAPI 4)",        mode: "Mary in Hall",         female: true),
    sapi4Voice("sapi4_mary_stadium",    "Mary in Stadium (SAPI 4)",     mode: "Mary in Stadium",      female: true),
    sapi4Voice("sapi4_mary_space",      "Mary in Space (SAPI 4)",       mode: "Mary in Space",        female: true),
    sapi4Voice("sapi4_robosoft1",       "RoboSoft One (SAPI 4)",        mode: "RoboSoft One",         female: false),
    sapi4Voice("sapi4_robosoft2",       "RoboSoft Two (SAPI 4)",        mode: "RoboSoft Two",         female: false),
    sapi4Voice("sapi4_robosoft3",       "RoboSoft Three (SAPI 4)",      mode: "RoboSoft Three",       female: false),
    sapi4Voice("sapi4_robosoft4",       "RoboSoft Four (SAPI 4)",       mode: "RoboSoft Four",        female: true),
    sapi4Voice("sapi4_robosoft5",       "RoboSoft Five (SAPI 4)",       mode: "RoboSoft Five",        female: true),
    sapi4Voice("sapi4_robosoft6",       "RoboSoft Six (SAPI 4)",        mode: "RoboSoft Six",         female: true),
    sapi4Voice("sapi4_male_whisper",    "Male Whisper (SAPI 4)",        mode: "Male Whisper",         female: false),
    sapi4Voice("sapi4_female_whisper",  "Female Whisper (SAPI 4)",      mode: "Female Whisper",       female: true),
]

/// L&H TruVoice American English: the 10 modes of tv_enua.dll 6.0.0.10 (1998) in the engine's own order, which is also
/// OpenTV's voice order (cvt_bridge.c maps the mode name to it).
/// Shown by their speakers' names (Quinton, 2026-09-27: "TruVoice should also now use the proper names, not just adult male
/// or female"). Mapping mode -> speaker from tv_enua.dll itself: each mode name is followed in its data by the speaker
/// string it reports (Adult Male #1 Peter, #2 Sidney, #3 Eddie, #4 Douglas, #5 Biff, #6 Amos, #7 Melvin, #8 Alex,
/// Adult Female #1 Wanda, #2 Julia); the display forms "Eager Eddie", "Deep Douglas", "Grandpa Amos" are the Microsoft
/// Agent names as github.com/devinprater/iTruVoice (fa9d7d5, TruVoice.voiceNames, same engine order) shows them.
/// Slugs stay the mode-based ones: VoiceOver's stored choices hang on them.
let TRUVOICE_VOICES: [ClassicVoiceDef] = [
    truVoice("truvoice_adult_male_1",    "Peter",        mode: "Adult Male #1",   female: false),
    truVoice("truvoice_adult_male_2",    "Sidney",       mode: "Adult Male #2",   female: false),
    truVoice("truvoice_adult_male_3",    "Eager Eddie",  mode: "Adult Male #3",   female: false),
    truVoice("truvoice_adult_male_4",    "Deep Douglas", mode: "Adult Male #4",   female: false),
    truVoice("truvoice_adult_male_5",    "Biff",         mode: "Adult Male #5",   female: false),
    truVoice("truvoice_adult_male_6",    "Grandpa Amos", mode: "Adult Male #6",   female: false),
    truVoice("truvoice_adult_male_7",    "Melvin",       mode: "Adult Male #7",   female: false),
    truVoice("truvoice_adult_male_8",    "Alex",         mode: "Adult Male #8",   female: false),
    truVoice("truvoice_adult_female_1",  "Wanda",        mode: "Adult Female #1", female: true),
    truVoice("truvoice_adult_female_2",  "Julia",        mode: "Adult Female #2", female: true),
]

/// The Windows 11 Narrator natural voices on Microsoft's embedded Speech SDK (cvn_bridge.c): en-US Jenny, Aria, Guy, then
/// en-GB Sonia and Ryan (declared en-GB, so VoiceOver lists them under English (United Kingdom)), then en-IN Neerja and
/// Prabhat (2026-09-30, declared en-IN: English (India); last, so no earlier slug moves). Slugs "neural_<name>" are
/// permanent. Their SDK is arm64 only: an Intel Mac lists none of them.
let NEURAL_VOICES: [ClassicVoiceDef] = {
#if os(macOS) && !arch(arm64)
    return []
#else
    return [("Jenny", true, "en-US"), ("Aria", true, "en-US"), ("Guy", false, "en-US"),
            ("Sonia", true, "en-GB"), ("Ryan", false, "en-GB"),
            ("Neerja", true, "en-IN"), ("Prabhat", false, "en-IN")].map { name, female, language in
        ClassicVoiceDef(slug: "neural_" + name.lowercased(), display: name, spd: "", effect: "none", basePitch: 0,
                        female: female, engine: .neural, neuralVoice: name, language: language)
    }
#endif
}()

enum VoiceCatalog {
    static func def(forIdentifier id: String) -> ClassicVoiceDef {
        let slug = id.split(separator: ".").last.map(String.init) ?? ""
        return CLASSIC_VOICES.first(where: { $0.slug == slug }) ?? CLASSIC_VOICES[0]
    }

    /// "Sam", "Mike in Hall", "Mike for Telephone" ... for the sample sentence.
    static func shortName(_ def: ClassicVoiceDef) -> String {
        def.display.replacingOccurrences(of: " (SAPI 5)", with: "")
            .replacingOccurrences(of: " (SAPI 4)", with: "")
            .replacingOccurrences(of: " (TruVoice)", with: "")
    }

    /// Display names used by more than one voice of this app (must stay empty; make ssml-test checks it).
    static var duplicateDisplayNames: [String] {
        Dictionary(grouping: CLASSIC_VOICES, by: \.display).filter { $0.value.count > 1 }.keys.sorted()
    }
}
