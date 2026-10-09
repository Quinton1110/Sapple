//
//  ClassicVoicesProvider.swift
//  AVSpeechSynthesisProviderAudioUnit that speaks VoiceOver (and any other speech client) as
//  Microsoft Sam, Mike, Mary and the SAPI 4 voice modes, fully offline - both the SAPI 5 engine
//  reconstruction and Microsoft's SAPI 4 engine (decompiled to C, Engine/sapi4); ClassicEngine picks by voice.
//
//  synthesizeSpeechRequest() starts the engine on a background thread and returns as soon as the
//  first audio exists; the engine's PCM streams through PCMStream into the render block while the
//  rest of the utterance is still being made. Output is the engine's native 22050 Hz, as Float32
//  non-interleaved mono.
//
//  Lessons carried over from NeuralVoice / DECtalk:
//   - iOS creates a FRESH audio unit per utterance: no per-instance counters or remembered state;
//     anything that must persist (the per-voice rate) is static.
//   - VoiceOver omits the rate on some announcements (voice switching), so the fallback is that
//     VOICE's last rate, not a global one.
//   - Each piece's baked-in leading/trailing silence is trimmed (cv_bridge.c), so digit-by-digit
//     speech does not stack dead air; the pauses VoiceOver asks for (<s> boundaries between label,
//     trait and value, <break>s) are put back as real silence of a calibrated length (SSML.swift).
//   - Nothing is ever spoken for an empty request (no sample text injection).
//   - No logging of spoken text: this is a screen reader voice.
//

import AVFoundation
import OSLog

private let log = Logger(subsystem: "com.quinton.classicvoices", category: "Provider")

public final class ClassicVoicesProvider: AVSpeechSynthesisProviderAudioUnit {
    private let outputBus: AUAudioUnitBus
    private var _outputBusses: AUAudioUnitBusArray!
    private let format: AVAudioFormat
    private let stream = PCMStream()

    @objc override init(componentDescription: AudioComponentDescription,
                        options: AudioComponentInstantiationOptions) throws {
        let asbd = AudioStreamBasicDescription(
            mSampleRate: ClassicEngine.sampleRate,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
            mBytesPerPacket: 4,
            mFramesPerPacket: 1,
            mBytesPerFrame: 4,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 32,
            mReserved: 0
        )
        self.format = AVAudioFormat(cmAudioFormatDescription:
            try CMAudioFormatDescription(audioStreamBasicDescription: asbd))
        self.outputBus = try AUAudioUnitBus(format: self.format)
        try super.init(componentDescription: componentDescription, options: options)
        self._outputBusses = AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
    }

    deinit {
        stream.cancel()   // releases a producer that may still be waiting for room
    }

    public override var outputBusses: AUAudioUnitBusArray { _outputBusses }

    public override func allocateRenderResources() throws {
        try super.allocateRenderResources()
    }

    // MARK: - Render (drains the stream)

    public override var internalRenderBlock: AUInternalRenderBlock {
        let stream = self.stream
        return { actionFlags, _, frameCount, _, outputAudioBufferList, _, _ in
            let abl = UnsafeMutableAudioBufferListPointer(outputAudioBufferList)
            guard let raw = abl[0].mData else { return noErr }
            let out = raw.assumingMemoryBound(to: Float32.self)
            let frames = Int(frameCount)
            out.update(repeating: 0, count: frames)

            let (n, done) = stream.read(into: out, frames: frames, maxWait: 0.25)
            if !done {
                // A full buffer, padded with silence if the engine has not caught up within the wait (not expected:
                // read waits for a full buffer and the engines run far faster than real time). Only the end is short.
                abl[0].mDataByteSize = UInt32(frames * MemoryLayout<Float32>.size)
                return noErr
            }
            abl[0].mDataByteSize = UInt32(n * MemoryLayout<Float32>.size)
            actionFlags.pointee = .offlineUnitRenderAction_Complete
            return noErr
        }
    }

    // MARK: - Speech requests

    public override func synthesizeSpeechRequest(_ speechRequest: AVSpeechSynthesisProviderRequest) {
        let ssml = speechRequest.ssmlRepresentation
        let def = VoiceCatalog.def(forIdentifier: speechRequest.voice.identifier)
        let rate = Self.sapiRate(fromSSML: ssml, voiceSlug: def.slug)
        // The neural voices also pause at line breaks (the other engines' text is split exactly as before).
        let segments = SSML.segments(ssml, lineBreaks: def.engine == .neural)
        // Diagnostics carry no spoken content.
        log.info("request voice=\(def.slug, privacy: .public) segments=\(segments.count) rate=\(rate, format: .fixed(precision: 1))")
        // Off unless Quinton switches it on in the app (15 minutes at most); records structure, never words.
        SSMLCapture.record(ssml: ssml, voice: def.slug, segments: segments,
                           speed: ClassicEngine.effectiveSpeed(sapiRate: rate, engine: def.engine))

        // Singing mode (ClassicEngine.speak(scored:)): with the app's switch on (read from the app group for every
        // request), SAPI 5 voices sing any song score inside the text and speak the rest; every other voice just speaks.
        let singing: SingingSettings? = def.engine == .sapi5 ? SingingMode.load() : nil

        let stream = self.stream
        let g = stream.begin()
        DispatchQueue.global(qos: .userInteractive).async {
            // Each segment its own engine call; every pause (VoiceOver's <s> boundaries between label,
            // trait and value, and <break>s) is real silence - see SSML.swift.
            // Pitch comes from each segment (scoped to its <prosody>), not one value for the whole request.
            let rc = ClassicEngine.shared.speak(segments: segments, voice: def, sapiRate: rate, semitones: 0,
                                                singing: singing,
                                                onPCM: { pcm in stream.append(g, pcm) },
                                                silence: { ms in stream.appendSilence(g, ms: ms) })
            if rc < 0 { log.error("synthesis failed voice=\(def.slug, privacy: .public)") }
            // A short tail so even an empty utterance hands the host a frame and completes.
            if rc != 1 { stream.appendSilence(g, ms: 10) }
            stream.finish(g)
        }
        // Return once the first audio exists (typically a few milliseconds).
        stream.waitForAudio(g, timeout: 0.3)
    }

    public override func cancelSpeechRequest() {
        stream.cancel()
        SSMLCapture.recordCancel()
    }

    // MARK: - Voices

    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            CLASSIC_VOICES.map { def in
                let v = AVSpeechSynthesisProviderVoice(
                    name: def.display,
                    identifier: def.identifier,
                    primaryLanguages: [def.language],
                    supportedLanguages: [def.language]
                )
                v.gender = def.gender
                return v
            }
        }
        set { }
    }

    // MARK: - Rate (per-voice memory)

    // VoiceOver omits the prosody rate on some announcements (notably the voice-name announcement when
    // you SWITCH voices). iOS keeps a rate PER VOICE, so the fallback is per voice too; a fresh audio
    // unit per utterance means it must be static.
    private static var lastFractionByVoice: [String: Double] = [:]
    private static let rateLock = NSLock()

    static func sapiRate(fromSSML ssml: String, voiceSlug: String) -> Double {
        rateLock.lock(); defer { rateLock.unlock() }
        let fraction: Double
        if let pct = SSML.ratePercent(ssml) {
            fraction = SpeechRate.fraction(fromVOProsodyPercent: pct)
            lastFractionByVoice[voiceSlug] = fraction
        } else {
            fraction = lastFractionByVoice[voiceSlug] ?? 0.5
        }
        return SpeechRate.sapiRate(fromFraction: fraction)
    }
}
