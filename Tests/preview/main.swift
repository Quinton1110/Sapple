// The Play / Stop button's player (App/PreviewPlayer.swift), on the Mac: `make preview-test`.
// Silent audio at volume 0, so nothing is heard. Exits non-zero on the first failure.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String) {
    print("\(ok ? "ok  " : "FAIL") \(what)")
    if !ok { failures += 1 }
}
func spin(_ seconds: Double) { RunLoop.main.run(until: Date().addingTimeInterval(seconds)) }
let rate = 22050
func silence(_ seconds: Double) -> [Int16] { [Int16](repeating: 0, count: Int(Double(rate) * seconds)) }

MainActor.assumeIsolated {
    let p = PreviewPlayer()
    p.volume = 0
    check(p.state == .idle && !p.isActive, "starts idle (button says Play)")

    // Play: loading, then playing; the button says Stop throughout.
    var g = p.beginLoading()
    check(p.state == .loading && p.isActive, "tap Play: loading, button says Stop")
    check(p.deliver(silence(3), sampleRate: rate, generation: g) == nil, "audio delivered")
    check(p.state == .playing && p.isPlayingAudio, "playing")

    // Stop: immediately idle, the audio is gone.
    spin(0.2)
    p.stop()
    check(p.state == .idle && !p.isActive && !p.isPlayingAudio, "tap Stop: idle at once, audio dropped")

    // Stop while still synthesizing: the late audio is thrown away.
    g = p.beginLoading()
    p.stop()
    check(p.deliver(silence(1), sampleRate: rate, generation: g) == nil && p.state == .idle && !p.isPlayingAudio,
          "stop while loading: late audio ignored, stays idle")

    // A new Play after a stale request: only the newest generation plays.
    let old = p.beginLoading()
    let new = p.beginLoading()
    p.deliver(silence(1), sampleRate: rate, generation: old)
    check(p.state == .loading, "older request's audio ignored")
    p.deliver(silence(1), sampleRate: rate, generation: new)
    check(p.state == .playing, "newest request plays")
    p.stop()

    // Natural finish: back to Play by itself.
    g = p.beginLoading()
    p.deliver(silence(0.3), sampleRate: rate, generation: g)
    check(p.state == .playing, "short clip playing")
    var waited = 0.0
    while p.state != .idle && waited < 5 { spin(0.1); waited += 0.1 }
    check(p.state == .idle && !p.isPlayingAudio, "finished on its own: idle (button says Play)")

    // Nothing to play.
    g = p.beginLoading()
    p.loadFailed(generation: g)
    check(p.state == .idle, "synthesis produced nothing: idle")

    // The WAV header the self-test and player share.
    let wav = PreviewPlayer.wavData([1, -1], sampleRate: rate)
    check(wav.count == 48 && wav.prefix(4) == Data("RIFF".utf8), "WAV container: 44-byte header + samples")
}
print(failures == 0 ? "preview-test: all passed" : "preview-test: \(failures) failed")
exit(failures == 0 ? 0 : 1)
