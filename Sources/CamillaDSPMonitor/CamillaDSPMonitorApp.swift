import AppKit
import CamillaDSPLib
import SwiftUI

@main
struct CamillaDSPMonitorApp: App {
  @StateObject private var appState = AppState()
  @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

  var body: some Scene {
    WindowGroup {
      ContentView()
        .environmentObject(appState)
        .environmentObject(appState.meters)
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
    let task = Process()
    task.launchPath = "/usr/bin/env"
    task.arguments = ["pkill", "camilladsp"]
    try? task.run()
    task.waitUntilExit()
  }
}
