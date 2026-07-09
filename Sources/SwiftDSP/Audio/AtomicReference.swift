import Foundation
import Synchronization

/// A lock-free atomic cell that holds a reference to a Swift class object.
/// Uses `Unmanaged` and Swift 6's standard `Atomic` to bypass `AtomicRepresentable` limitations for class types.
public final class AtomicReference<T: AnyObject>: @unchecked Sendable {
  private let pointer = Atomic<UnsafeMutableRawPointer?>(nil)

  public init(_ initialValue: T? = nil) {
    if let val = initialValue {
      let ptr = Unmanaged.passRetained(val).toOpaque()
      pointer.store(ptr, ordering: .relaxed)
    }
  }

  /// Atomically stores a new reference with releasing ordering, releasing the old reference if one existed.
  public func store(_ newValue: T?) {
    let newPtr = newValue.map { Unmanaged.passRetained($0).toOpaque() }
    if let oldPtr = pointer.exchange(newPtr, ordering: .releasing) {
      Unmanaged<T>.fromOpaque(oldPtr).release()
    }
  }

  /// Atomically exchanges the current reference with a new one with acquiring ordering, returning the old reference.
  /// The caller takes ownership of the returned reference and is responsible for its lifecycle.
  public func exchange(_ newValue: T?) -> T? {
    let newPtr = newValue.map { Unmanaged.passRetained($0).toOpaque() }
    if let oldPtr = pointer.exchange(newPtr, ordering: .acquiring) {
      return Unmanaged<T>.fromOpaque(oldPtr).takeRetainedValue()
    }
    return nil
  }

  deinit {
    if let oldPtr = pointer.exchange(nil, ordering: .relaxed) {
      Unmanaged<T>.fromOpaque(oldPtr).release()
    }
  }
}
