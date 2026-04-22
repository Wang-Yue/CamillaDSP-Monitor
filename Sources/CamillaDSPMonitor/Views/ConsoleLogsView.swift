import AppKit
import Observation
import SwiftUI

struct ConsoleLogsView: View {
  @Environment(LogManager.self) var logManager
  @State private var autoScroll = true

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      HStack {
        Text("Console Logs")
          .font(.headline)
        Spacer()

        Button {
          let allLogs = logManager.entries.map { entry in
            "[\(entry.timestamp.description)] \(entry.message)"
          }.joined(separator: "\n")
          let pasteboard = NSPasteboard.general
          pasteboard.clearContents()
          pasteboard.setString(allLogs, forType: .string)
        } label: {
          Label("Copy", systemImage: "doc.on.doc")
        }
        .buttonStyle(.borderless)

        Button {
          logManager.entries.removeAll()
        } label: {
          Label("Clear", systemImage: "trash")
        }
        .buttonStyle(.borderless)

        Toggle("Auto-scroll", isOn: $autoScroll)
          .toggleStyle(.checkbox)
      }
      .padding()
      .background(Color(nsColor: .windowBackgroundColor))

      Divider()

      ScrollViewReader { proxy in
        List {
          ForEach(logManager.entries) { entry in
            HStack(alignment: .top, spacing: 8) {
              Text(entry.timestamp, style: .time)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 70, alignment: .leading)

              Text(entry.message)
                .font(.system(.body, design: .monospaced))
                .textSelection(.enabled)
            }
            .id(entry.id)
          }
        }
        .listStyle(.plain)
        .onChange(of: logManager.entries.count) { _, _ in
          if autoScroll, let last = logManager.entries.last {
            proxy.scrollTo(last.id, anchor: .bottom)
          }
        }
      }
    }
    .background(Color(nsColor: .controlBackgroundColor))
  }
}
