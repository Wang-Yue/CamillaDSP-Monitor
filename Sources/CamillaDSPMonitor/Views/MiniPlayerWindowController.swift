// MiniPlayerWindowController - NSPanel-based floating window controller

import AppKit
import SwiftUI

@MainActor
final class MiniPlayerWindowController {
  static let shared = MiniPlayerWindowController()

  private var panel: NSPanel?
  private var mainWindow: NSWindow?

  func showMiniPlayer(appState: AppState) {
    // Remember the main window
    mainWindow =
      NSApplication.shared.mainWindow
      ?? NSApplication.shared.windows.first {
        $0.contentViewController != nil && !$0.isMiniaturized
      }

    // Hide main window (don't minimize to dock)
    mainWindow?.orderOut(nil)

    if let existing = panel {
      existing.orderFront(nil)
      return
    }

    // Create the SwiftUI content
    let miniView = MiniPlayerView()
      .environmentObject(appState)
      .environmentObject(appState.meters)

    let hostingView = NSHostingView(rootView: miniView)
    hostingView.frame = NSRect(x: 0, y: 0, width: 320, height: 90)

    // Create NSPanel — floating, non-activating, visible on all spaces
    let panel = NSPanel(
      contentRect: NSRect(x: 0, y: 0, width: 320, height: 90),
      styleMask: [.nonactivatingPanel, .hudWindow, .utilityWindow, .resizable],
      backing: .buffered,
      defer: false
    )
    panel.contentView = hostingView
    panel.isOpaque = false
    panel.backgroundColor = .clear
    panel.hasShadow = true
    panel.level = .screenSaver  // Above fullscreen video
    panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
    panel.isMovableByWindowBackground = true
    panel.hidesOnDeactivate = false
    panel.titlebarAppearsTransparent = true
    panel.titleVisibility = .hidden
    panel.isFloatingPanel = true
    panel.minSize = NSSize(width: 200, height: 80)
    panel.maxSize = NSSize(width: 600, height: 120)

    // Position: lower-right corner of the screen
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

    // Restore main window
    if let main = mainWindow {
      main.makeKeyAndOrderFront(nil)
      NSApplication.shared.activate(ignoringOtherApps: true)
    }
    mainWindow = nil
  }

  var isMiniPlayerVisible: Bool {
    panel?.isVisible ?? false
  }
}
