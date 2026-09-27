// Mac test harness for the extension's SSML handling and pauses - the same Swift (Shared/SSML.swift,
// SSMLCapture.swift, ClassicEngine.swift ...) and the same C engines and bridges the iOS targets compile.
//
//   ssml_test parse              parser fixtures (VoiceOver-style SSML shapes) + the capture's privacy
//   ssml_test capture            the capture switch / record / privacy / clear on this Mac user's real group
//                                container (manual: leaves ~/Library/Preferences/group.com.quinton.classicvoices.plist)
//   ssml_test render OUTDIR      every fixture through one voice per engine, before (the old parser:
//                                tags -> spaces, split only at <break>) and after; WAVs, gap table, checks
//
// `make ssml-test` runs parse and render (CV_DATA_ROOT = the repo root, for VoiceData/, SAPI4Voices/, AnnaVoice/ and
// OneCoreVoice/).

import Foundation
import AVFoundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    print((ok ? "ok   " : "FAIL ") + what)
    if !ok { failures += 1 }
}

// MARK: - Fixtures

struct Fixture {
    let name: String
    let ssml: String
}

/// VoiceOver-style request shapes. VoiceOver sends label / trait / value as adjacent <s> sentences with
/// nothing between them (RHVoice iOS port capture, iOS 27), wrapped in <prosody> and <lang>.
let fixtures: [Fixture] = [
    Fixture(name: "s_shape", ssml: "<speak><s>Speech</s><s>button</s></speak>"),
    Fixture(name: "vo_shape", ssml: #"<speak><prosody rate="100.0%" pitch="+0%"><s><lang xml:lang="en-US">Speech</lang></s><s><lang xml:lang="en-US">Button</lang></s></prosody></speak>"#),
    Fixture(name: "comma", ssml: "Speech, button"),
    Fixture(name: "break300", ssml: #"<speak>Speech<break time="300ms"/>button</speak>"#),
    Fixture(name: "battery_s", ssml: "<speak><s>65% battery power</s><s>Charging</s></speak>"),
    Fixture(name: "battery_comma", ssml: "<speak>65% battery power, charging</speak>"),
    Fixture(name: "heading", ssml: "<speak><s>Classic Voices</s><s>Heading</s></speak>"),
    Fixture(name: "digit", ssml: #"<speak><prosody rate="100%">5</prosody></speak>"#),
    Fixture(name: "digits_s", ssml: "<speak><s>5</s><s>0</s><s>3</s></speak>"),
    Fixture(name: "long", ssml: "<speak>Alice was beginning to get very tired of sitting by her sister on the bank, and of having nothing to do. Once or twice she had peeped into the book her sister was reading.</speak>"),
    Fixture(name: "mixed", ssml: #"<speak>One <break time="700ms"/> two. <say-as interpret-as="characters">abc</say-as>. <prosody pitch="+30%">This is higher.</prosody></speak>"#),
]

// MARK: - The old parser (before 2026-09-22), for parity and "before" renders

func legacySegments(_ ssml: String) -> [(text: String, silenceMs: Int)] {
    let s = SSML.spellOutSayAs(ssml)
    let ns = s as NSString
    let re = try! NSRegularExpression(pattern: #"<break\b([^>]*?)/?\s*>"#, options: [.caseInsensitive])
    var out: [(String, Int)] = []
    var last = 0
    for m in re.matches(in: s, range: NSRange(location: 0, length: ns.length)) {
        let chunk = ns.substring(with: NSRange(location: last, length: m.range.location - last))
        let attrs = m.range(at: 1).location != NSNotFound ? ns.substring(with: m.range(at: 1)) : ""
        out.append((SSML.plainText(chunk), SSML.breakMs(attrs)))
        last = m.range.location + m.range.length
    }
    let tail = SSML.plainText(ns.substring(from: last))
    if !tail.isEmpty { out.append((tail, 0)) }
    return out
}

// MARK: - parse

func texts(_ segs: [SSML.Segment]) -> [String] { segs.map(\.text) }
func gaps(_ segs: [SSML.Segment], speed: Double = 1) -> [Int] { segs.map { $0.silenceMs(speed: speed) } }

func runParse() {
    var s = SSML.segments("<speak><s>Speech</s><s>button</s></speak>")
    check(texts(s) == ["Speech", "button"] && gaps(s) == [SSML.phraseGapMs, 0], "<s>Speech</s><s>button</s> -> two segments, \(gaps(s)) ms")
    s = SSML.segments(fixtures[1].ssml)
    check(texts(s) == ["Speech", "Button"] && gaps(s) == [SSML.phraseGapMs, 0], "VoiceOver shape with <prosody>/<lang> -> Speech | Button, \(gaps(s))")
    check(SSML.ratePercent(fixtures[1].ssml) == 100, "rate still read from <prosody>")
    s = SSML.segments("Speech, button")
    check(texts(s) == ["Speech, button"] && gaps(s) == [0], "plain \"Speech, button\" stays one segment (the engines pause at the comma)")
    s = SSML.segments(#"<speak>Speech<break time="300ms"/>button</speak>"#)
    check(texts(s) == ["Speech", "button"] && gaps(s) == [300, 0] && gaps(s, speed: 3) == [300, 0], "break time=300ms: exactly 300 ms at any speed")
    s = SSML.segments("<speak><s>65% battery power</s><s>Charging</s></speak>")
    check(texts(s) == ["65% battery power", "Charging"] && gaps(s) == [SSML.phraseGapMs, 0], "battery label | value split")
    s = SSML.segments("<speak>65% battery power, charging</speak>")
    check(texts(s) == ["65% battery power, charging"], "battery with a comma: one segment")
    s = SSML.segments("<speak><s>Only</s></speak>")
    check(texts(s) == ["Only"] && gaps(s) == [0], "single <s>: no pause before or after")
    s = SSML.segments("<speak><s>5</s><s>0</s><s>3</s></speak>")
    check(texts(s) == ["5 0 3"] && gaps(s) == [0], "digit-only <s> neighbours stay one call, as before: \(texts(s))")
    s = SSML.segments(#"<speak>5<break time="200ms"/>5</speak>"#)
    check(texts(s) == ["5", "5"] && gaps(s) == [200, 0], "digits with an explicit break keep it")
    s = SSML.segments("<speak><s>Hello there.</s><s>Next one</s></speak>")
    check(gaps(s) == [SSML.sentenceGapMs, 0], "after a full stop: sentence pause \(gaps(s))")
    s = SSML.segments("<speak><s>Is it \u{201C}done?\u{201D}</s><s>Yes</s></speak>")
    check(gaps(s) == [SSML.sentenceGapMs, 0], "after ? inside closing quotes: sentence pause")
    s = SSML.segments("<speak><p>One</p><p>Two</p></speak>")
    check(texts(s) == ["One", "Two"] && gaps(s) == [SSML.paragraphGapMs, 0], "paragraphs")
    s = SSML.segments(#"<speak><s>A b</s><break time="50ms"/><s>C d</s></speak>"#)
    check(gaps(s) == [50, 0], "an explicit break between sentences wins over the boundary")
    s = SSML.segments(#"<speak><s>A b</s><break strength="none"/><s>C d</s></speak>"#)
    check(texts(s) == ["A b", "C d"] && gaps(s) == [0, 0], "strength=none between sentences: no pause")
    s = SSML.segments(#"<speak>a b<break strength="medium"/>c d</speak>"#)
    check(gaps(s) == [350, 0] && gaps(s, speed: 2) == [Int((350 * pow(2, -0.9)).rounded()), 0], "strength scales with speed: \(gaps(s, speed: 2))")
    s = SSML.segments("<speak><s>Speech</s><s>button</s></speak>")
    check(gaps(s, speed: 2)[0] == 80 && gaps(s, speed: 3)[0] == 56 && gaps(s, speed: 7.2)[0] == SSML.minPhraseGapMs
          && gaps(s, speed: 1.0 / 3)[0] == 233, "boundary pause vs speed 1/3,1,2,3,7.2: \([1.0 / 3, 1, 2, 3, 7.2].map { gaps(s, speed: $0)[0] })")
    s = SSML.segments(#"<speak><break time="1s"/>Hi</speak>"#)
    check(texts(s) == ["", "Hi"] && gaps(s) == [1000, 0], "leading break: leading silence, as before")
    s = SSML.segments(#"<speak>Hi<break time="1s"/></speak>"#)
    check(texts(s) == ["Hi"] && gaps(s) == [1000], "trailing break kept, as before")
    s = SSML.segments(#"<speak><say-as interpret-as="characters">abc</say-as></speak>"#)
    check(texts(s) == ["a, b, c"], "say-as characters still spelled")
    s = SSML.segments("<speak><s>Tom &amp; Jerry &lt;3</s><s>Heading</s></speak>")
    check(texts(s) == ["Tom & Jerry <3", "Heading"], "entities decoded per segment: \(texts(s))")
    s = SSML.segments(#"<?xml version="1.0"?><speak><!-- a > b --><S>X y</S><S>Z w</S></speak>"#)
    check(texts(s) == ["X y", "Z w"], "xml declaration, comment with '>', upper-case tags: \(texts(s))")
    s = SSML.segments("<speak></speak>")
    check(s.isEmpty, "empty request: no segments (never stand-in text)")
    s = SSML.segments("<speak><s> </s><s></s></speak>")
    check(s.isEmpty, "whitespace-only sentences: no segments")
    s = SSML.segments("a <b> c")
    check(texts(s) == ["a c"], "a stray '<...>' is stripped as before: \(texts(s))")

    // Singing mode (2026-09-25): a song score written in a message reaches the provider intact in the shapes iOS
    // sends (a plain-string utterance as <speak>text</speak>, VoiceOver's <s>/<prosody>/<lang>), line breaks and all
    s = SSML.segments("<speak>Sing along: twin-kle C4 1 C4 1\ntwin-kle G4 1 G4 1 okay</speak>")
    check(texts(s).count == 1 && texts(s)[0].contains("C4 1 C4 1") && texts(s)[0].contains("G4 1 G4 1"), "a score survives <speak>: \(texts(s))")
    s = SSML.segments(#"<speak><prosody rate="100%"><s><lang xml:lang="en-US">Tom &amp; Jerry sing twin-kle C4 1 C4 1 twin-kle G4 1 G4 1</lang></s></prosody></speak>"#)
    check(texts(s) == ["Tom & Jerry sing twin-kle C4 1 C4 1 twin-kle G4 1 G4 1"], "a score survives VoiceOver's <s>/<prosody>/<lang> shape: \(texts(s))")
    if let u = AVSpeechUtterance(ssmlRepresentation: "<speak>Hi star G4 2 `x &amp; la</speak>") {
        check(u.speechString == "Hi star G4 2 `x & la", "AVSpeechUtterance(ssmlRepresentation:) keeps the text: \(u.speechString)")
    }

    // Scoped pitch (2026-09-24): shapes captured on the phone from VoiceOver with "Use Pitch Changes"
    // on. Only the container name is lowered; the old code applied the first pitch to everything.
    func pitches(_ segs: [SSML.Segment]) -> [Double] { segs.map { ($0.semitones * 100).rounded() / 100 } }
    let low = (12 * log2(0.85) * 100).rounded() / 100
    s = SSML.segments(#"<speak><prosody rate="160%" volume="-3dB"><s><lang xml:lang="en-US"><prosody pitch="-14.999996%">Message header</prosody></lang></s><s><lang xml:lang="en-US">From Maybe Shaf - 12 minutes ago</lang></s></prosody></speak>"#)
    check(texts(s) == ["Message header", "From Maybe Shaf - 12 minutes ago"] && pitches(s) == [low, 0],
          "SAPI 4 shape (no outer pitch): only the label is lowered \(pitches(s))")
    s = SSML.segments(#"<speak><prosody pitch="+0.0%" rate="130%"><s><lang xml:lang="en-US"><prosody pitch="-14.999996%">Message content</prosody></lang></s><s><lang xml:lang="en-US">The rest reads normally.</lang></s></prosody></speak>"#)
    check(texts(s) == ["Message content", "The rest reads normally."] && pitches(s) == [low, 0],
          "TruVoice shape (outer pitch first): the label is still lowered \(pitches(s))")
    s = SSML.segments(#"<speak><s><prosody pitch="-15%">Dock</prosody>, Messages</s></speak>"#)
    check(texts(s) == ["Dock", ", Messages"] && pitches(s) == [low, 0] && gaps(s) == [0, 0],
          "pitch change inside a sentence: split with no pause \(texts(s)) \(pitches(s))")
    s = SSML.segments(#"<speak><prosody pitch="+20%">typed</prosody></speak>"#)
    check(texts(s) == ["typed"] && pitches(s) == [(12 * log2(1.2) * 100).rounded() / 100], "whole-request pitch still applies \(pitches(s))")
    s = SSML.segments(#"<speak><prosody pitch="+2st"><prosody pitch="-1st">a</prosody> b</prosody> c</speak>"#)
    check(texts(s) == ["a", "b", "c"] && pitches(s) == [1, 2, 0], "nested relative pitch adds, and ends with its element \(pitches(s))")
    s = SSML.segments(#"<speak><prosody pitch="+2st"><prosody pitch="low">a</prosody></prosody></speak>"#)
    check(pitches(s) == [low], "absolute pitch inside relative replaces it \(pitches(s))")
    s = SSML.segments(#"<speak><s><prosody pitch="-15%">5</prosody></s><s>0</s></speak>"#)
    check(texts(s) == ["5", "0"], "digits at different pitches are not merged \(texts(s))")

    // Parity: without <s>/<p>, the new parser gives exactly the old text and silence.
    let legacyInputs = fixtures.map(\.ssml).filter { !$0.contains("<s>") && !$0.contains("<p>") } + [
        #"<speak><break time="1s"/>Hi</speak>"#, #"<speak>One <break time="700ms"/> <break time="300ms"/> two</speak>"#,
        "<speak><prosody rate=\"130.0%\">Hello &amp; goodbye</prosody></speak>", "a <b> c", "",
        #"<speak>x<break/>y<break strength="x-strong"/>z<break time="3s"/>w</speak>"#,
    ]
    for input in legacyInputs {
        let old = legacySegments(input)
        let new = SSML.segments(input)
        // The old parser gave consecutive breaks their own empty segments; the silence adds up the same.
        var merged: [(String, Int)] = []
        for (t, ms) in old {
            if t.isEmpty, !merged.isEmpty { merged[merged.count - 1].1 += ms } else { merged.append((t, ms)) }
        }
        // A pitch change now splits a segment with no pause; for parity, join those back.
        var joined: [(text: String, ms: Int)] = []
        var glue = false
        for seg in new {
            if glue, !joined.isEmpty { joined[joined.count - 1].text += " " + seg.text; joined[joined.count - 1].ms = seg.silenceMs(speed: 1) }
            else { joined.append((seg.text, seg.silenceMs(speed: 1))) }
            glue = seg.pause.isEmpty
        }
        let same = merged.count == joined.count && zip(merged, joined).allSatisfy { $0.0 == $1.text && $0.1 == $1.ms }
        check(same, "parity with the old parser: \(input.prefix(60))")
    }

    // The capture keeps structure, never words.
    let secret = #"<speak><prosody rate="130.0%" pitch="+10%"><s><lang xml:lang="en-US">Secret Words</lang></s><s>Button</s></prosody><sub alias="hidden">x</sub><break time="250ms"/><say-as interpret-as="characters">qz</say-as>65% power, charging &amp; more</speak>"#
    let sk = SSMLCapture.skeleton(secret)
    print("     skeleton: \(sk)")
    let leaked = ["Secret", "Words", "Button", "hidden", "ecr", "utto", "qz", "power", "charging", "more"].filter { sk.contains($0) }
    check(leaked.isEmpty, "capture skeleton holds none of the words (leaked: \(leaked))")
    check(sk.contains(#"rate="130.0%""#) && sk.contains(#"time="250ms""#) && sk.contains(#"xml:lang="en-US""#)
          && sk.contains("«12:a_a»") && sk.contains("alias=«6»") && sk.contains("«26:9%_a,_a_&_a»"),
          "capture skeleton keeps tags, prosody, break time, lengths and classes")
    check(SSMLCapture.textClass("65% battery power, charging") == "9%_a_a,_a", "text class of the battery status")

    // Line breaks (the neural voices only): a sentence boundary with the phrase pause; everything else unchanged.
    s = SSML.segments("<speak>First line\nSecond line</speak>", lineBreaks: true)
    check(texts(s) == ["First line", "Second line"] && gaps(s) == [SSML.phraseGapMs, 0], "lineBreaks: two lines -> two segments \(gaps(s))")
    s = SSML.segments("<speak>Done.\r\n\r\nNext one</speak>", lineBreaks: true)
    check(texts(s) == ["Done.", "Next one"] && gaps(s) == [SSML.sentenceGapMs, 0], "lineBreaks: CRLF blank line after a full stop -> sentence pause \(gaps(s))")
    s = SSML.segments("\n\nOnly line\n", lineBreaks: true)
    check(texts(s) == ["Only line"] && gaps(s) == [0], "lineBreaks: no pause before the first or after the last line")
    for input in fixtures.map(\.ssml) + ["<speak>First line\nSecond line</speak>"] {
        check(SSML.segments(input) == SSML.segments(input, lineBreaks: false), "default is lineBreaks off: \(input.prefix(40))")
    }
    s = SSML.segments("<speak>First line\nSecond line</speak>")
    check(texts(s) == ["First line Second line"], "other engines: a line break is still a space")

    // Voice names (Quinton, 2026-09-27): no "Microsoft", TruVoice by name alone, no duplicates, slugs untouched.
    check(VoiceCatalog.duplicateDisplayNames.isEmpty, "no two voices share a display name \(VoiceCatalog.duplicateDisplayNames)")
    check(!CLASSIC_VOICES.contains { $0.display.contains("Microsoft") }, "no display name contains \"Microsoft\"")
    check(Set(CLASSIC_VOICES.map(\.slug)).count == CLASSIC_VOICES.count, "slugs unique")
    func name(_ slug: String) -> String { CLASSIC_VOICES.first { $0.slug == slug }?.display ?? "?" }
    check(name("sam") == "Sam (SAPI 5)" && name("sapi4_sam") == "Sam (SAPI 4)" && name("anna") == "Anna"
          && name("onecore_mark_happy") == "Mark Happy" && name("truvoice_adult_male_1") == "Peter"
          && name("truvoice_adult_male_8") == "Alex" && name("neural_jenny") == "Jenny" && name("neural_ryan") == "Ryan"
          && name("sapi4_mike_telephone") == "Mike for Telephone (SAPI 4)",
          "names: Sam (SAPI 5), Sam (SAPI 4), Anna, Mark Happy, Peter, Alex, Jenny, Ryan, Mike for Telephone (SAPI 4)")
    var oldSlugs: [String] = ["sam", "mike", "mary", "mike_hall", "mike_stadium", "mike_space", "mary_hall", "mary_stadium",
                              "mary_space", "robosoft1", "robosoft2", "robosoft3", "robosoft4", "robosoft6", "male_whisper",
                              "female_whisper", "anna"]
    for v in ["david", "zira", "mark"] {
        for e in ["", "_happy", "_sad", "_angry"] { oldSlugs.append("onecore_" + v + e) }
    }
    oldSlugs += ["onecore_hazel", "onecore_george", "onecore_susan", "onecore_eva", "onecore_eva_happy", "onecore_eva_sad",
                 "onecore_eva_angry", "onecore_sarah"]
    let sapi4Modes: [String] = ["sam", "mike", "mary", "mike_telephone", "mary_telephone", "mike_hall", "mike_stadium",
                                "mike_space", "mary_hall", "mary_stadium", "mary_space", "robosoft1", "robosoft2", "robosoft3",
                                "robosoft4", "robosoft5", "robosoft6", "male_whisper", "female_whisper"]
    for m in sapi4Modes { oldSlugs.append("sapi4_" + m) }
    for n in 1...8 { oldSlugs.append("truvoice_adult_male_\(n)") }
    oldSlugs += ["truvoice_adult_female_1", "truvoice_adult_female_2"]
    check(CLASSIC_VOICES.map(\.slug).prefix(oldSlugs.count).elementsEqual(oldSlugs)
          && CLASSIC_VOICES.dropFirst(oldSlugs.count).allSatisfy { $0.engine == .neural },
          "every earlier slug unchanged and in the same order (\(oldSlugs.count)), the neural voices after them")
    print("     voices: \(CLASSIC_VOICES.count); neural: \(NEURAL_VOICES.map(\.display))")
}

// MARK: - render

struct Render {
    var pcm: [Int16] = []
    var rc: Int32 = 0
}

func renderNew(_ ssml: String, _ v: ClassicVoiceDef, rate: Double) -> Render {
    ClassicEngine.shared.releaseIdle()   // every render from a fresh engine
    var r = Render()
    var pcm: [Int16] = []
    let segs = SSML.segments(ssml)
    r.rc = ClassicEngine.shared.speak(segments: segs, voice: v, sapiRate: rate, semitones: 0,
                                      onPCM: { buf in pcm.append(contentsOf: buf); return true },
                                      silence: { ms in pcm.append(contentsOf: repeatElement(0, count: Int(Double(ms) / 1000 * ClassicEngine.sampleRate))); return true })
    r.pcm = pcm
    return r
}

func renderLegacy(_ ssml: String, _ v: ClassicVoiceDef, rate: Double) -> Render {
    ClassicEngine.shared.releaseIdle()
    var r = Render()
    for (text, ms) in legacySegments(ssml) {
        if !text.isEmpty {
            let rc = ClassicEngine.shared.speak(text: text, voice: v, sapiRate: rate, semitones: 0) { buf in
                r.pcm.append(contentsOf: buf); return true
            }
            if rc != 0 { r.rc = rc }
        }
        r.pcm.append(contentsOf: repeatElement(0, count: Int(Double(ms) / 1000 * ClassicEngine.sampleRate)))
    }
    return r
}

/// Longest silent run (ms) between the first and last sound: 5 ms frames more than 40 dB below the
/// loudest frame (and below -42 dBFS) - the same rule as tools/pause_gaps.py.
func longestGapMs(_ s: [Int16]) -> Int {
    let fl = 110
    var db: [Double] = []
    var i = 0
    while i + fl <= s.count {
        var mean = 0.0
        for k in i..<(i + fl) { mean += Double(s[k]) }
        mean /= Double(fl)
        var e = 0.0
        for k in i..<(i + fl) { let d = Double(s[k]) - mean; e += d * d }
        db.append(20 * log10(sqrt(e / Double(fl)) + 1e-9))
        i += fl
    }
    guard let peak = db.max() else { return 0 }
    let thr = max(peak - 40, 20 * log10(250.0))
    let loud = db.map { $0 > thr }
    guard let first = loud.firstIndex(of: true), let last = loud.lastIndex(of: true) else { return 0 }
    var best = 0, run = 0
    for k in first...last {
        if loud[k] { best = max(best, run); run = 0 } else { run += 1 }
    }
    return best * 5
}

func writeWav(_ pcm: [Int16], _ path: String) {
    var d = Data()
    func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    d.append(contentsOf: Array("RIFF".utf8)); u32(UInt32(36 + pcm.count * 2)); d.append(contentsOf: Array("WAVEfmt ".utf8))
    u32(16); u16(1); u16(1); u32(22050); u32(44100); u16(2); u16(16)
    d.append(contentsOf: Array("data".utf8)); u32(UInt32(pcm.count * 2))
    pcm.withUnsafeBytes { d.append(contentsOf: $0) }
    try? d.write(to: URL(fileURLWithPath: path))
}

func runRender(_ outDir: String) {
    try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
    let voices = ["sam", "mike_hall", "sapi4_sam", "sapi4_mary", "truvoice_adult_male_1", "truvoice_adult_female_1", "anna",
                  "onecore_david", "onecore_zira", "onecore_mark_happy", "onecore_hazel", "onecore_george", "onecore_susan",
                  "onecore_eva", "onecore_eva_happy", "onecore_sarah", "neural_jenny", "neural_sonia"]
        .compactMap { slug in CLASSIC_VOICES.first { $0.slug == slug } }
    // natural speed, and VoiceOver's slider at 75% (1.5x Samantha's "fast"): SAPI rate 9
    for (label, rate) in [("1x", 0.0), ("vo75", SpeechRate.sapiRate(fromFraction: 0.75))] {
        print(String(format: "\n== %@ (SAPI rate %.1f)  gap = longest silence inside the utterance, ms; dur = ms", label, rate))
        print("voice                    fixture        before gap/dur    after gap/dur   pcm")
        for v in voices {
            let speed = ClassicEngine.effectiveSpeed(sapiRate: rate, engine: v.engine)
            for f in fixtures {
                let before = renderLegacy(f.ssml, v, rate: rate)
                let after = renderNew(f.ssml, v, rate: rate)
                let tag = "\(v.slug)_\(f.name)_\(label)"
                writeWav(before.pcm, "\(outDir)/\(tag)_before.wav")
                writeWav(after.pcm, "\(outDir)/\(tag)_after.wav")
                let gb = longestGapMs(before.pcm), ga = longestGapMs(after.pcm)
                let db = before.pcm.count * 1000 / 22050, da = after.pcm.count * 1000 / 22050
                let identical = before.pcm == after.pcm
                print(String(format: "%-24@ %-14@ %4d / %5d     %4d / %5d   %@", v.slug, f.name, gb, db, ga, da,
                             identical ? "identical" : "changed"))
                check(before.rc == 0 && after.rc == 0, "\(tag): rc before \(before.rc) after \(after.rc)")
                let segs = SSML.segments(f.ssml)
                // a scoped <prosody pitch> now changes that part's pitch (the old renders spoke all at 0)
                let scopedPitch = Set(segs.map(\.semitones)).count > 1 || segs.contains { $0.semitones != 0 }
                // pitch changes split with no pause; only splits that carry a pause count here
                let split = segs.filter { !$0.pause.isEmpty }.count + 1 > legacySegments(f.ssml).count
                    && segs.count > legacySegments(f.ssml).count && !scopedPitch
                if scopedPitch {
                    check(!identical, "\(tag): scoped pitch now reaches the engine (changed output)")
                } else if !split {
                    check(identical, "\(tag): same segments as before -> bit-identical output")
                } else {
                    // label | trait now has a real pause: at least the calibrated boundary silence
                    let want = SSML.segments(f.ssml)[0].silenceMs(speed: speed)
                    check(ga >= want && ga > gb, "\(tag): boundary gap \(gb) -> \(ga) ms (inserted \(want) ms)")
                }
            }
            // a lone digit: never slower than before, and identical to a direct engine call
            var direct: [Int16] = []
            ClassicEngine.shared.releaseIdle()
            _ = ClassicEngine.shared.speak(text: "5", voice: v, sapiRate: rate, semitones: 0) { direct.append(contentsOf: $0); return true }
            let d = renderNew(#"<speak><prosody rate="100%">5</prosody></speak>"#, v, rate: rate)
            check(d.pcm == direct, "\(v.slug) \(label): single digit identical to the direct engine call (\(direct.count * 1000 / 22050) ms)")
        }
    }
}

// MARK: - singing mode (render): through speak(segments:), the extension's path

func renderSing(_ ssml: String, _ v: ClassicVoiceDef, _ sing: SingingSettings?) -> Render {
    ClassicEngine.shared.releaseIdle()
    var r = Render()
    var pcm: [Int16] = []
    r.rc = ClassicEngine.shared.speak(segments: SSML.segments(ssml), voice: v, sapiRate: 0, semitones: 0, singing: sing,
                                      onPCM: { buf in pcm.append(contentsOf: buf); return true },
                                      silence: { ms in pcm.append(contentsOf: repeatElement(0, count: Int(Double(ms) / 1000 * ClassicEngine.sampleRate))); return true })
    r.pcm = pcm
    return r
}

func runSingScores(_ outDir: String) {
    try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
    var on = SingingSettings(); on.enabled = true
    var off = on; off.enabled = false
    let sam = CLASSIC_VOICES.first { $0.slug == "sam" }!
    let song = "twin-kle C4 1 C4 1 twin-kle G4 1 G4 1 lit-tle A4 1 A4 1 star G4 2"
    // 1. ordinary text, including near-scores and backquotes: switch on = switch off = no settings, bit-identical,
    //    for every engine's voices in the set
    let plain = ["<speak>Hello, my name is Sam. Is that not stylish?</speak>", fixtures[1].ssml,
                 "<speak><s>65% battery power</s><s>Charging</s></speak>", "<speak>It costs `5 and `sing1 quoted` text ``.</speak>",
                 "<speak>Flight B6 2 is boarding. Room C4 1. Seats A1 2 and B2 3. e4 e5 Nf3 Nc6 Bb5 a6.</speak>"]
    for slug in ["sam", "mike_hall", "robosoft1", "male_whisper", "sapi4_sam", "truvoice_adult_male_1", "anna", "onecore_david", "onecore_sarah"] {
        guard let v = CLASSIC_VOICES.first(where: { $0.slug == slug }) else { continue }
        for (i, x) in plain.enumerated() {
            let a = renderSing(x, v, nil), b = renderSing(x, v, on), c = renderSing(x, v, off)
            check(a.rc == 0 && !a.pcm.isEmpty && a.pcm == b.pcm && a.pcm == c.pcm,
                  "\(slug) ordinary text \(i): switch on = switch off = before (\(a.pcm.count) samples)")
        }
    }
    // 2. a score with the switch off, and on every non-SAPI 5 voice: spoken as text, exactly as before
    let withScore = "<speak>Here we go: \(song) and that was it.</speak>"
    for slug in ["sam", "sapi4_sam", "truvoice_adult_male_1", "anna", "onecore_eva", "onecore_sarah"] {
        guard let v = CLASSIC_VOICES.first(where: { $0.slug == slug }) else { continue }
        let t = renderSing(withScore, v, v.engine == .sapi5 ? off : on), p = renderSing(withScore, v, nil)
        check(t.rc == 0 && !t.pcm.isEmpty && t.pcm == p.pcm, "\(slug) \(v.engine == .sapi5 ? "switch off" : "switch on, not SAPI 5"): the score is spoken as text, as before")
    }
    // 3. SAPI 5, switch on: sung mid-sentence, several runs, lines joined by VoiceOver, tempo, an <s> split
    func dur(_ r: Render) -> String { String(format: "%.2f s", Double(r.pcm.count) / 22050) }
    let cases: [(String, String)] = [
        ("midsentence", "<speak>Here we go: \(song) and that was it.</speak>"),
        ("two_runs", "<speak>First: how F4 1 I F4 1 won-der E4 1 E4 1. Then: what D4 1 you D4 1 are C4 2. Done.</speak>"),
        ("tempo", "<speak>Slowly now tempo 60 star G4 2 - 1 star C4 2 thanks</speak>"),
        ("lines", "<speak>Song:\ntwin-kle C4 1 C4 1\ntwin-kle G4 1 G4 1\n- 1\nstar G4 2\nEnd.</speak>"),
        ("only_score", "<speak>\(song)</speak>")]
    for (name, x) in cases {
        let r = renderSing(x, sam, on), spoken = renderSing(x, sam, off)
        writeWav(r.pcm, "\(outDir)/sam_score_\(name).wav")
        check(r.rc == 0 && !r.pcm.isEmpty && r.pcm != spoken.pcm, "\(name): the score sings, rc \(r.rc), \(dur(r)) (spoken \(dur(spoken)))")
    }
    // 4. the text around a score is spoken exactly as it would be alone
    let before = renderSing("<speak>Here we go:</speak>", sam, nil)
    let mid = renderSing("<speak>Here we go: \(song)</speak>", sam, on)
    check(mid.pcm.count > before.pcm.count && Array(mid.pcm.prefix(before.pcm.count)) == before.pcm,
          "the words before a score are spoken exactly as without it")
    // 4b. tempo memory across requests (VoiceOver sends each line separately): line 2 has no tempo of its own
    ClassicEngine.forgetScoreTempo()
    let l2 = "<speak>la C4 1 la D4 1 la E4 1 la F4 1</speak>"
    let at60 = renderSing("<speak>tempo 60 la C4 1 la D4 1 la E4 1 la F4 1</speak>", sam, on)
    let line1 = renderSing("<speak>tempo 60 la G4 1 la A4 1 la B4 1</speak>", sam, on)
    let line2 = renderSing(l2, sam, on)
    ClassicEngine.forgetScoreTempo()
    let plain100 = renderSing(l2, sam, on)
    _ = renderSing("<speak>tempo 60 la G4 1 la A4 1 la B4 1</speak>", sam, on)
    let saved = ClassicEngine.scoreTempoMemorySeconds
    ClassicEngine.scoreTempoMemorySeconds = -1          // as if 60 s without a score had passed
    let expired = renderSing(l2, sam, on)
    ClassicEngine.scoreTempoMemorySeconds = saved
    ClassicEngine.forgetScoreTempo()
    check(!line1.pcm.isEmpty && line2.pcm == at60.pcm && line2.pcm != plain100.pcm && expired.pcm == plain100.pcm,
          "tempo remembered for the next line (\(dur(line2)) = \(dur(at60)) at tempo 60), default 100 after the expiry (\(dur(expired)))")
    let own = renderSing("<speak>tempo 200 la C4 1 la D4 1 la E4 1 la F4 1</speak>", sam, on)
    let own2 = renderSing("<speak>tempo 200 la C4 1 la D4 1 la E4 1 la F4 1</speak>", sam, on)
    check(own.pcm == own2.pcm && own.pcm.count < at60.pcm.count, "a score's own tempo overrides the remembered one")
    ClassicEngine.forgetScoreTempo()
    // 5. Preview: a pasted whole score sings with the switch off; with it on, a score inside text sings
    ClassicEngine.shared.releaseIdle()
    let pv = ClassicEngine.shared.synthesize(text: "tempo 120\ntwin-kle C4 1 C4 1\nstar G4 2", voice: sam, sapiRate: 0, singing: off) ?? []
    ClassicEngine.shared.releaseIdle()
    let ps = ClassicEngine.shared.synthesize(text: "twinkle twinkle star", voice: sam, sapiRate: 0, singing: off) ?? []
    check(!pv.isEmpty && pv != ps, "Preview: a pasted score sings")
    ClassicEngine.shared.releaseIdle()
    let pt = ClassicEngine.shared.synthesize(text: "Hello \(song) goodbye", voice: sam, sapiRate: 0, singing: on) ?? []
    ClassicEngine.shared.releaseIdle()
    let pp = ClassicEngine.shared.synthesize(text: "Hello \(song) goodbye", voice: sam, sapiRate: 0, singing: off) ?? []
    check(!pt.isEmpty && pt != pp, "Preview: a score inside text sings with the switch on")
}

// MARK: - capture (the switch uses this Mac user's real group container; the recording goes to a temp dir)

func runCapture() {
    guard SSMLCapture.container != nil else { check(false, "capture: no group container"); return }
    let dir = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("ssml-capture-test-\(getpid())")
    SSMLCapture.recordDirectory = dir
    defer { try? FileManager.default.removeItem(at: dir) }
    SSMLCapture.setEnabled(false)
    func recording() -> String { (SSMLCapture.logURL.flatMap { try? String(contentsOf: $0, encoding: .utf8) }) ?? "" }
    let vo = "<speak><prosody rate=\"130.0%\"><s><lang xml:lang=\"en-US\">Private label</lang></s><s>Button</s></prosody></speak>"
    SSMLCapture.record(ssml: vo, voice: "sam", segments: SSML.segments(vo), speed: 1)
    SSMLCapture.waitForWrites()
    check(recording().isEmpty, "nothing recorded while off")
    let status = (SSMLCapture.statusURL.flatMap { try? String(contentsOf: $0, encoding: .utf8) }) ?? ""
    check(status.contains("-> off"), "status file written once per process: \(status.trimmingCharacters(in: .newlines))")
    SSMLCapture.setEnabled(true, for: 60)
    check(SSMLCapture.isEnabled, "switched on")
    SSMLCapture.record(ssml: vo, voice: "sam", segments: SSML.segments(vo), speed: 1.5)
    SSMLCapture.recordCancel()
    SSMLCapture.record(ssml: "<speak>5</speak>", voice: "sapi4_sam", segments: SSML.segments("<speak>5</speak>"), speed: 1)
    SSMLCapture.waitForWrites()
    let text = recording()
    print(text.split(separator: "\n").map { "     | " + $0 }.joined(separator: "\n"))
    check(text.components(separatedBy: "\nreq ").count == 3 && text.contains("\ncancel "), "two requests and a cancel recorded")
    check(!text.contains("Private") && !text.contains("label") && !text.contains("Button"), "no words in the recording")
    check(text.contains("[13|s") && text.contains("segs=2"), "the recording shows the cut and the pause")
    SSMLCapture.setEnabled(true, for: 60)     // a new switch-on starts a fresh recording
    SSMLCapture.record(ssml: "<speak>5</speak>", voice: "sam", segments: SSML.segments("<speak>5</speak>"), speed: 1)
    SSMLCapture.waitForWrites()
    check(recording().components(separatedBy: "req ").count == 2, "switching on again starts a fresh recording")
    SSMLCapture.setEnabled(false)
    SSMLCapture.record(ssml: vo, voice: "sam", segments: SSML.segments(vo), speed: 1)
    SSMLCapture.waitForWrites()
    check(!SSMLCapture.isEnabled && recording().components(separatedBy: "req ").count == 2, "switched off: no more lines")
}

// MARK: - neural voices: the old NeuralVoice app's pauses vs this app's

/// Silent runs (ms) between the first and last sound, >= 40 ms: the rule of longestGapMs / tools/pause_gaps.py.
func gapList(_ s: [Int16]) -> [Int] {
    let fl = 110
    var db: [Double] = []
    var i = 0
    while i + fl <= s.count {
        var mean = 0.0
        for k in i..<(i + fl) { mean += Double(s[k]) }
        mean /= Double(fl)
        var e = 0.0
        for k in i..<(i + fl) { let d = Double(s[k]) - mean; e += d * d }
        db.append(20 * log10(sqrt(e / Double(fl)) + 1e-9))
        i += fl
    }
    guard let peak = db.max() else { return [] }
    let thr = max(peak - 40, 20 * log10(250.0))
    let loud = db.map { $0 > thr }
    guard let first = loud.firstIndex(of: true), let last = loud.lastIndex(of: true) else { return [] }
    var out: [Int] = [], run = 0
    for k in first...last {
        if loud[k] { if run * 5 >= 40 { out.append(run * 5) }; run = 0 } else { run += 1 }
    }
    return out
}

/// The old NeuralVoice app's request path (NeuralVoiceProvider.swift, ~/code/NeuralVoice, as of 2026-07-19): every tag
/// replaced by a space, five entities decoded, all whitespace (line breaks too) collapsed to one space, then ONE engine
/// call for the whole request, untrimmed, at RateAdjustment 100 when VoiceOver sends no rate (Sonia and Ryan's own is 95).
func oldNeuralPlainText(_ ssml: String) -> String {
    var s = ssml.replacingOccurrences(of: "<[^>]+>", with: " ", options: .regularExpression)
    let ents = ["&amp;": "&", "&lt;": "<", "&gt;": ">", "&quot;": "\"", "&apos;": "'", "&#39;": "'"]
    for (k, v) in ents { s = s.replacingOccurrences(of: k, with: v) }
    s = s.replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
    return s.trimmingCharacters(in: .whitespacesAndNewlines)
}

let neuralFixtures: [Fixture] = [
    Fixture(name: "commas", ssml: "<speak>First, second, and third, then we stop.</speak>"),
    Fixture(name: "periods", ssml: "<speak>This is one. This is two. This is three.</speak>"),
    Fixture(name: "questions", ssml: "<speak>Is it ready? Yes. Are you sure? Quite sure.</speak>"),
    Fixture(name: "list", ssml: "<speak>Shopping list: milk, eggs, bread, butter, and coffee.</speak>"),
    Fixture(name: "linebreak", ssml: "<speak>Meeting moved to Tuesday\nBring the slides</speak>"),
    Fixture(name: "listlines", ssml: "<speak>Milk\nEggs\nBread</speak>"),
    Fixture(name: "break500", ssml: #"<speak>Wait<break time="500ms"/>now go</speak>"#),
    Fixture(name: "vo_button", ssml: #"<speak><prosody rate="100.0%"><s><lang xml:lang="en-US">Speech</lang></s><s><lang xml:lang="en-US">Button</lang></s></prosody></speak>"#),
    Fixture(name: "vo_battery", ssml: "<speak><s>65% battery power</s><s>Charging</s></speak>"),
    Fixture(name: "vo_heading", ssml: "<speak><s>Classic Voices</s><s>Heading</s></speak>"),
]

func runNeural(_ outDir: String) {
    try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
    guard !NEURAL_VOICES.isEmpty else { check(false, "no neural voices on this Mac"); return }
    print("\nvoice   fixture        before: gaps ms (dur)                     after: gaps ms (dur)")
    for v in NEURAL_VOICES {
        let oldRate = v.language == "en-GB" ? 10 * log(100.0 / 95.0) / log(3.0) : 0   // RateAdjustment 100
        for f in neuralFixtures {
            var before: [Int16] = []
            let rcb = ClassicEngine.shared.speak(text: oldNeuralPlainText(f.ssml), voice: v, sapiRate: oldRate, semitones: 0,
                                                 trimSilence: false) { before.append(contentsOf: $0); return true }
            var after: [Int16] = []
            // the extension's path: SSMLCapture-free, as ClassicVoicesProvider does it for a neural voice
            let rca = ClassicEngine.shared.speak(segments: SSML.segments(f.ssml, lineBreaks: true), voice: v, sapiRate: 0,
                                                 semitones: 0, onPCM: { after.append(contentsOf: $0); return true },
                                                 silence: { ms in after.append(contentsOf: repeatElement(0, count: ms * 22050 / 1000)); return true })
            writeWav(before, "\(outDir)/\(v.neuralVoice)_\(f.name)_before.wav")
            writeWav(after, "\(outDir)/\(v.neuralVoice)_\(f.name)_after.wav")
            let gb = gapList(before), ga = gapList(after)
            print(String(format: "%-7@ %-13@  %-36@ %@", v.neuralVoice, f.name,
                         "\(gb) (\(before.count * 1000 / 22050))" as NSString, "\(ga) (\(after.count * 1000 / 22050))" as NSString))
            check(rcb == 0 && rca == 0 && !after.isEmpty, "\(v.neuralVoice) \(f.name): rendered")
            let bmax = gb.max() ?? 0, amax = ga.max() ?? 0
            switch f.name {
            case "linebreak", "vo_button", "vo_battery", "vo_heading":
                check(amax >= SSML.phraseGapMs && amax > bmax + 60, "\(v.neuralVoice) \(f.name): boundary pause \(bmax) -> \(amax) ms")
            case "listlines":
                check(ga.filter { $0 >= SSML.phraseGapMs }.count >= 2 && ga.filter { $0 >= SSML.phraseGapMs }.count > gb.filter { $0 >= SSML.phraseGapMs }.count,
                      "\(v.neuralVoice) \(f.name): a pause after each line \(gb) -> \(ga)")
            case "break500":
                check(amax >= 500 && bmax < 200, "\(v.neuralVoice) \(f.name): <break time=500ms> \(bmax) -> \(amax) ms")
            default:   // punctuation: the engine's own pauses, before and after
                check(ga.count >= 2 && amax >= 100, "\(v.neuralVoice) \(f.name): the engine's own pauses at punctuation \(ga)")
            }
        }
    }
}

// MARK: - main

let args = CommandLine.arguments
if args.count >= 2 && args[1] == "parse" {
    runParse()
} else if args.count >= 2 && args[1] == "capture" {
    runCapture()
} else if args.count >= 3 && args[1] == "render" {
    runRender(args[2])
} else if args.count >= 3 && args[1] == "singscores" {
    runSingScores(args[2])
} else if args.count >= 3 && args[1] == "neural" {
    runNeural(args[2])
} else {
    print("usage: ssml_test parse | render OUTDIR")
    exit(2)
}
print(failures == 0 ? "all passed" : "\(failures) FAILED")
exit(failures == 0 ? 0 : 1)
