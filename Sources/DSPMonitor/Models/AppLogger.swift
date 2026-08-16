import DSPConfig
import Foundation

public enum AppLogger {
  public typealias Handler = @Sendable (_ level: LogLevel, _ component: String, _ message: String) -> Void

  nonisolated(unsafe) private static var customHandler: Handler?
  private static let lock = NSLock()

  public static func setHandler(_ handler: Handler?) {
    lock.lock()
    customHandler = handler
    lock.unlock()
  }

  public static func log(_ level: LogLevel, component: String, _ message: String) {
    lock.lock()
    let h = customHandler
    lock.unlock()
    h?(level, component, message)
  }

  public static func info(_ component: String, _ message: String) {
    log(.info, component: component, message)
  }

  public static func warn(_ component: String, _ message: String) {
    log(.warn, component: component, message)
  }

  public static func error(_ component: String, _ message: String) {
    log(.error, component: component, message)
  }

  public static func debug(_ component: String, _ message: String) {
    log(.debug, component: component, message)
  }

  public static func trace(_ component: String, _ message: String) {
    log(.trace, component: component, message)
  }
}
