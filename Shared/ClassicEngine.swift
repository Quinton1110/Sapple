//
//  ClassicEngine.swift
//  Swift side of the C engines, shared by the app and the speech-provider extension. No dylibs:
//  the C sources are compiled into each target.
//   - SAPI 5 voices: Engine/sam + Shared/Bridge/cv_bridge.c (the Sam / Mike / Mary reconstruction)
//   - SAPI 4 voices: Engine/sapi4 (Microsoft's msttssyn.dll decompiled to C, reading the DLL's data and the voice files
//     from SAPI4Voices) + cv4n_bridge.c. Until 2026-09-27 the original DLL ran in an x86 interpreter (SAPI4/emu +
//     cv4_bridge.c, kept for the tests); the new path gives the same PCM (make sapi4-ab).
//   - TruVoice voices: Engine/opentv (OpenTV, the TruVoice engine decompiled to C; its tables linked in from
//     TruVoiceData/tvdata.s) + cvt_bridge.c. Until 2026-09-26 they ran as L&H's tv_enua.dll in the SAPI 4 interpreter.
//   - Microsoft Anna: Engine/anna (the Vista / 7 TTS20 engine reconstructed in C) + cva_bridge.c, data AnnaVoice
//   - Microsoft David / Zira / Mark (en-US) and Hazel / George / Susan (en-GB): Engine/onecore (the Windows 10 / 11 OneCore
//     engine reconstructed in C) + cvo_bridge.c, data OneCoreVoice (both languages' files in the one folder); the emotion
//     presets share their voice's handle (the emotion is set per utterance)
//   - Jenny / Aria / Guy (en-US) and Sonia / Ryan (en-GB), the Windows 11 Narrator natural voices: Microsoft's embedded
//     Speech SDK (the dylibs in NeuralSDK, embedded in the extension's Frameworks and loaded with dlopen) + cvn_bridge.c,
//     data NeuralVoices (plus the language data they share with OneCoreVoice); one SDK engine per process, whichever
//     neural voice speaks
//  Every bridge takes the same arguments (text, SAPI rate, semitones, trim) and streams 22050 Hz PCM.
//
//  Voice handles are not reentrant, so a small process-wide pool hands each utterance its own handle
//  and keeps a few warm for the next one (SAPI 5: ~4 ms / ~3.7 MB to open; TruVoice: ~0.02 ms / well under 1 MB; SAPI 4: ~1-7 ms on a
//  Mac, ~0.4 MB per engine plus its voice's data, 1.6-2.3 MB, shared by the voice's modes - and cv4n_bridge never really
//  closes one: a closed SAPI 4 voice's engine is parked, warm, for the next open of its mode, because the library cannot
//  free an engine and allows 127 per process; Anna: ~8 ms / ~13 MB, the files
//  it reads into memory - the 30 MB of compressed recordings stay on disk; OneCore: ~7-18 ms, David ~12 MB, Zira and
//  Mark ~3-4 MB of their own plus the shared 11 MB language data, mapped read-only). iOS creates a fresh audio
//  unit for every utterance, so nothing per-voice may live on the audio unit itself.
//
//  Privacy: this is a screen-reader voice. Nothing here logs or stores the text it speaks.
//

import Foundation

final class ClassicEngine {
    static let shared = ClassicEngine()

    static let sampleRate: Double = 22050.0

    private enum Handle {
        case sapi5(OpaquePointer)
        case sapi4(OpaquePointer)
        case truvoice(OpaquePointer)
        case anna(OpaquePointer)
        case onecore(OpaquePointer)
        case neural(OpaquePointer)
    }

    private let lock = NSLock()
    private var idle: [(slug: String, handle: Handle)] = []   // most recently used last; keyed by poolKey
    private let maxIdle = 3

    private static var ownBundle: Bundle { Bundle(for: ClassicEngine.self) }

    /// Where the bundled data folders live. iOS keeps resources flat in the bundle, macOS puts them in
    /// `Contents/Resources` - `resourceURL` is the right answer on both.
    private static var resourcesURL: URL {
#if os(macOS)
        // Mac test harness only (Tests/ssml): the repo root, which holds VoiceData/, SAPI4Voices/ and AnnaVoice/.
        if let root = ProcessInfo.processInfo.environment["CV_DATA_ROOT"] { return URL(fileURLWithPath: root) }
#endif
        let b = ownBundle
        return b.resourceURL ?? b.bundleURL
    }

    /// VoiceData folder inside whichever bundle this code was compiled into (app or extension).
    let dataDir: String = ClassicEngine.resourcesURL.appendingPathComponent("VoiceData").path
    /// SAPI4Voices folder: msttssyn.dll plus the .vce / .cfg voice data (Microsoft's, not in git).
    let sapi4DataDir: String = ClassicEngine.resourcesURL.appendingPathComponent("SAPI4Voices").path
    /// AnnaVoice: Microsoft Anna's M1033DSK.* (39 MB, Microsoft's, not in git). Bundled once, in the extension; the
    /// app reads the extension's copy inside its own bundle (PlugIns/), so the device does not carry it twice.
    /// The appex lays its resources out per platform, so try every place it can be and take the one that is there.
    let annaDataDir: String = {
        let fm = FileManager.default
        var candidates = [ClassicEngine.resourcesURL.appendingPathComponent("AnnaVoice")]
        if let plugIns = ClassicEngine.ownBundle.builtInPlugInsURL {
            let appex = plugIns.appendingPathComponent("ClassicVoicesExtension.appex")
            candidates.append(appex.appendingPathComponent("Contents/Resources/AnnaVoice"))   // macOS
            candidates.append(appex.appendingPathComponent("AnnaVoice"))                      // iOS
        }
        for c in candidates where fm.fileExists(atPath: c.appendingPathComponent("M1033DSK.KEY").path) {
            return c.path
        }
        return candidates[0].path
    }()

    /// OneCoreVoice: Microsoft David / Zira / Mark's data (MSTTSLocEnUS.dat, enUS.*.dat, M1033<voice>.*; 23 MB,
    /// Microsoft's, not in git). Like AnnaVoice: bundled once, in the extension, and the app reads that copy.
    let oneCoreDataDir: String = {
        let fm = FileManager.default
        var candidates = [ClassicEngine.resourcesURL.appendingPathComponent("OneCoreVoice")]
        if let plugIns = ClassicEngine.ownBundle.builtInPlugInsURL {
            let appex = plugIns.appendingPathComponent("ClassicVoicesExtension.appex")
            candidates.append(appex.appendingPathComponent("Contents/Resources/OneCoreVoice"))   // macOS
            candidates.append(appex.appendingPathComponent("OneCoreVoice"))                      // iOS
        }
        for c in candidates where fm.fileExists(atPath: c.appendingPathComponent("MSTTSLocEnUS.dat").path) {
            return c.path
        }
        return candidates[0].path
    }()

    /// NeuralVoices: the neural voices' models (Microsoft's, not in git; `make neural-data`). Bundled in the extension only,
    /// like OneCoreVoice; the app reads that copy.
    let neuralDataDir: String = ClassicEngine.extensionResource("NeuralVoices", probe: "model.key")

    /// The Speech SDK's three dylibs: embedded (and signed) in the extension's Frameworks folder. The app loads the
    /// extension's copy; the Mac test harness (CV_DATA_ROOT) the staged NeuralSDK/macos.
    let neuralSDKDir: String = {
        let fm = FileManager.default
        let core = "libMicrosoft.CognitiveServices.Speech.core.dylib"
        var candidates: [URL] = []
#if os(macOS)
        if let root = ProcessInfo.processInfo.environment["CV_DATA_ROOT"] {
            candidates.append(URL(fileURLWithPath: root).appendingPathComponent("NeuralSDK/macos"))
        }
#endif
        if let f = ClassicEngine.ownBundle.privateFrameworksURL { candidates.append(f) }
        if let plugIns = ClassicEngine.ownBundle.builtInPlugInsURL {
            let appex = plugIns.appendingPathComponent("ClassicVoicesExtension.appex")
            candidates.append(appex.appendingPathComponent("Contents/Frameworks"))   // macOS
            candidates.append(appex.appendingPathComponent("Frameworks"))            // iOS
        }
        for c in candidates where fm.fileExists(atPath: c.appendingPathComponent(core).path) { return c.path }
        return candidates.first?.path ?? ""
    }()

    /// Where cvn_bridge builds each neural voice's folder of links and writes its INI (the rate lives there).
    let neuralWorkDir: String = {
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory())
        return base.appendingPathComponent("neural", isDirectory: true).path
    }()

    /// A data folder bundled in the extension only: the extension finds it in its own resources, the app inside the
    /// extension in its PlugIns folder (the appex lays out resources per platform, so every place is tried).
    private static func extensionResource(_ name: String, probe: String) -> String {
        let fm = FileManager.default
        var candidates = [resourcesURL.appendingPathComponent(name)]
        if let plugIns = ownBundle.builtInPlugInsURL {
            let appex = plugIns.appendingPathComponent("ClassicVoicesExtension.appex")
            candidates.append(appex.appendingPathComponent("Contents/Resources/" + name))   // macOS
            candidates.append(appex.appendingPathComponent(name))                          // iOS
        }
        for c in candidates where fm.fileExists(atPath: c.appendingPathComponent(probe).path) { return c.path }
        return candidates[0].path
    }

    private init() {}

    /// Handles are pooled per engine instance: a OneCore voice's emotion presets share one handle (the emotion is
    /// a per-utterance setting), every other voice pools by its slug.
    private static func poolKey(_ def: ClassicVoiceDef) -> String {
        def.engine == .onecore ? "onecore:" + def.oneCoreVoice : def.slug
    }

    // MARK: - Handle pool

    private func checkout(_ def: ClassicVoiceDef) -> Handle? {
        lock.lock()
        let key = Self.poolKey(def)
        if let i = idle.lastIndex(where: { $0.slug == key }) {
            let h = idle.remove(at: i).handle
            lock.unlock()
            return h
        }
        lock.unlock()
        var err = [CChar](repeating: 0, count: 256)
        switch def.engine {
        case .sapi5:
            return cv_voice_open(dataDir, def.spd, def.effect, def.basePitch, &err, err.count).map { .sapi5($0) }
        case .sapi4:
            return cv4n_voice_open(sapi4DataDir, def.sapi4Mode, &err, err.count).map { .sapi4($0) }
        case .truvoice:
            return cvt_voice_open(def.sapi4Mode, &err, err.count).map { .truvoice($0) }
        case .anna:
            return cva_voice_open(annaDataDir, &err, err.count).map { .anna($0) }
        case .onecore:
            return cvo_voice_open(oneCoreDataDir, def.oneCoreVoice, &err, err.count).map { .onecore($0) }
        case .neural:
            return cvn_voice_open(neuralSDKDir, neuralDataDir, oneCoreDataDir, neuralWorkDir, def.neuralVoice, &err,
                                  err.count).map { .neural($0) }
        }
    }

    private func checkin(_ def: ClassicVoiceDef, _ h: Handle) {
        lock.lock()
        idle.append((Self.poolKey(def), h))
        var evicted: [Handle] = []
        while idle.count > maxIdle { evicted.append(idle.removeFirst().handle) }
        lock.unlock()
        evicted.forEach(close)
    }

    /// Closes every idle handle (the next utterance of each voice opens a fresh engine). The Mac harness
    /// uses it so renders compare from the same engine state: Sam / Mike / Mary and the Microsoft SAPI 4
    /// engine carry noise and filter state from one utterance to the next. The SAPI 4 engines parked by
    /// cv4n_bridge are closed too (each fresh one costs ~0.4 MB for good: tests and the self-test only).
    func releaseIdle() {
        lock.lock()
        let all = idle.map(\.handle)
        idle.removeAll()
        lock.unlock()
        all.forEach(close)
        cv4n_discard_parked()
        // (The neural voices are left loaded: a warm one renders exactly like a fresh one - make neural-test - and the SDK
        // never gives a dropped voice's memory back.)
    }

    private func close(_ h: Handle) {
        switch h {
        case .sapi5(let p): cv_voice_close(p)
        case .sapi4(let p): cv4n_voice_close(p)
        case .truvoice(let p): cvt_voice_close(p)
        case .anna(let p): cva_voice_close(p)
        case .onecore(let p): cvo_voice_close(p)
        case .neural(let p): cvn_voice_close(p)
        }
    }

    // MARK: - Speaking

    private final class Sink {
        let body: (UnsafeBufferPointer<Int16>) -> Bool
        init(_ body: @escaping (UnsafeBufferPointer<Int16>) -> Bool) { self.body = body }
    }

    private static let onPCM: cv_pcm_fn = { pcm, n, user in
        guard let pcm, let user else { return 1 }
        let s = Unmanaged<Sink>.fromOpaque(user).takeUnretainedValue()
        return s.body(UnsafeBufferPointer(start: pcm, count: n)) ? 0 : 1
    }

    /// Speaks `text` (plain text, no markup), streaming 22050 Hz Int16 PCM to `onPCM` as the engine
    /// produces it. `onPCM` returns false to stop. Returns 0 done, 1 stopped, -1 error.
    /// `singing` (enabled): a SAPI 5 voice sings this whole text (an automatic melody; `literalScore`: the text is a
    /// score, and `mismatches` gets the number of words whose notes and syllables disagree). Every other engine
    /// ignores it. nil = speech, exactly as before. Callers normally go through speak(scored:) instead, which sings
    /// only the song scores inside the text.
    @discardableResult
    func speak(text: String, voice def: ClassicVoiceDef, sapiRate: Double, semitones: Double,
               trimSilence: Bool = true, singing: SingingSettings? = nil, literalScore: Bool = false,
               mismatches: UnsafeMutablePointer<Int32>? = nil,
               onPCM: @escaping (UnsafeBufferPointer<Int16>) -> Bool) -> Int32 {
        guard let h = checkout(def) else { return -1 }
        let sink = Sink(onPCM)
        let ctx = Unmanaged.passUnretained(sink).toOpaque()
        let trim: Int32 = trimSilence ? 1 : 0
        let rc: Int32 = withExtendedLifetime(sink) {
            text.withCString { cText in
                switch h {
                case .sapi5(let p):
                    if let sing = singing, sing.enabled {
                        var cfg = sing.bridge
                        return cv_voice_sing(p, cText, sapiRate, semitones, &cfg, literalScore ? 1 : 0, trim, Self.onPCM, ctx,
                                             mismatches)
                    }
                    return cv_voice_speak(p, cText, sapiRate, semitones, trim, Self.onPCM, ctx)
                case .sapi4(let p): return cv4n_voice_speak(p, cText, sapiRate, semitones, trim, Self.onPCM, ctx)
                case .truvoice(let p): return cvt_voice_speak(p, cText, sapiRate, semitones, trim, Self.onPCM, ctx)
                case .anna(let p): return cva_voice_speak(p, cText, sapiRate, semitones, trim, Self.onPCM, ctx)
                case .onecore(let p):
                    _ = cvo_voice_set_emotion(p, def.emotion)   // "" = the voice's normal speech
                    return cvo_voice_speak(p, cText, sapiRate, semitones, trim, Self.onPCM, ctx)
                case .neural(let p): return cvn_voice_speak(p, cText, sapiRate, semitones, trim, Self.onPCM, ctx)
                }
            }
        }
        if rc < 0, case .sapi4 = h {
            close(h)            // a failed SAPI 4 engine is dead: never hand it out again (cv4n_bridge drops it)
        } else if rc < 0, case .truvoice = h {
            close(h)            // TruVoice: the next utterance starts from a fresh voice
        } else if rc < 0, case .anna = h {
            close(h)            // Anna: the next utterance starts from a fresh voice rather than a doubtful one
        } else if rc < 0, case .onecore = h {
            close(h)            // OneCore: likewise
        } else if rc < 0, case .neural = h {
            close(h)            // neural: likewise (cvn_bridge also rebuilds its engine)
        } else {
            checkin(def, h)
        }
        return rc
    }

    /// How fast the voice really speaks at a SAPI rate, relative to its natural speed: the engines cap it
    /// (Microsoft SAPI 4: 3x its default words per minute; TruVoice: 250 wpm, 1.67x the usual 150 - cvt_bridge keeps the
    /// old tv_enua.dll's speed at every rate, see its RATE_ROWS). Anna and the
    /// SAPI 5 voices follow the whole -10...18 scale (1/3x to 7.2x); so do David, Zira and Mark (their emotion
    /// presets speak a little slower on top - happy and angry ~5%, sad ~25% - which is not counted here).
    /// Pauses between segments scale with this, so they keep pace with the speech.
    static func effectiveSpeed(sapiRate: Double, engine: ClassicEngineKind) -> Double {
        let f = SpeechRate.speedFactor(sapiRate: sapiRate)
        switch engine {
        case .sapi5, .anna, .onecore: return f
        case .sapi4: return min(f, 3.0)
        case .truvoice: return min(f, 250.0 / 150.0)
        case .neural: return cvn_speed_factor(sapiRate)   // 1/3x .. 3x: the engine's prosody-rate limits
        }
    }

    /// Singing mode, DECtalk style (Quinton, 2026-09-25 17:22-17:27: "I want voiceover to behave normally but if it
    /// encounters something someone had written for this mode it would be smart enough to know and switch"): with a
    /// SAPI 5 voice and the switch on (`singing.enabled`), every song score inside the text (cv_score_find: runs of
    /// "word NOTE BEATS ..." groups and "- BEATS" rests, 3+ note pairs or led by "tempo N") is sung literally, and the
    /// text around it is spoken as usual. Anything else - switch off, another engine, or text without a score - goes
    /// to speak(text:) unchanged: the same audio as before.
    @discardableResult
    func speak(scored text: String, voice def: ClassicVoiceDef, sapiRate: Double, semitones: Double,
               singing: SingingSettings?, mismatches: UnsafeMutablePointer<Int32>? = nil,
               onPCM: @escaping (UnsafeBufferPointer<Int16>) -> Bool) -> Int32 {
        guard def.engine == .sapi5, singing?.enabled ?? false, text.utf8.count >= 8 else {
            return speak(text: text, voice: def, sapiRate: sapiRate, semitones: semitones, onPCM: onPCM)
        }
        let bytes = Array(text.utf8)
        var runs = [cv_score_run](repeating: cv_score_run(), count: 64)
        let n = Int(text.withCString { cv_score_find($0, &runs, Int32(runs.count)) })
        if n == 0 {
            return speak(text: text, voice: def, sapiRate: sapiRate, semitones: semitones, onPCM: onPCM)
        }
        var pieces: [(String, Bool)] = []
        var at = 0
        for r in runs.prefix(n) {
            if r.start > at { pieces.append((String(decoding: bytes[at ..< r.start], as: UTF8.self), false)) }
            pieces.append((String(decoding: bytes[r.start ..< r.start + r.len], as: UTF8.self), true))
            at = r.start + r.len
        }
        if at < bytes.count { pieces.append((String(decoding: bytes[at...], as: UTF8.self), false)) }
        var rc: Int32 = 0
        for (t, sung) in pieces {
            if t.allSatisfy(\.isWhitespace) { continue }
            let one: Int32
            if sung {
                var m: Int32 = 0
                let t = Self.applyScoreTempo(t)
                one = speak(text: t, voice: def, sapiRate: sapiRate, semitones: semitones, singing: singing,
                            literalScore: true, mismatches: &m, onPCM: onPCM)
                mismatches?.pointee += m
            } else {
                one = speak(text: t, voice: def, sapiRate: sapiRate, semitones: semitones, onPCM: onPCM)
            }
            if one == 1 { return 1 }
            if one < 0 { rc = -1 }
        }
        return rc
    }

    // MARK: - Score tempo memory (Quinton, 2026-09-26 01:40)
    // VoiceOver sends each line of a message as its own request, so a "tempo 115" at the top of a score reached only
    // its first line. The last tempo seen in a sung score is remembered in process memory and put in front of the
    // next score that has none of its own; a new tempo replaces it, and it expires after `scoreTempoMemorySeconds`
    // without any sung score. Nothing is saved.
    static var scoreTempoMemorySeconds: TimeInterval = 60
    private static let tempoLock = NSLock()
    private static var lastTempo: String?
    private static var lastScoreAt = Date.distantPast

    /// The run as it should be sung: led by the remembered tempo when it has none; remembers its own when it does.
    static func applyScoreTempo(_ run: String, now: Date = Date()) -> String {
        tempoLock.lock(); defer { tempoLock.unlock() }
        let toks = run.split(whereSeparator: { $0 == " " || $0 == "\n" || $0 == "\t" || $0 == "\r" })
        var out = run
        if toks.count >= 2, toks[0].lowercased() == "tempo" {
            lastTempo = String(toks[1])
        } else if let t = lastTempo, now.timeIntervalSince(lastScoreAt) <= scoreTempoMemorySeconds {
            out = "tempo " + t + "\n" + run
        } else {
            lastTempo = nil
        }
        lastScoreAt = now
        return out
    }

    /// Tests: forget the remembered tempo.
    static func forgetScoreTempo() {
        tempoLock.lock(); lastTempo = nil; lastScoreAt = .distantPast; tempoLock.unlock()
    }

    /// Speaks SSML segments (SSML.segments) the way the extension does: each segment is its own engine
    /// call (trimmed of the engine's padding), and every pause is real silence - an explicit <break> at
    /// once, an element boundary just before the next segment's first audio (so a segment that turns
    /// out silent, or the end of the request, never leaves a gap). `onPCM` / `silence(ms)` return false
    /// to stop. Returns 0 done, 1 stopped, -1 if an engine call failed (the other segments still speak).
    /// Each segment speaks at its own pitch (SSML.Segment.semitones, from the markup) plus `semitones`.
    @discardableResult
    func speak(segments: [SSML.Segment], voice def: ClassicVoiceDef, sapiRate: Double, semitones: Double,
               singing: SingingSettings? = nil,
               onPCM: @escaping (UnsafeBufferPointer<Int16>) -> Bool,
               silence: @escaping (Int) -> Bool) -> Int32 {
        let speed = Self.effectiveSpeed(sapiRate: sapiRate, engine: def.engine)
        var pendingMs = 0          // boundary pause owed before the next audio
        var failed = false
        for seg in segments {
            if !seg.text.isEmpty {
                var ok = true
                let rc = speak(scored: seg.text, voice: def, sapiRate: sapiRate,
                               semitones: max(-12, min(12, semitones + seg.semitones)), singing: singing) { pcm in
                    if pendingMs > 0 {
                        let ms = pendingMs
                        pendingMs = 0
                        if !silence(ms) { ok = false; return false }
                    }
                    if !onPCM(pcm) { ok = false; return false }
                    return true
                }
                if rc == 1 || !ok { return 1 }
                if rc < 0 { failed = true }
            }
            let ms = seg.silenceMs(speed: speed)
            if seg.pause.hasBreak {
                if ms > 0 && !silence(ms) { return 1 }
            } else if ms > 0 {
                pendingMs = ms
            }
        }
        return failed ? -1 : 0
    }

    /// Whole-utterance synthesis for the app's Preview button: scores inside the text as for VoiceOver (speak(scored:)).
    /// With a SAPI 5 voice, a pasted text that is a whole score ("tempo 100", lines "word NOTE BEATS") is sung as
    /// written - switch or not. `mismatches`: words whose notes and syllables disagree.
    func synthesize(text: String, voice def: ClassicVoiceDef, sapiRate: Double,
                    semitones: Double = 0, singing: SingingSettings? = nil,
                    mismatches: UnsafeMutablePointer<Int32>? = nil) -> [Int16]? {
        var out: [Int16] = []
        let collect: (UnsafeBufferPointer<Int16>) -> Bool = { buf in out.append(contentsOf: buf); return true }
        let rc: Int32
        if def.engine == .neural {
            // As under VoiceOver (SSML.segments lineBreaks): each line its own engine call, the boundary pause between.
            let lines = text.split(whereSeparator: \.isNewline)
                .map { $0.replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
                    .trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
            var segs = lines.map { line -> SSML.Segment in
                var p = SSML.Pause()
                p.boundary = .sentence
                return SSML.Segment(text: line, pause: p)
            }
            if !segs.isEmpty { segs[segs.count - 1].pause = SSML.Pause() }
            rc = speak(segments: segs, voice: def, sapiRate: sapiRate, semitones: semitones, onPCM: collect,
                       silence: { ms in out.append(contentsOf: repeatElement(0, count: ms * Int(Self.sampleRate) / 1000)); return true })
        } else if def.engine == .sapi5, text.withCString({ cv_sing_is_score($0) }) != 0 {
            var sing = singing ?? SingingSettings()
            sing.enabled = true
            rc = speak(text: text, voice: def, sapiRate: sapiRate, semitones: semitones, singing: sing,
                       literalScore: true, mismatches: mismatches, onPCM: collect)
        } else {
            rc = speak(scored: text, voice: def, sapiRate: sapiRate, semitones: semitones, singing: singing,
                       mismatches: mismatches, onPCM: collect)
        }
        return rc < 0 ? nil : out
    }
}
