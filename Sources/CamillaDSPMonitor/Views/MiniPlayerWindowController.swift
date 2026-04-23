// MiniPlayerWindowController - NSPanel-based floating window controller

import AppKit
import Observation
import SwiftUI

@MainActor
final class MiniPlayerWindowController {
  static let shared = MiniPlayerWindowController()

  private var panel: NSPanel?
  private var mainWindow: NSWindow?
  private weak var appState: AppState?

  func showMiniPlayer(appState: AppState) {
    self.appState = appState

    mainWindow =
      NSApplication.shared.mainWindow
      ?? NSApplication.shared.windows.first {
        $0.contentViewController != nil && !$0.isMiniaturized
      }

    // Suppress hidden window re-renders before hiding it
    appState.isMiniPlayerActive = true
    mainWindow?.orderOut(nil)

    if let existing = panel {
      existing.orderFront(nil)
      return
    }

    let miniView = MiniPlayerView()
      .environment(appState)
      .environment(appState.levels)
      .environment(appState.settings)
      .environment(appState.devices)
      .environment(appState.pipeline)
      .environment(appState.monitoring)
      .environment(appState.dsp)
      .environment(appState.spectrum)
      .environment(appState.vuSettings)  // Inject persistent VU settings

    let hostingView = NSHostingView(rootView: miniView)
    hostingView.frame = NSRect(x: 0, y: 0, width: 320, height: 90)

    let panel = NSPanel(
      contentRect: NSRect(x: 0, y: 0, width: 320, height: 90),
      styleMask: [.hudWindow, .utilityWindow, .resizable, .nonactivatingPanel],
      backing: .buffered,
      defer: false
    )
    panel.contentView = hostingView
    panel.isOpaque = false
    panel.backgroundColor = .clear
    panel.hasShadow = true
    // screenSaver level is the highest and floats over full screen
    panel.level = .screenSaver
    // canJoinAllSpaces: stay visible when switching spaces
    // fullScreenAuxiliary: allows floating over full-screen apps
    // ignoresCycle: doesn't participate in Cmd-backtick cycling
    panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
    panel.isMovableByWindowBackground = true
    panel.hidesOnDeactivate = false
    panel.becomesKeyOnlyIfNeeded = true
    panel.titlebarAppearsTransparent = true
    panel.titleVisibility = .hidden
    panel.isFloatingPanel = true
    panel.minSize = NSSize(width: 200, height: 80)
    panel.maxSize = NSSize(width: 600, height: 120)

    if let screen = NSScreen.main {
      let screenFrame = screen.visibleFrame
      let x = screenFrame.maxX - 330
      let y = screenFrame.minY + 10
      panel.setFrameOrigin(NSPoint(x: x, y: y))
    }

    panel.orderFront(nil)
    self.panel = panel
  }

  func closeMiniPlayer() {
    panel?.orderOut(nil)
    panel = nil

    // Re-enable main window re-renders before showing it so SwiftUI can rebuild the
    // view hierarchy while the window is still off-screen.
    appState?.isMiniPlayerActive = false

    let windowToRestore = mainWindow
    mainWindow = nil
    DispatchQueue.main.async {
      windowToRestore?.makeKeyAndOrderFront(nil)
      NSApplication.shared.activate(ignoringOtherApps: true)
    }
  }

  var isMiniPlayerVisible: Bool {
    panel?.isVisible ?? false
  }
}
