import AppKit
import CamillaDSPLib
import SwiftUI

@main
struct CamillaDSPMonitorApp: App {
  @StateObject private var appState = AppState()
  @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

  var body: some Scene {
    Window("CamillaDSP Monitor", id: "main") {
      ContentView()
        .environmentObject(appState)
        .environmentObject(appState.levels)
        .environmentObject(appState.spectrum)
        .environmentObject(appState.load)
        .frame(minWidth: 960, minHeight: 680)
        .onAppear {
          appDelegate.appState = appState
        }
    }
    .windowStyle(.titleBar)
    .defaultSize(width: 1100, height: 780)

    Settings {
      DevicePickerView()
        .environmentObject(appState)
        .frame(width: 450, height: 350)
    }
  }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
  var appState: AppState?

  func applicationWillFinishLaunching(_ notification: Notification) {
    NSApplication.shared.setActivationPolicy(.regular)
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    NSApplication.shared.activate(ignoringOtherApps: true)

    // Kill camilladsp on SIGINT (ctrl+c) and SIGTERM (kill).
    // applicationWillTerminate only fires on graceful quit (cmd+q).
    for sig in [SIGINT, SIGTERM] {
      signal(sig) { _ in
        DSPEngine.killStaleCamillaDSP()
        _exit(0)
      }
    }
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    // Don't terminate when the main window is hidden for mini player mode.
    return false
  }

  func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool
  {
    if MiniPlayerWindowController.shared.isMiniPlayerVisible {
      MiniPlayerWindowController.shared.closeMiniPlayer()
      return false
    }
    return true
  }

  func applicationWillTerminate(_ notification: Notification) {
    print("[AppDelegate] applicationWillTerminate")
    appState?.savePipelineStages()
    appState?.saveEQPresets()
    appState?.stopMonitoring()

    // Kill camilladsp synchronously — can't await the actor during termination
    DSPEngine.killStaleCamillaDSP()
  }
}
