import AppKit
import SwiftUI

@main
struct DSPMonitorApp: App {
  @State private var appState = AppState()
  @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

  var body: some Scene {
    Window("DSPMonitor", id: "main") {
      ContentView()
        .environment(appState)
        .environment(appState.dsp)
        .environment(appState.settings)
        .environment(appState.devices)
        .environment(appState.pipeline)
        .environment(appState.monitoring)
        .frame(minWidth: 960, minHeight: 680)
        .onAppear {
          appDelegate.appState = appState
        }
    }
    .windowStyle(.titleBar)
    .defaultSize(width: 1100, height: 780)

    Settings {
      GeneralSettingsView()
        .environment(appState.settings)
        .environment(appState.monitoring)
        .frame(width: 450, height: 320)
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

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
    return false
  }

  func applicationWillTerminate(_ notification: Notification) {
    print("[AppDelegate] applicationWillTerminate")
    appState?.savePipelineStages()
    appState?.saveEQPresets()
  }

}
