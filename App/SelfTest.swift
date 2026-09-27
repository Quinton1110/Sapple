//
//  SelfTest.swift
//  End-to-end check of the speech-provider extension THROUGH iOS, for development only.
//
//  Runs only when the app is launched with the argument -cvSelfTest (xcrun devicectl ... launch
//  com.quinton.classicvoices -- -cvSelfTest); add -cvSelfTestPauses for the pause cases only. It asks the system for our voices, then renders fixed test
//  sentences with AVSpeechSynthesizer.write - the same path VoiceOver's audio takes, so this exercises
//  the extension's SSML parsing, rate mapping, streaming and trimming - and saves each result as a WAV
//  in the app's Documents folder, where devicectl can copy it back to the Mac for transcription.
//  Prints one summary line per case to stdout. Never runs in normal use and never touches user text.
//

// The self-test drives iOS's own speech path and the idle timer; it is an iOS-only development tool.
#if os(iOS)

import AVFoundation
import UIKit

enum SelfTest {
    static var isRequested: Bool { ProcessInfo.processInfo.arguments.contains("-cvSelfTest") }

    private struct Case {
        let name: String
        let voiceSlug: String
        let rate: Float?          // nil = AVSpeechUtteranceDefaultSpeechRate
        let text: String
        var ssml = false          // text is SSML
        var cancelAfter: TimeInterval? = nil   // stopSpeaking(.immediate) this long after the first audio
    }

    // Newest first: the neural voices, Microsoft Anna, TruVoice, then the Microsoft SAPI 4 engine, then SAPI 5.
    // -cvSelfTestSAPI5: SAPI 5 only. -cvSelfTestTruVoice: TruVoice only. -cvSelfTestAnna: Anna only.
    // -cvSelfTestNeural: the neural voices only (with -cvSelfTestPauses: their pause cases only).
    // -cvSelfTestPauses: only the pause cases (with the SSML capture switched on for the run); with
    // -cvSelfTestAnna, only Anna's (six short cases: they finish before a locked phone suspends the app).
    private static let onlySAPI5 = ProcessInfo.processInfo.arguments.contains("-cvSelfTestSAPI5")
    private static let onlyTruVoice = ProcessInfo.processInfo.arguments.contains("-cvSelfTestTruVoice")
    private static let onlyAnna = ProcessInfo.processInfo.arguments.contains("-cvSelfTestAnna")
    private static let onlyPauses = ProcessInfo.processInfo.arguments.contains("-cvSelfTestPauses")
    private static let onlyNeural = ProcessInfo.processInfo.arguments.contains("-cvSelfTestNeural")
    private static let cases: [Case] = onlyPauses ? (onlyAnna ? pauseCases.filter { $0.voiceSlug == "anna" }
                                                     : onlyNeural ? neuralPauseCases : pauseCases)
        : onlySAPI5 ? sapi5Cases : onlyTruVoice ? truvoiceCases : onlyNeural ? neuralCases + neuralPauseCases
        : onlyAnna ? annaCases : neuralCases + annaCases + truvoiceCases + sapi4Cases + sapi5Cases + pauseCases
    /// the voices the in-process engine pass covers
    private static var engineVoices: [ClassicVoiceDef] {
        onlySAPI5 ? [] : onlyTruVoice ? TRUVOICE_VOICES : onlyAnna ? [ANNA_VOICE] : onlyNeural ? NEURAL_VOICES
            : NEURAL_VOICES + [ANNA_VOICE] + TRUVOICE_VOICES + SAPI4_VOICES
    }

    /// The neural voices (Microsoft's embedded Speech SDK, cvn_bridge): every voice once with the ~5 s sentence (rtf=),
    /// then numbers, hostile text, SSML, the rate range ends, the digit trim and cancel.
    private static let neuralCases: [Case] = NEURAL_VOICES.map { v in
        Case(name: "nv_" + v.neuralVoice.lowercased(), voiceSlug: v.slug, rate: nil,
             text: "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.")
    } + [
        Case(name: "nv_jenny_numbers", voiceSlug: "neural_jenny", rate: nil,
             text: "The meeting moved from March 3rd, 2021 to 12/25/2024 at 10:30 am, and it costs $56.78."),
        Case(name: "nv_guy_hostile", voiceSlug: "neural_guy", rate: nil,
             text: "Party time \u{1F600}\u{1F389}. Go \u{2192} next. \u{00DF}. \u{4F60}\u{597D}. <speak>tags</speak> C:\\Windows. This sentence must still be spoken."),
        Case(name: "nv_sonia_ssml", voiceSlug: "neural_sonia", rate: nil,
             text: #"<speak>One <break time="700ms"/> two. <say-as interpret-as="characters">abc</say-as>. <prosody pitch="+30%">This is higher.</prosody></speak>"#,
             ssml: true),
        Case(name: "nv_jenny_fast", voiceSlug: "neural_jenny", rate: AVSpeechUtteranceMaximumSpeechRate,
             text: "Hello from the Classic Voices extension. The quick brown fox jumps over the lazy dog."),
        Case(name: "nv_jenny_slow", voiceSlug: "neural_jenny", rate: AVSpeechUtteranceMinimumSpeechRate, text: "5"),
        Case(name: "nv_jenny_digit_default_after_slow", voiceSlug: "neural_jenny", rate: nil, text: "5"),
        Case(name: "nv_ryan_cancel", voiceSlug: "neural_ryan", rate: nil, text: alice, cancelAfter: 0),
        Case(name: "nv_ryan_after_cancel", voiceSlug: "neural_ryan", rate: nil, text: "This sentence must still be spoken."),
    ]

    /// The neural voices' pauses (the old NeuralVoice app ran these together): VoiceOver's shapes plus a line break, a
    /// list, a <break>; measure with tools/pause_gaps.py.
    private static let neuralPauseCases: [Case] = ["neural_jenny", "neural_ryan"].flatMap { v in
        (pauseFixtures + [("linebreak", "Meeting moved to Tuesday\nBring the slides", false),
                          ("list", "Shopping list: milk, eggs, bread, butter, and coffee.", false),
                          ("questions", "Is it ready? Yes. Are you sure? Quite sure.", false)]).map { f in
            Case(name: "pause_" + v + "_" + f.name, voiceSlug: v, rate: nil, text: f.text, ssml: f.ssml)
        }
    }

    /// VoiceOver's pause shapes (label | trait as adjacent <s> sentences, see SSML.swift) through the
    /// system path, for one voice per engine and for Apple's Alex as the reference; the WAVs are
    /// measured on the Mac (tools/pause_gaps.py).
    private static let pauseFixtures: [(name: String, text: String, ssml: Bool)] = [
        ("s_shape", "<speak><s>Speech</s><s>button</s></speak>", true),
        ("battery_s", "<speak><s>65% battery power</s><s>Charging</s></speak>", true),
        ("heading", "<speak><s>Classic Voices</s><s>Heading</s></speak>", true),
        ("comma", "Speech, button", false),
        ("break300", #"<speak>Speech<break time="300ms"/>button</speak>"#, true),
        ("digit", "5", false),
    ]
    private static let pauseCases: [Case] = ["sam", "sapi4_sam", "truvoice_adult_male_1", "anna", "apple:Alex"].flatMap { v in
        pauseFixtures.map { f in
            Case(name: "pause_" + (v.hasPrefix("apple:") ? "apple" : v) + "_" + f.name, voiceSlug: v, rate: nil,
                 text: f.text, ssml: f.ssml)
        }
    }
    private static var captureWasOn = false

    private static let alice = String(repeating: "Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to do. ", count: 6)

    /// Microsoft Anna (Engine/anna): the ~5 s sentence (rtf=), her own introduction, numbers and dates, then
    /// robustness, SSML, the rate range ends, the digit trim and cancel - the same shapes as the other engines.
    private static let annaCases: [Case] = [
        Case(name: "anna_default", voiceSlug: "anna", rate: nil,
             text: "The quick brown fox jumps over the lazy dog, and then it runs away into the forest."),
        Case(name: "anna_hello", voiceSlug: "anna", rate: nil, text: "Hello, my name is Microsoft Anna."),
        Case(name: "anna_numbers", voiceSlug: "anna", rate: nil,
             text: "The meeting moved from March 3rd, 2021 to 12/25/2024 at 10:30 am, and it costs $56.78."),
        Case(name: "anna_hostile", voiceSlug: "anna", rate: nil,
             text: "Party time \u{1F600}\u{1F389}. Go \u{2192} next. \u{00DF}. \u{4F60}\u{597D}. <rate speed=\"10\">tags</rate> C:\\Windows. This sentence must still be spoken."),
        Case(name: "anna_ssml", voiceSlug: "anna", rate: nil,
             text: #"<speak>One <break time="700ms"/> two. <say-as interpret-as="characters">abc</say-as>. <prosody pitch="+30%">This is higher.</prosody></speak>"#,
             ssml: true),
        Case(name: "anna_fast", voiceSlug: "anna", rate: AVSpeechUtteranceMaximumSpeechRate,
             text: "Hello from the Classic Voices extension. The quick brown fox jumps over the lazy dog."),
        Case(name: "anna_slow", voiceSlug: "anna", rate: AVSpeechUtteranceMinimumSpeechRate, text: "5"),
        Case(name: "anna_digit_default_after_slow", voiceSlug: "anna", rate: nil, text: "5"),
        Case(name: "anna_cancel", voiceSlug: "anna", rate: nil, text: alice, cancelAfter: 0),
        Case(name: "anna_after_cancel", voiceSlug: "anna", rate: nil, text: "This sentence must still be spoken."),
    ]

    /// L&H TruVoice (OpenTV, cvt_bridge): every voice once with the ~5 s sentence (rtf= per
    /// voice), then robustness, SSML, the rate range ends, the digit trim and cancel.
    private static let truvoiceCases: [Case] = TRUVOICE_VOICES.map { v in
        Case(name: "tv_" + v.slug.replacingOccurrences(of: "truvoice_adult_", with: ""), voiceSlug: v.slug, rate: nil,
             text: "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.")
    } + [
        Case(name: "tv_male_2_hostile", voiceSlug: "truvoice_adult_male_2", rate: nil,
             text: "Party time \u{1F600}\u{1F389}. Go \u{2192} next. \u{00DF}. \u{4F60}\u{597D}. C:\\Windows \\Pit=400\\ tags. This sentence must still be spoken."),
        Case(name: "tv_female_1_ssml", voiceSlug: "truvoice_adult_female_1", rate: nil,
             text: #"<speak>One <break time="700ms"/> two. <say-as interpret-as="characters">abc</say-as>. <prosody pitch="+30%">This is higher.</prosody></speak>"#,
             ssml: true),
        Case(name: "tv_male_1_fast", voiceSlug: "truvoice_adult_male_1", rate: AVSpeechUtteranceMaximumSpeechRate,
             text: "Hello from the Classic Voices extension. The quick brown fox jumps over the lazy dog."),
        Case(name: "tv_male_1_slow", voiceSlug: "truvoice_adult_male_1", rate: AVSpeechUtteranceMinimumSpeechRate, text: "5"),
        Case(name: "tv_male_1_digit_default_after_slow", voiceSlug: "truvoice_adult_male_1", rate: nil, text: "5"),
        Case(name: "tv_male_2_cancel", voiceSlug: "truvoice_adult_male_2", rate: nil, text: alice, cancelAfter: 0),
        Case(name: "tv_male_2_after_cancel", voiceSlug: "truvoice_adult_male_2", rate: nil, text: "This sentence must still be spoken."),
    ]

    private static let sapi5Cases: [Case] = [
        Case(name: "sam_default", voiceSlug: "sam", rate: nil,
             text: "Hello from the Classic Voices extension. The quick brown fox jumps over the lazy dog."),
        Case(name: "sam_fast", voiceSlug: "sam", rate: AVSpeechUtteranceMaximumSpeechRate,
             text: "Hello from the Classic Voices extension. The quick brown fox jumps over the lazy dog."),
        Case(name: "mike_hall_hostile", voiceSlug: "mike_hall", rate: nil,
             text: "Party time \u{1F600}\u{1F389}. Go \u{2192} next. \u{00DF}. \u{4F60}\u{597D}. This sentence must still be spoken."),
        Case(name: "mary_default", voiceSlug: "mary", rate: nil,
             text: "This is Mary, natively on iOS."),
        Case(name: "robosoft1_default", voiceSlug: "robosoft1", rate: nil,
             text: "This is RoboSoft One, natively on iOS."),
        Case(name: "mike_ssml", voiceSlug: "mike", rate: nil,
             text: #"<speak>One <break time="700ms"/> two. <say-as interpret-as="characters">abc</say-as>. <prosody pitch="+30%">This is higher.</prosody></speak>"#,
             ssml: true),
        // Rate probes. Reference: "5" at natural speed is about 0.58 s.
        Case(name: "sam_slow", voiceSlug: "sam", rate: AVSpeechUtteranceMinimumSpeechRate, text: "5"),
        Case(name: "sam_digit_default_after_slow", voiceSlug: "sam", rate: nil, text: "5"),
        Case(name: "sam_digit_ssml100_after_slow", voiceSlug: "sam", rate: nil,
             text: #"<speak><prosody rate="100%">5</prosody></speak>"#, ssml: true),
        Case(name: "sam_slow_again", voiceSlug: "sam", rate: AVSpeechUtteranceMinimumSpeechRate, text: "5"),
        Case(name: "sam_digit_rate051_after_slow", voiceSlug: "sam", rate: 0.51, text: "5"),
        Case(name: "sam_digit_rate049", voiceSlug: "sam", rate: 0.49, text: "5"),
        Case(name: "sam_cancel", voiceSlug: "sam", rate: nil, text: alice, cancelAfter: 0),
        Case(name: "sam_after_cancel", voiceSlug: "sam", rate: nil, text: "This sentence must still be spoken."),
        Case(name: "mary_after_cancel", voiceSlug: "mary", rate: nil, text: "This sentence must still be spoken."),
    ]

    /// Microsoft's SAPI 4 engine (decompiled to C, Engine/sapi4): every mode once with the same ~5 s sentence, so the
    /// summary line's rtf= is the phone's real-time factor per mode, then the robustness, SSML, rate
    /// and cancel paths on a few of them.
    private static let sapi4Cases: [Case] = SAPI4_VOICES.map { v in
        Case(name: "s4_" + v.slug.replacingOccurrences(of: "sapi4_", with: ""), voiceSlug: v.slug, rate: nil,
             text: "The quick brown fox jumps over the lazy dog, and then it runs away into the forest.")
    } + [
        Case(name: "s4_mike_hall_hostile", voiceSlug: "sapi4_mike_hall", rate: nil,
             text: "Party time \u{1F600}\u{1F389}. Go \u{2192} next. \u{00DF}. \u{4F60}\u{597D}. C:\\Windows \\Pit=400\\ tags. This sentence must still be spoken."),
        Case(name: "s4_mary_ssml", voiceSlug: "sapi4_mary", rate: nil,
             text: #"<speak>One <break time="700ms"/> two. <say-as interpret-as="characters">abc</say-as>. <prosody pitch="+30%">This is higher.</prosody></speak>"#,
             ssml: true),
        Case(name: "s4_sam_fast", voiceSlug: "sapi4_sam", rate: AVSpeechUtteranceMaximumSpeechRate,
             text: "Hello from the Classic Voices extension. The quick brown fox jumps over the lazy dog."),
        Case(name: "s4_sam_slow", voiceSlug: "sapi4_sam", rate: AVSpeechUtteranceMinimumSpeechRate, text: "5"),
        Case(name: "s4_sam_digit_default_after_slow", voiceSlug: "sapi4_sam", rate: nil, text: "5"),
        Case(name: "s4_sam_cancel", voiceSlug: "sapi4_sam", rate: nil, text: alice, cancelAfter: 0),
        Case(name: "s4_sam_after_cancel", voiceSlug: "sapi4_sam", rate: nil, text: "This sentence must still be spoken."),
    ]

    // A late didFinish / didCancel of the previous (cancelled) utterance must not complete the next case:
    // that is what made the case after a cancel report 0 samples (seen 2026-09-21 and 09-22).
    private final class Watcher: NSObject, AVSpeechSynthesizerDelegate {
        func speechSynthesizer(_ s: AVSpeechSynthesizer, didFinish u: AVSpeechUtterance) {
            DispatchQueue.main.async { SelfTest.ended(u, note: "ok") }
        }
        func speechSynthesizer(_ s: AVSpeechSynthesizer, didCancel u: AVSpeechUtterance) {
            DispatchQueue.main.async { SelfTest.ended(u, note: "cancelled") }
        }
    }
    private static var currentUtterance: AVSpeechUtterance?
    private static let watcher = Watcher()
    private static var cancelScheduled = false

    private static var synth: AVSpeechSynthesizer?
    private static var systemVoices: [AVSpeechSynthesisVoice] = []
    private static var index = 0
    private static var samples: [Float] = []
    private static var sampleRate: Double = 22050
    private static var started = Date()
    private static var finished = false

    static func run() {
        setvbuf(stdout, nil, _IONBF, 0)   // every line reaches devicectl even if the run is cut short
        // a run suspends as soon as the app leaves the foreground: at least keep auto-lock away meanwhile
        UIApplication.shared.isIdleTimerDisabled = true
        startHeartbeats()
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
        // Phase 1, in this process: the SAPI 4 engine itself on the phone's cores (no system speech
        // path), every mode, so the real-time factor is the engine's. Phase 2: the system path.
        if onlyPauses {
            captureWasOn = SSMLCapture.isEnabled
            if !captureWasOn { SSMLCapture.setEnabled(true, for: 120) }   // short: a run cut off must not leave it on
            print("CVSELFTEST capture on, container=\(SSMLCapture.container != nil ? "ok" : "MISSING")")
        }
        DispatchQueue.global(qos: .userInitiated).async {
            if !onlySAPI5 && !onlyPauses { engineCheck() }
            DispatchQueue.main.async {
                let ours = AVSpeechSynthesisVoice.speechVoices().filter { $0.identifier.hasPrefix(CLASSIC_VOICE_ID_PREFIX) }
                print("CVSELFTEST voices=\(ours.count) first=\(ours.first?.identifier ?? "-") name=\(ours.first?.name ?? "-")")
                if let a = ours.first(where: { $0.identifier.hasSuffix("." + ANNA_VOICE.slug) }) {
                    print("CVSELFTEST anna system voice: name=\(a.name) id=\(a.identifier) language=\(a.language) female=\(a.gender == .female)")
                } else {
                    print("CVSELFTEST anna system voice MISSING")
                }
                systemVoices = ours
                synth = AVSpeechSynthesizer()
                synth?.delegate = watcher
                index = 0
                next()
            }
        }
    }

    /// Main-thread and background heartbeats: if both stop, the app was suspended; if only the main
    /// one stops, the main thread is blocked.
    private static var beatsMain = 0
    private static var beatsBackground = 0
    private static let launched = Date()
    private static func startHeartbeats() {
        Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
            beatsMain += 1
            print(String(format: "CVSELFTEST heartbeat main=%d t=%.0fs case=%d", beatsMain, Date().timeIntervalSince(launched), index))
        }
        Thread.detachNewThread {
            while true {
                Thread.sleep(forTimeInterval: 5)
                beatsBackground += 1
                print(String(format: "CVSELFTEST heartbeat background=%d t=%.0fs", beatsBackground, Date().timeIntervalSince(launched)))
            }
        }
    }

    /// What jetsam counts (phys_footprint), in MB.
    private static func footprintMB() -> Double {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        }
        return kr == KERN_SUCCESS ? Double(info.phys_footprint) / 1_048_576 : -1
    }

    private static func engineCheck() {
        let text = "The quick brown fox jumps over the lazy dog, and then it runs away into the forest."
        for v in engineVoices {
            // cold: opening the engine + the first utterance; firstAudio = the call to its first PCM
            ClassicEngine.shared.releaseIdle()
            let mem0 = footprintMB()
            let t0 = Date()
            var firstAudio = -1.0
            var pcm: [Int16] = []
            let rc = ClassicEngine.shared.speak(text: text, voice: v, sapiRate: 0, semitones: 0) { buf in
                if firstAudio < 0 { firstAudio = Date().timeIntervalSince(t0) }
                pcm.append(contentsOf: buf)
                return true
            }
            let wall = Date().timeIntervalSince(t0)
            let mem1 = footprintMB()
            let secs = Double(pcm.count) / ClassicEngine.sampleRate
            let t1 = Date()
            let again = ClassicEngine.shared.synthesize(text: text, voice: v, sapiRate: 0)
            let warm = Date().timeIntervalSince(t1)
            print(String(format: "CVSELFTEST engine voice=%@ %@ audio=%.2fs first=%.3fs (incl. open) firstAudio=%.3fs warm=%.3fs rtf=%.1fx mem=+%.1fMB",
                         v.slug, rc < 0 || again == nil ? "FAIL" : "ok", secs, wall, firstAudio, warm, warm > 0 ? secs / warm : 0,
                         mem1 - mem0))
        }
        // cancel mid-utterance, then speak again on the same (pooled) engine - each engine kind
        for slug in (onlyTruVoice ? ["truvoice_adult_male_2"] : onlyAnna ? ["anna"] : ["anna", "truvoice_adult_male_2", "sapi4_sam"]) {
            guard let sam = CLASSIC_VOICES.first(where: { $0.slug == slug }) else { continue }
            var got = 0
            let rc = ClassicEngine.shared.speak(text: String(repeating: "Alice was beginning to get very tired. ", count: 20),
                                                voice: sam, sapiRate: 0, semitones: 0) { buf in
                got += buf.count
                return got < 22050
            }
            let after = ClassicEngine.shared.synthesize(text: "This sentence must still be spoken.", voice: sam, sapiRate: 0)
            print("CVSELFTEST engine cancel voice=\(slug) rc=\(rc) samplesBeforeStop=\(got) next=\(after?.count ?? -1)")
        }
    }

    private static func next() {
        guard index < cases.count else {
            if onlyPauses {
                if !captureWasOn { SSMLCapture.setEnabled(false) }
                print("CVSELFTEST capture switched \(captureWasOn ? "on by the user, left on" : "off"); the recording is in the extension's Documents/ssml-capture.txt")
            }
            print("CVSELFTEST all cases done")
            return
        }
        let c = cases[index]
        let voice: AVSpeechSynthesisVoice?
        if c.voiceSlug.hasPrefix("apple:") {
            // an Apple voice as the reference (Alex, Quinton's VoiceOver voice; else any en-US voice)
            let name = String(c.voiceSlug.dropFirst(6))
            voice = AVSpeechSynthesisVoice.speechVoices().first {
                $0.name == name && $0.language == "en-US" && !$0.identifier.contains(CLASSIC_VOICE_ID_PREFIX)
            } ?? AVSpeechSynthesisVoice(language: "en-US")
            print("CVSELFTEST case=\(c.name) apple voice=\(voice?.identifier ?? "-")")
        } else {
            // iOS re-prefixes provider voice identifiers, so match on the slug at the end.
            voice = systemVoices.first(where: { $0.identifier.hasSuffix("." + c.voiceSlug) })
        }
        guard let voice else {
            print("CVSELFTEST case=\(c.name) FAIL voice not found")
            index += 1
            next()
            return
        }
        guard let u = c.ssml ? AVSpeechUtterance(ssmlRepresentation: c.text) : AVSpeechUtterance(string: c.text) else {
            print("CVSELFTEST case=\(c.name) FAIL bad SSML")
            index += 1
            next()
            return
        }
        u.voice = voice
        currentUtterance = u
        print("CVSELFTEST start case=\(c.name)")
        cancelScheduled = false
        if let r = c.rate { u.rate = r }
        samples = []
        finished = false
        started = Date()
        let caseIndex = index
        synth?.write(u) { buffer in
            DispatchQueue.main.async { receive(buffer, caseIndex: caseIndex) }
        }
        if c.cancelAfter == 0 {
            cancelScheduled = true
            stopAt = 0
            synth?.stopSpeaking(at: .immediate)
        }
        // A case that never completes must not stall the rest.
        DispatchQueue.main.asyncAfter(deadline: .now() + 20) {
            if index == caseIndex && !finished { complete(caseIndex: caseIndex, note: "TIMEOUT") }
        }
    }

    private static func receive(_ buffer: AVAudioBuffer, caseIndex: Int) {
        guard caseIndex == index, !finished else { return }
        guard let pcm = buffer as? AVAudioPCMBuffer else { return }
        if pcm.frameLength == 0 {
            complete(caseIndex: caseIndex, note: "ok")
            return
        }
        sampleRate = pcm.format.sampleRate
        let n = Int(pcm.frameLength)
        if let after = cases[caseIndex].cancelAfter, !cancelScheduled {
            cancelScheduled = true
            DispatchQueue.main.asyncAfter(deadline: .now() + after) {
                if index == caseIndex && !finished {
                    stopAt = samples.count
                    synth?.stopSpeaking(at: .immediate)
                }
            }
        }
        if let f = pcm.floatChannelData {
            samples.append(contentsOf: UnsafeBufferPointer(start: f[0], count: n))
        } else if let i = pcm.int16ChannelData {
            samples.append(contentsOf: UnsafeBufferPointer(start: i[0], count: n).map { Float($0) / 32768 })
        }
    }

    private static var stopAt = -1

    fileprivate static func ended(_ u: AVSpeechUtterance, note: String) {
        guard u === currentUtterance else {
            print("CVSELFTEST (late \(note) callback of an earlier utterance ignored)")
            return
        }
        if !finished { complete(caseIndex: index, note: note) }
    }

    private static func complete(caseIndex: Int, note: String) {
        guard caseIndex < cases.count, !finished else { return }
        finished = true
        let c = cases[caseIndex]
        let secs = Double(samples.count) / sampleRate
        let peak = samples.reduce(0) { max($0, abs($1)) }
        let wall = Date().timeIntervalSince(started)
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("selftest-\(c.name).wav")
        let int16 = samples.map { Int16(max(-1, min(1, $0)) * 32767) }
        try? PreviewPlayer.wavData(int16, sampleRate: Int(sampleRate)).write(to: url)
        print(String(format: "CVSELFTEST case=%@ %@ samples=%d rate=%.0f audio=%.2fs peak=%.3f wall=%.2fs rtf=%.1fx%@",
                     c.name, note, samples.count, sampleRate, secs, peak, wall, wall > 0 ? secs / wall : 0,
                     stopAt >= 0 ? " (stop requested at sample \(stopAt))" : ""))
        stopAt = -1
        index += 1
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { next() }
    }
}

#endif  // os(iOS)
