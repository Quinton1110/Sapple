import SwiftUI
import AVFoundation

struct ContentView: View {
    @State private var voiceIndex = 0
    @State private var speed: Double = 0.5            // slider fraction, same scale VoiceOver uses
    @State private var text: String = ""
    @State private var status = ""
    @StateObject private var previewPlayer = PreviewPlayer()   // the Play / Stop button's audio (PreviewPlayer.swift)
    @State private var singing = SingingMode.load()   // shared with the extension (app group), saved on every change

    private var voices: [ClassicVoiceDef] {
        let v = VoiceCatalog.availableVoices
        return v.isEmpty ? CLASSIC_VOICES : v
    }
    private var voice: ClassicVoiceDef { voices[min(voiceIndex, max(0, voices.count - 1))] }
    private var speedPercent: Int { Int((speed * 100).rounded()) }
    private var speedDescription: String { SpeechRate.describe(fraction: speed) }
    private var transposeDescription: String {
        let t = Int(singing.transpose)
        return t == 0 ? "Transpose: none" : "Transpose: \(t > 0 ? "up" : "down") \(abs(t)) semitone\(abs(t) == 1 ? "" : "s")"
    }
    /// What VoiceOver says after "Transpose" (never the word itself): "none", "up 3 semitones", "down 12 semitones, one octave"
    private var transposeSpokenValue: String {
        let t = Int(singing.transpose), n = abs(t)
        if t == 0 { return "none" }
        var v = "\(t > 0 ? "up" : "down") \(n) semitone\(n == 1 ? "" : "s")"
        if n % 12 == 0 { v += n == 12 ? ", one octave" : ", two octaves" }
        return v
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Voice") {
                    Picker("Voice", selection: $voiceIndex) {
                        voiceSection("SAPI 5 voices") { $0.engine == .sapi5 || $0.engine == .anna }
                        voiceSection("Windows 10") { $0.engine == .onecore }
                        voiceSection("SAPI 4 voices") { $0.engine == .sapi4 }
                        voiceSection("TruVoice") { $0.engine == .truvoice }
                        voiceSection("Neural") { $0.engine == .neural }
                    }
                    .onChange(of: voiceIndex) { _, _ in
                        previewPlayer.stop()
                        prefill()
                    }
                }

                Section("Speed") {
                    Slider(value: $speed, in: 0...1, step: 0.05) {
                        Text("Speed")
                    } minimumValueLabel: {
                        Text("Slow")
                    } maximumValueLabel: {
                        Text("Fast")
                    }
                    .accessibilityValue("\(speedPercent) percent, \(speedDescription)")

                    // Visual only: the slider already announces this value.
                    Text("\(speedPercent)%, \(speedDescription)")
                        .font(.footnote).foregroundStyle(.secondary)
                        .accessibilityHidden(true)
                }

                // Always visible, whatever voice is picked (Quinton, 2026-09-25): applies to VoiceOver and the Play button.
                Section {
                    Toggle("Enable singing mode for SAPI 5 voices", isOn: $singing.enabled)
                        .accessibilityHint("Sam, Mike and Mary sing song scores they come across")
                    Slider(value: $singing.vibratoCents, in: 0...100, step: 5) {
                        Text("Vibrato depth")
                    } minimumValueLabel: {
                        Text("None")
                    } maximumValueLabel: {
                        Text("Wide")
                    }
                    .accessibilityValue(singing.vibratoCents == 0 ? "none" : "\(Int(singing.vibratoCents)) cents")
                    Slider(value: $singing.vibratoRate, in: 3...8, step: 0.5) {
                        Text("Vibrato speed")
                    } minimumValueLabel: {
                        Text("Slow")
                    } maximumValueLabel: {
                        Text("Fast")
                    }
                    .accessibilityValue(String(format: "%.1f per second", singing.vibratoRate))
                    Stepper(value: $singing.transpose, in: -24...12, step: 1) {
                        Text(transposeDescription)
                    }
                    // One adjustable element (Quinton 19:54): "Transpose, none, adjustable"; swipe up / down = +1 / -1
                    .accessibilityElement(children: .ignore)
                    .accessibilityLabel("Transpose")
                    .accessibilityValue(transposeSpokenValue)
                    .accessibilityAdjustableAction { direction in
                        switch direction {
                        case .increment: singing.transpose = min(12, singing.transpose + 1)
                        case .decrement: singing.transpose = max(-24, singing.transpose - 1)
                        @unknown default: break
                        }
                    }
                } header: {
                    Text("Singing")
                } footer: {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("When on, Sam, Mike and Mary sing any song score they come across, such as twin-kle C4 1 C4 1, and speak everything else normally.")
                        if !SingingMode.isShared {
                            Text("This copy of the app has no shared app group, so VoiceOver cannot see the singing switch; the Play button still sings.")
                        }
                    }
                }
                .onChange(of: singing) { _, new in SingingMode.save(new) }

                Section("Text") {
                    TextEditor(text: $text)
                        .frame(minHeight: 90)
                        .accessibilityLabel("Text to speak")
                        .onChange(of: text) { _, _ in previewPlayer.stop() }
                }

                Section {
                    // One button that plays and stops (Quinton, 2026-09-27). The same view in every state, only its
                    // label changes, so VoiceOver focus stays on it. Never disabled while it can stop something.
                    Button(action: playStopTapped) {
                        Label(previewPlayer.isActive ? "Stop" : "Play",
                              systemImage: previewPlayer.isActive ? "stop.circle.fill" : "play.circle.fill")
                    }
                    .disabled(!previewPlayer.isActive && text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    .accessibilityLabel(previewPlayer.isActive ? "Stop" : "Play")
                    .accessibilityHint(previewHint)

                    if !status.isEmpty {
                        Text(status).font(.footnote).foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Sapple")
        }
#if os(macOS)
        // A Mac window has no natural size; the form needs room for the picker and the text editor.
        .frame(minWidth: 460, idealWidth: 520, minHeight: 560, idealHeight: 640)
#endif
        .onAppear {
            prefill()
            // Tell the system the extension's voice list may have changed so it shows up in Settings
            // (iOS) / System Settings > Accessibility > Spoken Content (macOS).
            AVSpeechSynthesisProviderVoice.updateSpeechVoices()
#if os(iOS)
            if SelfTest.isRequested { SelfTest.run() }   // development only: -cvSelfTest launch argument
#endif
        }
    }

    @ViewBuilder
    private func voiceSection(_ title: String, matching: (ClassicVoiceDef) -> Bool) -> some View {
        let items = Array(voices.enumerated()).filter { matching($0.element) }
        if !items.isEmpty {
            Section(title) {
                ForEach(items, id: \.offset) { i, v in
                    Text(v.display).tag(i)
                }
            }
        }
    }

    // MARK: - Actions

    private func prefill() {
        let name = VoiceCatalog.shortName(voice)
        let prefix = String(voice.language.prefix(2))
        switch prefix {
        case "ja": text = "こんにちは、\(name)です。"
        case "zh": text = voice.language == "zh-HK" ? "你好，我係\(name)。" : "你好，我是\(name)。"
        case "ko": text = "안녕하세요, \(name)입니다."
        case "de": text = "Hallo, hier spricht \(name)."
        case "fr": text = "Bonjour, voici la voix de \(name)."
        case "es": text = "Hola, esta es la voz de \(name)."
        case "it": text = "Ciao, questa è la voce di \(name)."
        case "pt": text = "Olá, esta é a voz de \(name)."
        case "ru": text = "Здравствуйте, это голос \(name)."
        default:
#if os(macOS)
            text = "This is \(name), natively on the Mac."
#else
            text = "This is \(name), natively on iOS."
#endif
        }
    }

    private var previewHint: String {
        previewPlayer.isActive ? "Stops the preview" : "Speaks the text with the selected voice and speed"
    }

    /// Play from the beginning when idle; otherwise stop.
    private func playStopTapped() {
        if previewPlayer.isActive {
            previewPlayer.stop()
            // Only a stop is announced: on play the preview itself is the answer, and speech over it would clash.
            AccessibilityNotification.Announcement("Play").post()
        } else {
            preview()
        }
    }

    private func preview() {
        status = ""
        let g = previewPlayer.beginLoading()
        let t = text, v = voice, rate = SpeechRate.sapiRate(fromFraction: speed), sing = singing
        DispatchQueue.global(qos: .userInitiated).async {
            var mismatches: Int32 = 0
            let samples = ClassicEngine.shared.synthesize(text: t, voice: v, sapiRate: rate, singing: sing,
                                                          mismatches: &mismatches)
            DispatchQueue.main.async {
                guard g == previewPlayer.generation else { return }   // stopped, or the text or voice changed, meanwhile
                if mismatches > 0 {   // a pasted score: some word's notes do not match its syllables
                    status = "\(mismatches) word\(mismatches == 1 ? "" : "s") in the score had a different number of notes than syllables."
                }
                guard let samples else {
                    status = "Synthesis failed."
                    previewPlayer.loadFailed(generation: g)
                    return
                }
                guard !samples.isEmpty else {
                    status = "Nothing to speak in that text."
                    previewPlayer.loadFailed(generation: g)
                    return
                }
                if let error = previewPlayer.deliver(samples, sampleRate: Int(ClassicEngine.sampleRate), generation: g) {
                    status = error
                }
            }
        }
    }
}
