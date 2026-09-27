//
//  SingingMode.swift
//  "Enable singing mode for SAPI 5 voices" (Quinton, 2026-09-25; final form 17:27): with the switch on, the SAPI 5
//  Sam / Mike / Mary voices (Engine/sam, every "(SAPI 5)" voice including the Hall / Stadium / Space / RoboSoft /
//  Whisper variants) speak normally and SING any song score they come across in the text - "twin-kle C4 1 C4 1 ..."
//  (ClassicEngine.speak(scored:), cv_score_find), DECtalk style: speak, sing the notation, speak on. Switch off:
//  everything is spoken. Two settings go with it: vibrato (depth in cents and speed) and transposition (semitones,
//  -24...+12), applied to the sung scores.
//
//  Shared between the app (writes) and the speech extension (reads, once per request) through the app group:
//  its preferences, plus Library/singing-mode.plist in the group container as a fallback - the same pattern as
//  SSMLCapture, whose switch the extension has been seen to read on the phone (the extension can READ the group,
//  not write it). The group identifier is "group.com.quinton.classicvoices" on iOS and
//  "ZGJQ5DZFK2.com.quinton.classicvoices" on macOS (a Mac group must start with the team ID). A copy re-signed by
//  AltStore gets its groups rewritten; AltStore lists the new identifiers under ALTAppGroups in Info.plist, so
//  that list is tried first. If no group is reachable the switch simply reads as off: speech stays normal.
//
//  Privacy: settings only, never text.
//

import Foundation

struct SingingSettings: Equatable {
    var enabled = false
    var vibratoCents: Double = 30      // 0...200
    var vibratoRate: Double = 5.5      // Hz, 2...9
    var transpose: Double = 0          // semitones, -24...+12

    /// The engine's view of it (cv_bridge.h).
    var bridge: cv_sing_settings {
        cv_sing_settings(vibrato_cents: max(0, min(200, vibratoCents)), vibrato_rate: max(2, min(9, vibratoRate)),
                         transpose: max(-24, min(12, transpose)))
    }
}

enum SingingMode {
#if os(macOS)
    static let defaultGroupID = "ZGJQ5DZFK2.com.quinton.classicvoices"
#else
    static let defaultGroupID = "group.com.quinton.classicvoices"
#endif

    /// The app group actually granted to this copy: AltStore's rewritten one if it left us a list, else ours.
    static var groupID: String {
        for b in [Bundle.main, Bundle(for: Marker.self)] {
            if let groups = b.object(forInfoDictionaryKey: "ALTAppGroups") as? [String],
               let g = groups.first(where: { $0.hasPrefix("group.com.quinton.classicvoices") }) ?? groups.first {
                return g
            }
        }
        return defaultGroupID
    }
    private final class Marker {}

    private static let key = "singingMode.v1"
    private static var defaults: UserDefaults? { UserDefaults(suiteName: groupID) }
    private static var fileURL: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID)?
            .appendingPathComponent("Library/singing-mode.plist")
    }

    private static func encode(_ s: SingingSettings) -> [String: Any] {
        ["enabled": s.enabled, "vibratoCents": s.vibratoCents, "vibratoRate": s.vibratoRate, "transpose": s.transpose]
    }
    private static func decode(_ d: [String: Any]) -> SingingSettings {
        var s = SingingSettings()
        s.enabled = d["enabled"] as? Bool ?? false
        s.vibratoCents = d["vibratoCents"] as? Double ?? s.vibratoCents
        s.vibratoRate = d["vibratoRate"] as? Double ?? s.vibratoRate
        s.transpose = d["transpose"] as? Double ?? s.transpose
        return s
    }

    /// Read fresh every time (the extension calls this once per speech request).
    static func load() -> SingingSettings {
        if let d = defaults?.dictionary(forKey: key) { return decode(d) }
        if let url = fileURL, let data = try? Data(contentsOf: url),
           let d = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] {
            return decode(d)
        }
        return SingingSettings()
    }

    /// The app saves every change to both places.
    static func save(_ s: SingingSettings) {
        let d = encode(s)
        defaults?.set(d, forKey: key)
        if let url = fileURL, let data = try? PropertyListSerialization.data(fromPropertyList: d, format: .binary, options: 0) {
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? data.write(to: url, options: .atomic)
        }
    }

    /// Whether the group is reachable at all from this process (the app shows a note when it is not).
    static var isShared: Bool { FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID) != nil }
}
