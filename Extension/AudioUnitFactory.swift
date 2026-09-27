//
//  AudioUnitFactory.swift
//  Principal class of the speech-provider extension.
//
//  The @objc name is bare (not module-prefixed) and matches NSExtensionPrincipalClass in
//  Extension/Info.plist - the same pattern that works for NeuralVoice.
//

import CoreAudioKit

@objc(ClassicVoicesProviderFactory)
public class ClassicVoicesProviderFactory: NSObject, AUAudioUnitFactory {
    public func beginRequest(with context: NSExtensionContext) {}

    @objc public func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        return try ClassicVoicesProvider(componentDescription: componentDescription, options: [])
    }
}
