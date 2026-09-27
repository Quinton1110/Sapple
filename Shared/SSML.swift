//
//  SSML.swift
//  What the extension takes from the SSML that VoiceOver (and other speech clients) send: the text,
//  cut into segments at every pause the markup asks for, plus the prosody rate and pitch.
//
//  Pauses (Quinton, 2026-09-21: "Speech, button" was read as "Speech button", and "65% battery power,
//  charging" ran together):
//   - VoiceOver sends the parts of an element - label, trait, value - as ADJACENT <s> SENTENCES with
//     nothing between them: <s>Speech</s><s>Button</s> (seen in a real iOS 27 VoiceOver capture by the
//     RHVoice iOS port; our own capture tool is SSMLCapture). Until 2026-09-22 every tag became a space,
//     so the engines got "Speech Button" as one phrase with no pause. Now an <s> or <p> boundary ends a
//     segment, and the gap is real silence.
//   - <break time="..."/> / strength="...": explicit pauses, as before.
//   - Commas and full stops inside a text are left to the engine: all three engines pause there on
//     their own (measured 155-310 ms for a comma at normal speed).
//  Each segment is spoken by its own engine call, so it ends with the engine's own phrase-final
//  lengthening and fall; the engine's padding is trimmed (cv_bridge.c) and the pause is the silence
//  computed here. A boundary before the first or after the last text adds nothing, and neighbouring
//  segments that are only digits stay one engine call, exactly as before.
//

import Foundation

enum SSML {
    /// Where a segment ends. Element boundaries: VoiceOver's label | trait | value separator.
    enum Boundary: Int, Comparable {
        case none = 0, sentence, paragraph
        static func < (a: Boundary, b: Boundary) -> Bool { a.rawValue < b.rawValue }
    }

    /// Everything that separates a segment from the next one.
    struct Pause: Equatable {
        /// <break time="..."/>: exact milliseconds (each break capped at 2 s), never scaled with the rate.
        var timeMs = 0
        /// <break strength="..."/> or a bare <break/>: milliseconds at natural speed, scaled with the rate.
        var strengthMs = 0
        /// any <break> at all - then the breaks decide and an element boundary adds nothing
        var hasBreak = false
        /// <s> / <p> element boundary
        var boundary: Boundary = .none

        var isEmpty: Bool { !hasBreak && boundary == .none }
    }

    struct Segment: Equatable {
        let text: String
        /// the pause after this segment
        var pause: Pause
        /// This segment's pitch shift in semitones, from the <prosody pitch> it sits inside (nested
        /// prosody combined, 0 = unchanged).
        var semitones: Double

        init(text: String, pause: Pause = Pause(), semitones: Double = 0) {
            self.text = text
            self.pause = pause
            self.semitones = semitones
        }

        /// Milliseconds of silence after this segment when the voice speaks at `speed` x its natural rate.
        func silenceMs(speed: Double) -> Int { SSML.silenceMs(pause, after: text, speed: speed) }
    }

    // MARK: - Segments

    /// Splits at <break> tags and at <s> / <p> element boundaries. Text is cleaned exactly as before
    /// (tags become spaces, entities decoded, whitespace collapsed), so SSML without <s>/<p> gives the
    /// same text as it always did.
    ///
    /// ⛔ PITCH IS SCOPED, NOT PER REQUEST (2026-09-24). With "Use Pitch Changes" on, VoiceOver lowers
    /// only a container's name: <s><prosody pitch="-15%">Message header</prosody></s><s>...content...</s>,
    /// "Dock" in "Dock, Messages", "iMessage" in "iMessage, text field". The old code took the FIRST
    /// pitch in the request for the whole request: SAPI 4 Sam (no outer pitch) read the whole line low,
    /// TruVoice (outer pitch="+0.0%" first) ignored it. Captured on the phone 2026-09-24 00:03 / 00:07.
    /// Each segment now carries the pitch of the <prosody> it sits in, and a pitch change mid-sentence
    /// starts a new segment with no pause.
    ///
    /// `lineBreaks` (the neural voices, 2026-09-27): a line break in the text is a sentence boundary too, so each line is
    /// its own engine call with the boundary pause after it (the old NeuralVoice app collapsed line breaks into spaces and
    /// ran the lines together). Off for every other engine: their text is split exactly as before.
    static func segments(_ ssml: String, lineBreaks: Bool = false) -> [Segment] {
        let s = spellOutSayAs(ssml)
        var out: [Segment] = []
        var raw = ""               // this segment's text, tags already turned into spaces, entities not yet decoded
        var hasText = false        // raw holds something other than whitespace
        var pending = Pause()      // pause signals seen since the last text
        var pitchStack: [Double] = [0]   // semitones in force, one entry per open <prosody>
        var segPitch = 0.0         // the pitch of the segment being collected (set at its first text)

        func close() {
            out.append(Segment(text: clean(raw), pause: pending, semitones: segPitch))
            raw = ""
            hasText = false
            pending = Pause()
        }

        var i = s.startIndex
        while i < s.endIndex {
            let c = s[i]
            if c == "<", let end = tagEnd(s, from: i) {
                let tag = parseTag(s[s.index(after: i)..<end])
                switch tag.name {
                case "break" where !tag.isEnd:
                    pending.hasBreak = true
                    if let ms = breakTimeMs(tag.attrs) { pending.timeMs += ms }
                    else { pending.strengthMs += breakStrengthMs(tag.attrs) }
                case "s", "p":
                    pending.boundary = max(pending.boundary, tag.name == "p" ? .paragraph : .sentence)
                    raw.append(" ")
                case "prosody":
                    if tag.isEnd {
                        if pitchStack.count > 1 { pitchStack.removeLast() }
                    } else {
                        let parent = pitchStack.last ?? 0
                        var p = parent
                        if let v = attribute("pitch", in: String(tag.attrs)), let (st, relative) = pitchValue(v) {
                            p = min(12, max(-12, relative ? parent + st : st))
                        }
                        // a self-closing <prosody .../> scopes nothing
                        if !tag.attrs.trimmingCharacters(in: .whitespaces).hasSuffix("/") { pitchStack.append(p) }
                    }
                    raw.append(" ")
                default:
                    raw.append(" ")
                }
                i = s.index(after: end)
                continue
            }
            if lineBreaks && c.isNewline {
                pending.boundary = max(pending.boundary, .sentence)
                raw.append(" ")
                i = s.index(after: i)
                continue
            }
            if !c.isWhitespace {
                let pitch = pitchStack.last ?? 0
                if pending.hasBreak {
                    close()                        // (text-less when the breaks came first: leading silence)
                } else if pending.boundary != .none {
                    if hasText { close() } else { pending = Pause() }   // no pause before the first text
                } else if hasText && pitch != segPitch {
                    close()                        // pitch changes mid-sentence: new segment, no pause
                }
                if !hasText { segPitch = pitch }
                hasText = true
            }
            raw.append(c)
            i = s.index(after: i)
        }
        pending.boundary = .none                   // no pause after the last text either
        if hasText || pending.hasBreak { close() }
        return mergingDigitRuns(out)
    }

    /// Neighbouring digit-only segments split by an element boundary stay one engine call (digits
    /// typed or read one at a time must stay as snappy as before).
    private static func mergingDigitRuns(_ segs: [Segment]) -> [Segment] {
        var out: [Segment] = []
        for seg in segs {
            if let last = out.last, !last.pause.hasBreak, last.pause.boundary != .none,
               last.semitones == seg.semitones, isDigitsOnly(last.text), isDigitsOnly(seg.text) {
                out[out.count - 1] = Segment(text: last.text + " " + seg.text, pause: seg.pause,
                                             semitones: seg.semitones)
            } else {
                out.append(seg)
            }
        }
        return out
    }

    static func isDigitsOnly(_ s: String) -> Bool {
        !s.isEmpty && s.allSatisfy { $0.isASCII && ($0.isNumber || $0 == " ") }
    }

    // MARK: - Pause lengths

    /// Silence at an element boundary at natural speed. Calibrated 2026-09-22 against Apple's voices
    /// rendering VoiceOver's <s>label</s><s>trait</s> shape (AVSpeechSynthesizer on the Mac, gap = the
    /// longest silence between the words): Samantha 240-275 ms ("Speech | button" 260, "Classic Voices |
    /// Heading" 275, "65% battery power | Charging" 240; 175 at 1.5x, 140 at 2x, 95 at 3x), Alex 75-90 ms
    /// plus a strongly lengthened last syllable. Our measured gaps come out 40-140 ms longer than this
    /// number (the trimmed segment keeps a 30 ms faded tail, stop consonants start with a quiet closure,
    /// echo voices ring): with 150, about 190-290 ms at natural speed - Samantha's range.
    static let phraseGapMs = 150
    /// ...after a segment that ends a sentence (. ! ?): Apple's gap there is 230 (Samantha) to 585 ms
    /// (Alex); the engines' own full-stop pause is 335-500 ms.
    static let sentenceGapMs = 380
    static let paragraphGapMs = 450
    /// Shortest boundary pause at the fastest rates.
    static let minPhraseGapMs = 50
    static let minSentenceGapMs = 90

    static func silenceMs(_ p: Pause, after text: String, speed: Double) -> Int {
        if p.hasBreak { return p.timeMs + scaled(p.strengthMs, speed: speed) }
        switch p.boundary {
        case .none:
            return 0
        case .sentence:
            return endsSentence(text) ? max(minSentenceGapMs, scaled(sentenceGapMs, speed: speed))
                                      : max(minPhraseGapMs, scaled(phraseGapMs, speed: speed))
        case .paragraph:
            return max(minSentenceGapMs, scaled(paragraphGapMs, speed: speed))
        }
    }

    /// A pause at natural speed, at `speed` x natural. Follows Samantha's boundary pause: it shrinks a
    /// little slower than the speech itself when faster (x speed^-0.9), and grows much slower than the
    /// speech when slower (x speed^-0.4). Exact at speed 1.
    static func scaled(_ ms: Int, speed: Double) -> Int {
        guard ms > 0 else { return 0 }
        let s = speed.isFinite && speed > 0 ? speed : 1
        if s == 1 { return ms }
        return Int((Double(ms) * pow(s, s > 1 ? -0.9 : -0.4)).rounded())
    }

    /// Ends with . ! ? or an ellipsis, allowing closing quotes and brackets after it.
    static func endsSentence(_ text: String) -> Bool {
        let closers: Set<Character> = ["\"", "'", ")", "]", "}", "\u{201D}", "\u{2019}", "\u{00BB}"]
        guard let last = text.reversed().first(where: { !$0.isWhitespace && !closers.contains($0) }) else { return false }
        return ".!?\u{2026}".contains(last)
    }

    // MARK: - Text

    /// Strip tags, decode entities, collapse whitespace.
    static func plainText(_ ssml: String) -> String {
        clean(ssml.replacingOccurrences(of: "<[^>]*>", with: " ", options: .regularExpression))
    }

    /// Decode entities, collapse whitespace, trim (text whose tags are already spaces).
    static func clean(_ text: String) -> String {
        decodeEntities(text).replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// <say-as interpret-as="characters">abc</say-as> -> "a, b, c": the engine then names each letter
    /// ("a" run into "b" would be read as the article).
    static func spellOutSayAs(_ ssml: String) -> String {
        let pattern = #"<say-as\b[^>]*interpret-as\s*=\s*["'](?:characters|spell-out|letters)["'][^>]*>(.*?)</say-as>"#
        guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive, .dotMatchesLineSeparators])
        else { return ssml }
        let ns = ssml as NSString
        var result = ""
        var last = 0
        for m in re.matches(in: ssml, range: NSRange(location: 0, length: ns.length)) {
            result += ns.substring(with: NSRange(location: last, length: m.range.location - last))
            let inner = decodeEntities(ns.substring(with: m.range(at: 1))
                .replacingOccurrences(of: "<[^>]*>", with: "", options: .regularExpression))
            let chars = inner.filter { !$0.isWhitespace }.map { String($0) }
            result += " " + escape(chars.joined(separator: ", ")) + " "
            last = m.range.location + m.range.length
        }
        result += ns.substring(from: last)
        return result
    }

    private static func escape(_ s: String) -> String {
        s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
            .replacingOccurrences(of: ">", with: "&gt;")
    }

    static func decodeEntities(_ input: String) -> String {
        guard input.contains("&") else { return input }
        var s = input
        let named = ["&quot;": "\"", "&apos;": "'", "&lt;": "<", "&gt;": ">", "&nbsp;": " "]
        for (k, v) in named { s = s.replacingOccurrences(of: k, with: v) }
        if let re = try? NSRegularExpression(pattern: "&#(x?)([0-9A-Fa-f]{1,6});") {
            let ns = s as NSString
            var out = ""
            var last = 0
            for m in re.matches(in: s, range: NSRange(location: 0, length: ns.length)) {
                out += ns.substring(with: NSRange(location: last, length: m.range.location - last))
                let hex = ns.substring(with: m.range(at: 1)) != ""
                let digits = ns.substring(with: m.range(at: 2))
                if let v = UInt32(digits, radix: hex ? 16 : 10), let u = Unicode.Scalar(v) {
                    out.unicodeScalars.append(u)
                }
                last = m.range.location + m.range.length
            }
            out += ns.substring(from: last)
            s = out
        }
        return s.replacingOccurrences(of: "&amp;", with: "&")   // last, so "&amp;lt;" stays "&lt;"
    }

    // MARK: - Tags

    /// The '>' that ends the tag starting at `start` (a '<'): the end of "-->" for a comment, else the
    /// next '>' (the same rule as the old "<[^>]*>" strip). nil = no tag here, the '<' is text.
    static func tagEnd(_ s: String, from start: String.Index) -> String.Index? {
        if s[start...].hasPrefix("<!--") {
            return s[start...].range(of: "-->").map { s.index(before: $0.upperBound) }
        }
        return s[s.index(after: start)...].firstIndex(of: ">")
    }

    struct Tag {
        let name: String        // lowercased local name ("" for comments, <?...?>, <!...>)
        let isEnd: Bool
        let attrs: Substring
    }

    /// `body` is what is between '<' and '>'.
    static func parseTag(_ body: Substring) -> Tag {
        var b = body.drop(while: { $0.isWhitespace })
        if b.first == "!" || b.first == "?" { return Tag(name: "", isEnd: false, attrs: b) }
        let isEnd = b.first == "/"
        if isEnd { b = b.dropFirst() }
        let nameEnd = b.firstIndex(where: { $0.isWhitespace || $0 == "/" }) ?? b.endIndex
        var name = b[..<nameEnd].lowercased()
        if let colon = name.lastIndex(of: ":") { name = String(name[name.index(after: colon)...]) }
        return Tag(name: name, isEnd: isEnd, attrs: b[nameEnd...])
    }

    /// <break time="300ms"/>, time="1.5s", strength="medium" ... -> milliseconds, capped at 2 s.
    static func breakMs(_ attrs: String) -> Int {
        breakTimeMs(Substring(attrs)) ?? breakStrengthMs(Substring(attrs))
    }

    /// The time attribute in milliseconds (capped at 2 s); nil when absent or unreadable.
    static func breakTimeMs(_ attrs: Substring) -> Int? {
        func cap(_ ms: Double) -> Int { max(0, min(Int(ms.rounded()), 2000)) }
        guard let t = attribute("time", in: String(attrs))?.lowercased().trimmingCharacters(in: .whitespaces)
        else { return nil }
        if t.hasSuffix("ms"), let n = Double(t.dropLast(2)) { return cap(n) }
        if t.hasSuffix("s"), let n = Double(t.dropLast(1)) { return cap(n * 1000) }
        if let n = Double(t) { return cap(n) }
        return nil
    }

    /// The strength attribute (or a bare <break/>) in milliseconds at natural speed.
    static func breakStrengthMs(_ attrs: Substring) -> Int {
        switch attribute("strength", in: String(attrs))?.lowercased() {
        case "none": return 0
        case "x-weak": return 100
        case "weak": return 200
        case "medium": return 350
        case "strong": return 500
        case "x-strong": return 800
        default: return 300
        }
    }

    static func attribute(_ name: String, in attrs: String) -> String? {
        firstMatch(in: attrs, pattern: "\\b\(name)\\s*=\\s*[\"']([^\"']*)[\"']")
    }

    // MARK: - Prosody

    /// VoiceOver sends rate as a float percentage, e.g. rate="130.0%". nil when absent.
    static func ratePercent(_ ssml: String) -> Double? {
        guard let r = firstMatch(in: ssml, pattern: "rate\\s*=\\s*[\"']([0-9]+(?:\\.[0-9]+)?)%") else { return nil }
        return Double(r)
    }

    /// Pitch as a shift in semitones (0 = unchanged). Accepts "120%" (of normal), "+20%" / "-10%"
    /// (relative), "+2st", and the SSML names x-low ... x-high.
    /// Only the FIRST pitch in the request: kept for callers without segments. Segments carry their own
    /// scoped pitch (see segments(_:)).
    static func pitchSemitones(_ ssml: String) -> Double {
        guard let raw = firstMatch(in: ssml, pattern: "pitch\\s*=\\s*[\"']([^\"']*)[\"']"),
              let (st, _) = pitchValue(raw) else { return 0 }
        return st
    }

    /// One pitch attribute value as semitones, and whether it is relative to the enclosing pitch
    /// ("+20%", "-10%", "+2st") rather than absolute ("120%", "x-low" ... "x-high", "medium").
    /// nil when empty or unreadable.
    static func pitchValue(_ value: String) -> (Double, Bool)? {
        let raw = value.trimmingCharacters(in: .whitespaces).lowercased()
        guard !raw.isEmpty else { return nil }
        func st(_ mult: Double) -> Double {
            let m = min(2.0, max(0.5, mult))
            return 12.0 * log2(m)
        }
        switch raw {
        case "x-low": return (st(0.7), false)
        case "low": return (st(0.85), false)
        case "medium", "default": return (0, false)
        case "high": return (st(1.15), false)
        case "x-high": return (st(1.3), false)
        default: break
        }
        let signed = raw.hasPrefix("+") || raw.hasPrefix("-")
        // semitones are always a change from the enclosing pitch
        if raw.hasSuffix("st"), let n = Double(raw.dropLast(2)) { return (min(12, max(-12, n)), true) }
        if raw.hasSuffix("%"), let n = Double(raw.dropLast()) {
            if signed { return (st(1.0 + n / 100.0), true) }
            return (n <= 0 ? 0 : st(n / 100.0), false)
        }
        return nil
    }

    static func firstMatch(in text: String, pattern: String) -> String? {
        guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else { return nil }
        let range = NSRange(text.startIndex..., in: text)
        guard let m = re.firstMatch(in: text, range: range), m.numberOfRanges >= 2,
              let r = Range(m.range(at: 1), in: text) else { return nil }
        return String(text[r])
    }
}
