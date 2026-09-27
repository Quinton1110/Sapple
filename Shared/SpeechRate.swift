//
//  SpeechRate.swift
//  One speed scale for VoiceOver and the in-app preview.
//
//  Speed is the engine's own control: the SAPI rate divides every sound's duration by 3^(rate/10),
//  exactly as the 2001 engine does it. Nothing is time-stretched.
//
//  Both VoiceOver and the app speak in terms of a slider fraction 0...1 (VoiceOver's Speaking Rate,
//  the app's Slow...Fast slider). 0.5 is the voice's natural speed; the lower half slows down to SAPI
//  rate -10 (a third of natural speed), the upper half speeds up to SAPI rate 18 (about 7.2 times),
//  the engine's own fast-listening limit.
//

import Foundation

enum SpeechRate {
    static let slowestSAPI = -10.0
    static let fastestSAPI = 18.0

    /// Slider fraction (0...1) -> SAPI rate (-10...18), 0.5 -> 0.
    static func sapiRate(fromFraction f: Double) -> Double {
        let x = min(1.0, max(0.0, f))
        if x <= 0.5 { return slowestSAPI * (1.0 - x / 0.5) }
        return fastestSAPI * ((x - 0.5) / 0.5)
    }

    /// Speed relative to natural (1.0 = the voice's own speed).
    static func speedFactor(sapiRate r: Double) -> Double { pow(3.0, r / 10.0) }

    /// VoiceOver's non-linear slider -> SSML prosody curve, inverted (learned on NeuralVoice):
    /// slider 0...50% is sent as 0...100%, slider 50...100% as 100...400%.
    static func fraction(fromVOProsodyPercent p: Double) -> Double {
        let f: Double
        if p <= 100.0 { f = (p / 100.0) * 0.5 }
        else { f = 0.5 + ((p - 100.0) / 300.0) * 0.5 }
        return min(1.0, max(0.0, f))
    }

    /// Spoken description, e.g. "normal speed", "1.5 times normal".
    static func describe(fraction f: Double) -> String {
        let factor = speedFactor(sapiRate: sapiRate(fromFraction: f))
        if abs(factor - 1.0) < 0.03 { return "normal speed" }
        return String(format: "%.1f times normal", factor)
    }
}
