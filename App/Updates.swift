//
//  Updates.swift
//  Mac only: Sparkle 2 updates of the app itself, and the switch for the background update checker.
//
//  - While the app is open, Sparkle's standard updater (SPUStandardUpdaterController) checks on its own schedule
//    (Info-macOS.plist: SUEnableAutomaticChecks, SUScheduledCheckInterval one day, SUAutomaticallyUpdate NO, so
//    it always asks before installing), and "Check for Updates..." in the app menu checks at once.
//  - While it is closed, "Sapple Updates" (UpdateChecker/, a login item inside this app) checks and posts a
//    notification; opening the notification opens this app (sapple-updates:check), which shows Sparkle's
//    standard update window. The helper never downloads or installs anything. After an update it notices its app was
//    replaced and exits; launchd starts the new one (UpdateChecker.swift).
//  - How often both check is the Updates section's "How often to check for updates" picker (every hour, 6 hours, day - the
//    default - or week): the app's Sparkle setting, mirrored through the app group (UpdateFeed.checkIntervalKey)
//    for the helper, which applies it as soon as it changes.
//  - The feed is SUFeedURL in Info-macOS.plist; every update is checked against SUPublicEDKey (EdDSA). Builds made
//    with CV_SPARKLE_TEST (never a release) read a local test feed instead (UpdateFeed.testURL).
//  - The app is sandboxed: Sparkle installs through the installer XPC service inside Sparkle.framework
//    (SUEnableInstallerLauncherService, and the -spks / -spki mach-lookup exceptions in App-macOS.entitlements).
//    The app itself has no network access (Sapple is offline): the appcast and the update are fetched by Sparkle's
//    downloader XPC service inside Sparkle.framework (SUEnableDownloaderService), the only part with network access.
//

#if os(macOS)
import AppKit
import Combine
import ServiceManagement
import Sparkle
import SwiftUI

/// Shared by the app and the helper (UpdateChecker/), so the two agree.
enum UpdateFeed {
    /// The helper's bundle identifier: its login item (SMAppService.loginItem).
    static let checkerBundleID = "com.quinton.classicvoices.UpdateChecker"
    /// What opening the helper's notification opens.
    static let showUpdateURL = URL(string: "sapple-updates:check")!
    /// The Mac app group (App-macOS.entitlements, the singing switch's): where "Skip This Version" is shared with the helper.
    static let appGroup = "ZGJQ5DZFK2.com.quinton.classicvoices"
    static let skippedBuildKey = "updates.skippedBuild"
    /// How often to check, in seconds (Quinton, 2026-11-21: "Why not just have the check be configurable?"): the app's
    /// Sparkle setting, mirrored here in the app group for the helper, which applies it at once.
    static let checkIntervalKey = "updates.checkInterval"
    /// Posted after the interval changes, so a running helper applies it at once (a Darwin notification, named with the
    /// app group, which sandboxed processes may use; key-value observing of the app group's defaults does not report
    /// another process's change, measured 2026-11-21).
    static let checkIntervalChanged = "ZGJQ5DZFK2.com.quinton.classicvoices.checkIntervalChanged"
    /// The choices; Sparkle 2.10.1 never checks more often than once an hour (SPUUpdaterSettings.minimumUpdateCheckInterval).
    static let intervals: [(seconds: Double, title: String)] = [
        (3600, "Every hour"), (6 * 3600, "Every 6 hours"), (86400, "Every day"), (7 * 86400, "Every week")]
    static let defaultInterval: Double = 86400   // SUScheduledCheckInterval in Info-macOS.plist
#if CV_SPARKLE_TEST
    /// Test builds only (CV_SPARKLE_TEST): a feed served on this Mac.
    static let testURL = "http://127.0.0.1:8766/appcast.xml"
#endif
}

@MainActor
final class AppUpdates: NSObject, ObservableObject, SPUUpdaterDelegate {
    static let shared = AppUpdates()

    @Published private(set) var canCheckForUpdates = false
    @Published private(set) var automaticChecks = true
    @Published private(set) var backgroundChecks = false
    @Published private(set) var backgroundStatus = ""
    @Published private(set) var checkInterval: Double = UpdateFeed.defaultInterval

    private var controller: SPUStandardUpdaterController!
    private var observers: [AnyCancellable] = []
    private var checkWhenReady = false
    private let checker = SMAppService.loginItem(identifier: UpdateFeed.checkerBundleID)
    private let defaults = UserDefaults.standard
    private static let registeredOnceKey = "updates.backgroundRegisteredOnce"

    private override init() {
        super.init()
        controller = SPUStandardUpdaterController(startingUpdater: true, updaterDelegate: self, userDriverDelegate: nil)
        let updater = controller.updater
        updater.publisher(for: \.canCheckForUpdates).sink { [weak self] can in
            self?.canCheckForUpdates = can
            self?.checkIfWaiting()
        }.store(in: &observers)
        updater.publisher(for: \.sessionInProgress).sink { [weak self] _ in
            DispatchQueue.main.async { self?.checkIfWaiting() }   // after the value has changed
        }.store(in: &observers)
        updater.publisher(for: \.automaticallyChecksForUpdates).sink { [weak self] in self?.automaticChecks = $0 }.store(in: &observers)
        // The first Sparkle-enabled launch turns the background checker on (Quinton, 2026-11-21: "both kinds of
        // checking"); after that only the switch changes it, so turning it off stays off.
        if !defaults.bool(forKey: Self.registeredOnceKey) {
            defaults.set(true, forKey: Self.registeredOnceKey)
            if updater.automaticallyChecksForUpdates { setBackgroundChecks(true) }
        }
#if CV_SPARKLE_TEST
        // test builds only: -cvBackgroundChecks on|off does what the switch does (tests drive it without the UI)
        if let arg = defaults.string(forKey: "cvBackgroundChecks") { setBackgroundChecks(arg == "on") }
#endif
        // the helper reads the interval from the app group: keep it equal to the app's own
        checkInterval = updater.updateCheckInterval
        UserDefaults(suiteName: UpdateFeed.appGroup)?.set(checkInterval, forKey: UpdateFeed.checkIntervalKey)
#if CV_SPARKLE_TEST
        // test builds only: -cvCheckInterval <seconds> does what the picker does
        if defaults.object(forKey: "cvCheckInterval") != nil { setCheckInterval(defaults.double(forKey: "cvCheckInterval")) }
#endif
        NSLog("SappleUpdates: check interval %.0f s", controller.updater.updateCheckInterval)
        refreshBackgroundStatus()
    }

    /// "How often to check for updates": for the app and the background checker alike. Sparkle reschedules the app's
    /// next check itself; the helper sees the app-group value change and does the same.
    func setCheckInterval(_ seconds: Double) {
        let s = max(seconds, 3600)   // Sparkle's minimum (it would round anything shorter up to an hour anyway)
        controller.updater.updateCheckInterval = s
        checkInterval = s
        UserDefaults(suiteName: UpdateFeed.appGroup)?.set(s, forKey: UpdateFeed.checkIntervalKey)
        CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                             CFNotificationName(UpdateFeed.checkIntervalChanged as CFString), nil, nil, true)
        NSLog("SappleUpdates: check interval set to %.0f s", s)
    }

    /// "Check for Updates..." (the app menu and the Updates section).
    func checkForUpdates() { controller.checkForUpdates(nil) }

    /// The helper's notification (sapple-updates:check): right after launch Sparkle is still starting and would
    /// drop a check, so it waits until Sparkle can take one.
    func checkWhenSparkleIsReady() {
        checkWhenReady = true
        checkIfWaiting()
    }

    private func checkIfWaiting() {
        let updater = controller.updater
        guard checkWhenReady, updater.canCheckForUpdates, !updater.sessionInProgress else { return }
        checkWhenReady = false
        controller.checkForUpdates(nil)
    }

    /// "Check for updates automatically": Sparkle's own setting; off also turns the background checker off.
    func setAutomaticChecks(_ on: Bool) {
        controller.updater.automaticallyChecksForUpdates = on
        if !on { setBackgroundChecks(false) }
    }

    /// "Also check when Sapple is closed": the helper's login item.
    func setBackgroundChecks(_ on: Bool) {
        do {
            if on {
                if checker.status != .enabled { try checker.register() }
            } else if checker.status == .enabled || checker.status == .requiresApproval {
                try checker.unregister()
            }
        } catch {
            NSLog("SappleUpdates: background checker %@ failed: %@", on ? "register" : "unregister", "\(error)")
        }
        refreshBackgroundStatus()
    }

    func refreshBackgroundStatus() {
        NSLog("SappleUpdates: background checker status %ld (0 not registered, 1 enabled, 2 requires approval, 3 not found)",
              checker.status.rawValue)
        switch checker.status {
        case .enabled:
            backgroundChecks = true; backgroundStatus = ""
        case .requiresApproval:
            backgroundChecks = true
            backgroundStatus = "Allow Sapple Updates in System Settings, General, Login Items."
        default:
            backgroundChecks = false; backgroundStatus = ""
        }
    }

    // MARK: SPUUpdaterDelegate

#if CV_SPARKLE_TEST
    nonisolated func feedURLString(for updater: SPUUpdater) -> String? { UpdateFeed.testURL }
#endif

    /// "Skip This Version" here also quiets the helper: it reads the skipped build from the app group.
    nonisolated func updater(_ updater: SPUUpdater, userDidMake choice: SPUUserUpdateChoice, forUpdate updateItem: SUAppcastItem,
                             state: SPUUserUpdateState) {
        let build = updateItem.versionString
        DispatchQueue.main.async {
            if choice == .skip { UserDefaults(suiteName: UpdateFeed.appGroup)?.set(build, forKey: UpdateFeed.skippedBuildKey) }
        }
    }
}

/// The app menu's "Check for Updates..." (after "About Sapple", where Mac apps put it).
struct CheckForUpdatesCommand: View {
    @ObservedObject var updates: AppUpdates
    var body: some View {
        Button("Check for Updates\u{2026}") { updates.checkForUpdates() }
            .disabled(!updates.canCheckForUpdates)
            .accessibilityLabel("Check for Updates")
    }
}

/// The Updates section of the window.
struct UpdatesSection: View {
    @ObservedObject var updates: AppUpdates
    var body: some View {
        Section("Updates") {
            Toggle("Check for updates automatically", isOn: Binding(
                get: { updates.automaticChecks }, set: { updates.setAutomaticChecks($0) }))
            Picker("How often to check for updates", selection: Binding(
                get: { UpdateFeed.intervals.contains { $0.seconds == updates.checkInterval } ? updates.checkInterval
                                                                                              : UpdateFeed.defaultInterval },
                set: { updates.setCheckInterval($0) })) {
                ForEach(UpdateFeed.intervals, id: \.seconds) { Text($0.title).tag($0.seconds) }
            }
            .disabled(!updates.automaticChecks)
            Toggle("Also check when Sapple is closed", isOn: Binding(
                get: { updates.backgroundChecks }, set: { updates.setBackgroundChecks($0) }))
                .disabled(!updates.automaticChecks)
            if !updates.backgroundStatus.isEmpty {
                Text(updates.backgroundStatus).font(.footnote)
            }
            Button("Check for Updates\u{2026}") { updates.checkForUpdates() }
                .disabled(!updates.canCheckForUpdates)
                .accessibilityLabel("Check for Updates")
        }
        .onAppear { updates.refreshBackgroundStatus() }
    }
}

/// Starts the updater at launch.
final class UpdatesAppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) { _ = AppUpdates.shared }
}

extension View {
    /// The helper's notification opens sapple-updates:check: the window already open takes it (no second window)
    /// and Sparkle's update window comes up.
    func opensUpdatesFromNotification() -> some View {
        self.handlesExternalEvents(preferring: ["*"], allowing: ["*"])
            .onOpenURL { url in
                if url.scheme == UpdateFeed.showUpdateURL.scheme { AppUpdates.shared.checkWhenSparkleIsReady() }
            }
    }
}
#endif
