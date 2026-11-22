//
//  UpdateChecker.swift
//  "Sapple Updates": the Mac app's background update checker, a login item inside the app
//  (Contents/Library/LoginItems, registered by App/Updates.swift with SMAppService), so updates are found while
//  Sapple is closed.
//
//  It runs Sparkle's updater ON THE MAIN APP'S BUNDLE (its feed, its EdDSA key, its version, its check interval),
//  on Sparkle's schedule, with a user driver of its own that never downloads or installs: an update found is a
//  notification, "Sapple <version> is available"; opening it opens the app, which shows Sparkle's standard
//  update window (App/Updates.swift), so installing is always the user's choice, in the app, exactly as from
//  "Check for Updates...". A version skipped in that window is not announced again; nothing is announced while
//  the app is open (it checks on its own). If notifications are not allowed, the app's own check finds the update
//  the next time it is opened.
//
//  No window, no Dock icon (LSUIElement), no speech engine: it never touches the voices.
//

import AppKit
import Sparkle
import UserNotifications

@main
final class UpdateChecker: NSObject, NSApplicationDelegate, SPUUpdaterDelegate, SPUUserDriver, UNUserNotificationCenterDelegate {
    static let mainAppBundleID = "com.quinton.classicvoices"
    static let showUpdateURL = URL(string: "sapple-updates:check")!
    static let appGroup = "ZGJQ5DZFK2.com.quinton.classicvoices"
    static let skippedBuildKey = "updates.skippedBuild"
    static let checkIntervalKey = "updates.checkInterval"   // seconds, set by the app's "How often to check for updates" picker
    /// Posted by the app when the interval changes (a Darwin notification: sandboxed processes may use names that start
    /// with their app group).
    static let checkIntervalChanged = "ZGJQ5DZFK2.com.quinton.classicvoices.checkIntervalChanged"

    private var updater: SPUUpdater?
    private let center = UNUserNotificationCenter.current()

    static func main() {
        let app = NSApplication.shared
        let delegate = UpdateChecker()
        app.delegate = delegate
        app.run()
    }

    /// .../Sapple.app/Contents/Library/LoginItems/Sapple Updates.app -> .../Sapple.app
    private static var mainAppURL: URL {
        var u = Bundle.main.bundleURL
        for _ in 0..<4 { u.deleteLastPathComponent() }
        return u
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        center.delegate = self
        watchForReplacement()
        guard let app = Bundle(url: Self.mainAppURL), app.bundleIdentifier == Self.mainAppBundleID else {
            NSLog("SappleUpdateChecker: not inside Sapple.app (%@); nothing to check", Self.mainAppURL.path)
            return
        }
        let updater = SPUUpdater(hostBundle: app, applicationBundle: app, userDriver: self, delegate: self)
        do {
            try updater.start()
            self.updater = updater
            applyCheckInterval()
            watchCheckInterval()
            NSLog("SappleUpdateChecker: checking %@ %@ (build %@), every %.0f s, automatic %@",
                  app.bundlePath, app.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?",
                  app.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?",
                  updater.updateCheckInterval, updater.automaticallyChecksForUpdates ? "yes" : "no")
        } catch {
            NSLog("SappleUpdateChecker: Sparkle did not start: %@", "\(error)")
        }
    }

    // MARK: How often (the app's "How often to check for updates" picker)

    /// The app's interval, from the app group, on this updater: Sparkle stores it and reschedules the next check
    /// (from the last one), so a change in the app takes effect here at once, no restart or logout needed.
    private func applyCheckInterval() {
        guard let updater, let value = UserDefaults(suiteName: Self.appGroup)?.object(forKey: Self.checkIntervalKey) as? Double,
              value >= 3600, abs(value - updater.updateCheckInterval) > 0.5 else { return }
        updater.updateCheckInterval = value
        updater.resetUpdateCycleAfterShortDelay()
        NSLog("SappleUpdateChecker: check interval now %.0f s", value)
    }

    /// The app posts checkIntervalChanged after writing the new value (key-value observing of the app group's
    /// defaults does not report another process's change); Sparkle's scheduling and the hourly look in
    /// watchForReplacement() read it again as fallbacks.
    private func watchCheckInterval() {
        CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), Unmanaged.passUnretained(self).toOpaque(),
                                        { _, observer, _, _, _ in
            guard let observer else { return }
            let me = Unmanaged<UpdateChecker>.fromOpaque(observer).takeUnretainedValue()
            DispatchQueue.main.async { me.applyCheckInterval() }
        }, Self.checkIntervalChanged as CFString, nil, .deliverImmediately)
    }

    // MARK: After an update

    private var watcher: DispatchSourceFileSystemObject?
    private var hourly: Timer?
    private let startedAs = Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String

    /// An update replaces the whole app, this helper included, while it runs: the process left is the old one. It
    /// watches the app's Info.plist (replaced = deleted or renamed; no polling) and, as a fallback, looks once an
    /// hour; when the app on disk is another build, it exits (unsuccessfully, on purpose) and launchd starts the new
    /// one. Cheap: one file descriptor and one timer.
    private func watchForReplacement() {
        let plist = Self.mainAppURL.appendingPathComponent("Contents/Info.plist")
        let fd = open(plist.path, O_EVTONLY)
        if fd >= 0 {
            let src = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.delete, .rename, .revoke, .write],
                                                                queue: .main)
            src.setEventHandler { [weak self] in
                // give the installer a moment to put the new app in place, then compare
                DispatchQueue.main.asyncAfter(deadline: .now() + 5) { self?.exitIfReplaced() }
            }
            src.setCancelHandler { close(fd) }
            src.resume()
            watcher = src
        }
        hourly = Timer.scheduledTimer(withTimeInterval: 3600, repeats: true) { [weak self] _ in
            self?.exitIfReplaced()
            self?.applyCheckInterval()
        }
        hourly?.tolerance = 600
    }

    private func exitIfReplaced() {
        let plist = Self.mainAppURL.appendingPathComponent("Contents/Info.plist")
        guard let data = try? Data(contentsOf: plist),
              let info = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any] else {
            DispatchQueue.main.asyncAfter(deadline: .now() + 10) { [weak self] in self?.exitIfReplaced() }   // mid-install
            return
        }
        let onDisk = info["CFBundleVersion"] as? String
        guard onDisk != startedAs else { return }
        NSLog("SappleUpdateChecker: the app is now build %@ (this checker is %@); exiting so the new one starts",
              onDisk ?? "?", startedAs ?? "?")
        // not 0: launchd restarts the login item only after an unsuccessful exit (its job: "successful exit => 0")
        exit(75)   // EX_TEMPFAIL
    }

    // MARK: SPUUpdaterDelegate

#if CV_SPARKLE_TEST
    func feedURLString(for updater: SPUUpdater) -> String? { "http://127.0.0.1:8766/appcast.xml" }
#endif

    /// Not while the app is open: it checks on its own and shows its own window.
    func updater(_ updater: SPUUpdater, mayPerform updateCheck: SPUUpdateCheck) throws {
        if !NSRunningApplication.runningApplications(withBundleIdentifier: Self.mainAppBundleID).isEmpty {
            throw NSError(domain: "SappleUpdateChecker", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "Sapple is open and checks for itself."])
        }
    }

    // MARK: SPUUserDriver: an update found is a notification; everything else is quiet

    func show(_ request: SPUUpdatePermissionRequest, reply: @escaping (SUUpdatePermissionResponse) -> Void) {
        // the app asks (or Info.plist decides); the helper never does
        reply(SUUpdatePermissionResponse(automaticUpdateChecks: true, sendSystemProfile: false))
    }

    func showUserInitiatedUpdateCheck(cancellation: @escaping () -> Void) {}

    func showUpdateFound(with appcastItem: SUAppcastItem, state: SPUUserUpdateState, reply: @escaping (SPUUserUpdateChoice) -> Void) {
        let skipped = UserDefaults(suiteName: Self.appGroup)?.string(forKey: Self.skippedBuildKey)
        if skipped != appcastItem.versionString {
            notify(version: appcastItem.displayVersionString, build: appcastItem.versionString)
        } else {
            NSLog("SappleUpdateChecker: build %@ was skipped in the app; not announced", appcastItem.versionString)
        }
        reply(.dismiss)   // later: the next scheduled check reminds again; nothing is downloaded
    }

    func showUpdateReleaseNotes(with downloadData: SPUDownloadData) {}
    func showUpdateReleaseNotesFailedToDownloadWithError(_ error: Error) {}
    func showUpdateNotFoundWithError(_ error: Error, acknowledgement: @escaping () -> Void) { acknowledgement() }
    func showUpdaterError(_ error: Error, acknowledgement: @escaping () -> Void) {
        NSLog("SappleUpdateChecker: check failed: %@", error.localizedDescription)
        acknowledgement()
    }
    func showDownloadInitiated(cancellation: @escaping () -> Void) { cancellation() }
    func showDownloadDidReceiveExpectedContentLength(_ expectedContentLength: UInt64) {}
    func showDownloadDidReceiveData(ofLength length: UInt64) {}
    func showDownloadDidStartExtractingUpdate() {}
    func showExtractionReceivedProgress(_ progress: Double) {}
    func showReady(toInstallAndRelaunch reply: @escaping (SPUUserUpdateChoice) -> Void) { reply(.dismiss) }
    func showInstallingUpdate(withApplicationTerminated applicationTerminated: Bool, retryTerminatingApplication: @escaping () -> Void) {}
    func showUpdateInstalledAndRelaunched(_ relaunched: Bool, acknowledgement: @escaping () -> Void) { acknowledgement() }
    func dismissUpdateInstallation() {}

    // MARK: The notification

    private func notify(version: String, build: String) {
        center.requestAuthorization(options: [.alert, .sound]) { granted, error in
            guard granted else {
                NSLog("SappleUpdateChecker: %@ found, but notifications are not allowed%@", version,
                      error.map { " (\($0.localizedDescription))" } ?? "")
                return
            }
            let content = UNMutableNotificationContent()
            content.title = "Sapple \(version) is available"
            content.body = "Open this to see what's new and install it."
            content.sound = .default
            // one per build: a reminder of the same update replaces the earlier one
            let request = UNNotificationRequest(identifier: "update-\(build)", content: content, trigger: nil)
            self.center.add(request) { error in
                if let error { NSLog("SappleUpdateChecker: notification failed: %@", error.localizedDescription) }
                else { NSLog("SappleUpdateChecker: announced %@ (build %@)", version, build) }
            }
        }
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
                                withCompletionHandler completionHandler: @escaping () -> Void) {
        // THIS copy of the app (the one around the helper), not whichever copy Launch Services would pick for the URL:
        // the update is for the app it checked
        let config = NSWorkspace.OpenConfiguration()
        config.activates = true
        NSWorkspace.shared.open([Self.showUpdateURL], withApplicationAt: Self.mainAppURL, configuration: config) { _, error in
            if let error { NSLog("SappleUpdateChecker: could not open the app: %@", "\(error)") }
        }
        completionHandler()
    }

    func userNotificationCenter(_ center: UNUserNotificationCenter, willPresent notification: UNNotification,
                                withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void) {
        completionHandler([.banner, .list, .sound])
    }
}
