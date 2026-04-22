import AppKit
import CamillaDSPLib
import SwiftUI

@main
struct CamillaDSPMonitorApp: App {
  @State private var appState = AppState()
  @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

  var body: some Scene {
    Window("CamillaDSP Monitor", id: "main") {
      ContentView()
        .environment(appState)
        .environment(appState.levels)
        .environment(appState.dsp)
        .environment(appState.settings)
        .environment(appState.devices)
        .environment(appState.pipeline)
        .environment(appState.monitoring)
        .environment(appState.spectrum)
        .environment(appState.vuSettings)  // Inject persistent VU settings
        .frame(minWidth: 960, minHeight: 680)
        .onAppear {
          appDelegate.appState = appState
          setupWindowIntercept()
        }
    }
    .windowStyle(.titleBar)
    .defaultSize(width: 1100, height: 780)

    Settings {
      DevicePickerView()
        .environment(appState.devices)
        .environment(appState.settings)
        .frame(width: 450, height: 350)
    }
  }

  /// Directly overrides the minimize button action to enter PiP mode.
  private func setupWindowIntercept() {
    // Slight delay to ensure the NSWindow and its titlebar buttons are ready.
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
      guard
        let window = NSApplication.shared.windows.first(where: { $0.identifier?.rawValue == "main" }
        )
      else {
        return
      }

      if let minimizeButton = window.standardWindowButton(.miniaturizeButton) {
        minimizeButton.target = appDelegate
        minimizeButton.action = #selector(AppDelegate.customMinimizeAction(_:))
      }
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

    for sig in [SIGINT, SIGTERM] {
      signal(sig) { _ in
        DSPEngine.killStaleCamillaDSP()
        _exit(0)
      }
    }
  }

  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
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
    DSPEngine.killStaleCamillaDSP()
  }

  // MARK: - Custom Actions

  @objc func customMinimizeAction(_: Any?) {
    if let appState = appState {
      MiniPlayerWindowController.shared.showMiniPlayer(appState: appState)
    }
  }
}
