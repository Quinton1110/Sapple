//
//  SSMLCapture.swift
//  Diagnostics for the pause fix: what SHAPE of SSML does VoiceOver send? OFF unless switched on in the
//  app (Diagnostics, "Record pause markup"), and then only for 15 minutes.
//
//  While on, the extension appends one line per speech request to ssml-capture.txt in ITS OWN
//  Documents folder: the time, the gap since the previous request, the voice, how SSML.swift cut it
//  up, and the markup with every text replaced by its length and a class string - letters -> a,
//  digits -> 9, whitespace -> _, punctuation kept - so "65% battery power, charging" is recorded as
//  «27:9%_a_a,_a». Tag names and the attributes that shape speech (rate, pitch, volume, break time /
//  strength, xml:lang, interpret-as ...) are kept; any other attribute value is replaced by its
//  length. NEVER the words. At most 256 KB; switching on again starts a fresh recording. Cancels are
//  logged too (so separate requests for label and trait would show).
//
//  Why the extension's own folder: its sandbox lets it READ the app group (the switch lives there,
//  in the group's preferences and in Library/ssml-capture-until) but, measured on the phone
//  2026-09-22, not write to it - neither a file (container root or Library/) nor the group's
//  preferences received anything. Speech extensions are walled in (no network either). So the app
//  cannot read the recording; the Mac collects it:
//    xcrun devicectl device copy from --device <id> --domain-type appDataContainer
//      --domain-identifier com.quinton.classicvoices.Extension --source Documents/ssml-capture.txt --destination x.txt
//  Documents/ssml-capture-status.txt (rewritten at each extension process start, no speech content)
//  says whether the extension could see the switch.
//

import Foundation

enum SSMLCapture {
    static let groupID = "group.com.quinton.classicvoices"
    static let window: TimeInterval = 15 * 60
    static let maxBytes = 256 * 1024

    static var container: URL? { FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID) }
    static var flagURL: URL? { container?.appendingPathComponent("Library/ssml-capture-until") }
    /// Where this process records (the extension's own Documents). Tests point it elsewhere.
    static var recordDirectory: URL? = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
    static var logURL: URL? { recordDirectory?.appendingPathComponent("ssml-capture.txt") }
    static var statusURL: URL? { recordDirectory?.appendingPathComponent("ssml-capture-status.txt") }
    private static let defaults = UserDefaults(suiteName: groupID)
    private static let untilKey = "ssmlCaptureUntil"

    // MARK: - The switch (app writes, extension reads)

    /// When recording stops, or nil when it is off (the preferences or the file may carry the switch).
    static var enabledUntil: Date? {
        let t = untilStamp
        return t > 0 && Date(timeIntervalSince1970: t) > Date() ? Date(timeIntervalSince1970: t) : nil
    }

    private static var untilStamp: Double {
        var t = defaults?.double(forKey: untilKey) ?? 0
        if t == 0, let url = flagURL, let s = try? String(contentsOf: url, encoding: .utf8) {
            t = Double(s.trimmingCharacters(in: .whitespacesAndNewlines)) ?? 0
        }
        return t
    }

    static var isEnabled: Bool { enabledUntil != nil }

    static func setEnabled(_ on: Bool, for seconds: TimeInterval = window) {
        if on {
            let until = Date().addingTimeInterval(seconds).timeIntervalSince1970
            defaults?.set(until, forKey: untilKey)
            if let url = flagURL {
                try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? String(format: "%.3f", until).write(to: url, atomically: true, encoding: .utf8)
            }
        } else {
            defaults?.removeObject(forKey: untilKey)
            if let url = flagURL { try? FileManager.default.removeItem(at: url) }
        }
    }

    // MARK: - Recording (extension)

    private static let queue = DispatchQueue(label: "com.quinton.classicvoices.ssmlcapture")
    private static var lastEvent: Date?
    private static var session: Double = 0        // the switch's "until" stamp this recording belongs to
    private static var statusWritten = false

    /// One line for a speech request, when recording is on. Cheap when off: a cached preference read,
    /// and (only while that is empty) one file-exists check.
    static func record(ssml: String, voice: String, segments: [SSML.Segment], speed: Double) {
        let until = switchStamp()
        let now = Date()
        guard until > 0, Date(timeIntervalSince1970: until) > now else {
            queue.async { writeStatusOnce(until: until) }
            return
        }
        queue.async {
            writeStatusOnce(until: until)
            let dt = lastEvent.map { String(format: "+%.0fms", now.timeIntervalSince($0) * 1000) } ?? "-"
            lastEvent = now
            let cuts = segments.map { seg -> String in
                let p = seg.pause
                var kind = p.hasBreak ? "break" : ["", "s", "p"][p.boundary.rawValue]
                if p.hasBreak && p.boundary != .none { kind += "+" + ["", "s", "p"][p.boundary.rawValue] }
                let ms = seg.silenceMs(speed: speed)
                return "\(seg.text.count)" + (kind.isEmpty ? "" : "|\(kind)\(ms)")
            }.joined(separator: " ")
            append(String(format: "req %@ dt=%@ voice=%@ speed=%.2f segs=%d [%@] ssml=%@\n",
                          stamp(now), dt, voice, speed, segments.count, cuts, skeleton(ssml)), session: until)
        }
    }

    static func recordCancel() {
        let until = switchStamp()
        let now = Date()
        guard until > 0, Date(timeIntervalSince1970: until) > now else { return }
        queue.async {
            let dt = lastEvent.map { String(format: "+%.0fms", now.timeIntervalSince($0) * 1000) } ?? "-"
            lastEvent = now
            append("cancel \(stamp(now)) dt=\(dt)\n", session: until)
        }
    }

    /// Waits until every queued line is written (tests).
    static func waitForWrites() { queue.sync {} }

    /// The switch's stamp, or 0: the preferences first (cached, cheap), the file only if it exists.
    private static func switchStamp() -> Double {
        if let t = defaults?.double(forKey: untilKey), t > 0 { return t }
        guard let flag = flagURL, FileManager.default.fileExists(atPath: flag.path) else { return 0 }
        return untilStamp
    }

    /// Once per process: can this process see the switch at all? (No speech content.)
    private static func writeStatusOnce(until: Double) {
        guard !statusWritten, let url = statusURL else { return }
        statusWritten = true
        let prefs = defaults?.double(forKey: untilKey) ?? 0
        let file: String
        if let f = flagURL {
            file = FileManager.default.fileExists(atPath: f.path)
                ? ((try? String(contentsOf: f, encoding: .utf8)).map { "readable \($0)" } ?? "exists, unreadable") : "absent"
        } else {
            file = "no group container"
        }
        let line = String(format: "process start %@: switch prefs=%.3f file=%@ -> %@\n", stamp(Date()), prefs, file,
                          until > 0 && Date(timeIntervalSince1970: until) > Date() ? "recording" : "off")
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? line.write(to: url, atomically: true, encoding: .utf8)
    }

    /// Appends to the recording (capped at maxBytes); a new switch-on starts a fresh file.
    private static func append(_ line: String, session until: Double) {
        guard let url = logURL, let data = line.data(using: .utf8) else { return }
        let fm = FileManager.default
        if until != session || !fm.fileExists(atPath: url.path) {
            session = until
            let head = "recording \(stamp(Date())) until \(stamp(Date(timeIntervalSince1970: until))) (structure only)\n"
            try? fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? (head + line).data(using: .utf8)?.write(to: url, options: .atomic)
            return
        }
        guard let h = try? FileHandle(forWritingTo: url) else { return }
        defer { try? h.close() }
        guard let size = try? h.seekToEnd(), size + UInt64(data.count) <= UInt64(maxBytes) else { return }
        try? h.write(contentsOf: data)
    }

    private static func stamp(_ d: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HH:mm:ss.SSS"
        return f.string(from: d)
    }

    // MARK: - Structure only

    /// Attributes whose values shape speech and never carry the spoken words.
    private static let keptAttributes: Set<String> = [
        "rate", "pitch", "volume", "range", "contour", "duration", "time", "strength", "xml:lang", "lang",
        "interpret-as", "format", "detail", "version", "xmlns", "gender", "age", "variant", "level",
    ]

    /// The SSML with every text node replaced by «length:class» and unknown attribute values by «length».
    static func skeleton(_ ssml: String) -> String {
        var out = ""
        var text = ""
        func flushText() {
            guard !text.isEmpty else { return }
            let decoded = SSML.decodeEntities(text)
            out += "«\(decoded.count):\(textClass(decoded))»"
            text = ""
        }
        var i = ssml.startIndex
        while i < ssml.endIndex {
            if ssml[i] == "<", let end = SSML.tagEnd(ssml, from: i) {
                flushText()
                out += tagSkeleton(ssml[ssml.index(after: i)..<end])
                i = ssml.index(after: end)
            } else {
                text.append(ssml[i])
                i = ssml.index(after: i)
            }
        }
        flushText()
        return out
    }

    private static func tagSkeleton(_ body: Substring) -> String {
        let trimmed = body.drop(while: { $0.isWhitespace })
        if trimmed.hasPrefix("!--") { return "<!--«\(max(0, trimmed.count - 5))»-->" }
        if trimmed.hasPrefix("?xml") { return "<?xml?>" }
        if trimmed.first == "?" || trimmed.first == "!" { return "<\(trimmed.first!)«\(trimmed.count)»>" }
        let isEnd = trimmed.first == "/"
        let b = isEnd ? trimmed.dropFirst() : trimmed
        let nameEnd = b.firstIndex(where: { $0.isWhitespace || $0 == "/" }) ?? b.endIndex
        let name = String(b[..<nameEnd]).filter { $0.isLetter || $0.isNumber || "-:_.".contains($0) }
        let selfClosing = b.last == "/"
        var attrs = ""
        let rest = String(b[nameEnd...])
        if let re = try? NSRegularExpression(pattern: #"([A-Za-z_][-A-Za-z0-9_:.]*)\s*=\s*(?:"([^"]*)"|'([^']*)')"#) {
            let ns = rest as NSString
            for m in re.matches(in: rest, range: NSRange(location: 0, length: ns.length)) {
                let key = ns.substring(with: m.range(at: 1))
                let r = m.range(at: 2).location != NSNotFound ? m.range(at: 2) : m.range(at: 3)
                let value = r.location != NSNotFound ? ns.substring(with: r) : ""
                if keptAttributes.contains(key.lowercased()) {
                    let safe = value.filter { $0.isASCII && ($0.isLetter || $0.isNumber || "+-.%:_ ".contains($0)) }
                    attrs += " \(key)=\"\(safe.prefix(40))\""
                } else {
                    attrs += " \(key)=«\(value.count)»"
                }
            }
        }
        return "<" + (isEnd ? "/" : "") + name + attrs + (selfClosing ? "/" : "") + ">"
    }

    /// letters -> a, digits -> 9, whitespace -> _ (runs of each collapsed to one), punctuation and
    /// symbols kept as they are (runs up to 3), anything else -> *.
    static func textClass(_ s: String) -> String {
        var out = ""
        var last: Character = "\0"
        var run = 0
        for ch in s {
            let c: Character
            if ch.isLetter { c = "a" }
            else if ch.isNumber { c = "9" }
            else if ch.isWhitespace { c = "_" }
            else if ch.unicodeScalars.count == 1, let u = ch.unicodeScalars.first, u.value < 0x2E80,
                    CharacterSet.punctuationCharacters.contains(u) || CharacterSet.symbols.contains(u) {
                c = u.value == 0x5F ? "\u{2017}" : ch   // a literal underscore, told apart from whitespace
            } else { c = "*" }
            if c == last {
                run += 1
                if "a9_*".contains(c) || run > 3 { continue }
            } else {
                run = 1
            }
            out.append(c)
            last = c
        }
        return out
    }
}
