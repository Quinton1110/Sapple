import SwiftUI

@main
struct ClassicVoicesApp: App {
#if os(macOS)
    // Sparkle updates (App/Updates.swift): started at launch; opens Sparkle's window from the helper's notification.
    @NSApplicationDelegateAdaptor(UpdatesAppDelegate.self) private var updatesDelegate
#endif

    var body: some Scene {
        WindowGroup {
#if os(macOS)
            ContentView().opensUpdatesFromNotification()
#else
            ContentView()
#endif
        }
#if os(macOS)
        .commands {
            CommandGroup(after: .appInfo) {
                CheckForUpdatesCommand(updates: AppUpdates.shared)
                UpdatesMenu(updates: AppUpdates.shared)
            }
        }
#endif
    }
}
