import Foundation
import AVFoundation

/// The preview button's player: one button that plays and stops (Quinton, 2026-09-27: "the button for previewing text
/// should play and pause, not just play", then "no, play stop, not pause").
///
///   idle      -> tap: synthesize, then play from the start      (button says "Play")
///   loading   -> tap: stop; the audio is thrown away on arrival  (button says "Stop")
///   playing   -> tap: stop at once and drop the audio            (button says "Stop")
///
/// Playback finishing on its own, or `stop()` for any other reason (the text or voice changed), returns to idle, so the
/// next tap always starts from the beginning. There is no pause / resume.
@MainActor
final class PreviewPlayer: NSObject, ObservableObject, AVAudioPlayerDelegate {
    enum State: Equatable { case idle, loading, playing }

    @Published private(set) var state: State = .idle
    private var player: AVAudioPlayer?
    /// Bumped by stop(): audio still being synthesized for an older request is thrown away when it arrives.
    private(set) var generation = 0
    /// For tests: play the audio silently.
    var volume: Float = 1

    /// True while the button should read "Stop" (playing, or about to).
    var isActive: Bool { state != .idle }

    override init() {
        super.init()
#if os(iOS)
        // A phone call or another app taking the audio pauses the player without telling its delegate: treat that as a
        // stop so the button says "Play" again and the next tap starts from the beginning.
        NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: nil,
                                               queue: .main) { [weak self] note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            guard raw.flatMap(AVAudioSession.InterruptionType.init) == .began else { return }
            MainActor.assumeIsolated {
                guard let self, self.state == .playing else { return }
                self.stop()
            }
        }
#endif
    }

    /// Starts a new preview: returns the generation the synthesized audio must be delivered with.
    func beginLoading() -> Int {
        stop()
        state = .loading
        return generation
    }

    /// The synthesized audio for `generation`. Ignored if the request was stopped in the meantime.
    /// Returns an error message if playback could not start (the state is then idle).
    @discardableResult
    func deliver(_ samples: [Int16], sampleRate: Int, generation g: Int) -> String? {
        guard g == generation, state == .loading else { return nil }
        do {
#if os(iOS)
            // macOS has no AVAudioSession: AVAudioPlayer plays straight to the default output.
            try AVAudioSession.sharedInstance().setCategory(.playback, options: [.duckOthers])
            try AVAudioSession.sharedInstance().setActive(true)
#endif
            let p = try AVAudioPlayer(data: Self.wavData(samples, sampleRate: sampleRate))
            p.delegate = self
            p.volume = volume
            p.prepareToPlay()
            player = p
            guard p.play() else {
                stop()
                return "Playback could not start."
            }
            state = .playing
            return nil
        } catch {
            stop()
            return "Playback error: \(error.localizedDescription)"
        }
    }

    /// Synthesis produced nothing (or failed) for `generation`: back to idle.
    func loadFailed(generation g: Int) {
        guard g == generation, state == .loading else { return }
        state = .idle
    }

    /// Stop now, drop the audio (and any audio still being synthesized), and go back to "Play".
    func stop() {
        generation &+= 1
        player?.delegate = nil
        player?.stop()
        player = nil
        if state != .idle { state = .idle }
    }

    /// Whether audio is loaded and playing (tests).
    var isPlayingAudio: Bool { player?.isPlaying ?? false }

    nonisolated func audioPlayerDidFinishPlaying(_ p: AVAudioPlayer, successfully flag: Bool) {
        MainActor.assumeIsolated { finished(p) }
    }

    nonisolated func audioPlayerDecodeErrorDidOccur(_ p: AVAudioPlayer, error: Error?) {
        MainActor.assumeIsolated { finished(p) }
    }

    private func finished(_ p: AVAudioPlayer) {
        guard p === player else { return }
        stop()
    }

    /// Wrap raw Int16 mono PCM in a minimal WAV container for AVAudioPlayer.
    nonisolated static func wavData(_ samples: [Int16], sampleRate: Int) -> Data {
        func u32(_ v: UInt32) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }
        func u16(_ v: UInt16) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }
        let dataLen = samples.count * 2
        var d = Data()
        d.append(Data("RIFF".utf8)); d.append(u32(UInt32(36 + dataLen))); d.append(Data("WAVE".utf8))
        d.append(Data("fmt ".utf8)); d.append(u32(16)); d.append(u16(1)); d.append(u16(1))
        d.append(u32(UInt32(sampleRate))); d.append(u32(UInt32(sampleRate * 2)))
        d.append(u16(2)); d.append(u16(16))
        d.append(Data("data".utf8)); d.append(u32(UInt32(dataLen)))
        samples.withUnsafeBytes { d.append(contentsOf: $0) }
        return d
    }
}
