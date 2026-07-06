// JSON loader for `DSPConfiguration`.
//
// All configuration validation logic (top-level schema bounds, per-component
// constraints, and pipeline channel verification) resides inside the model
// definitions in the `DSPConfig` package. This file is responsible only
// for decoding the JSON representation of the configuration.
//
// DSPMonitor only ever sends JSON over the actor's
// `start(configJson:)` boundary, so the loader is JSON-only; the
// YAML pathway and Yams dependency have been pruned.

import DSPConfig
import Foundation

public enum ConfigLoader {
  private static let logger = Logger(label: "dsp.config")

  /// Parse a DSP configuration from JSON and run full validation.
  public static func parse(json: String) throws -> DSPConfiguration {
    guard let data = json.data(using: .utf8) else {
      throw ConfigError.parseError("JSON config is not valid UTF-8")
    }
    let config: DSPConfiguration
    do {
      config = try JSONDecoder().decode(DSPConfiguration.self, from: data)
    } catch let error as DecodingError {
      throw ConfigError.parseError("\(error)")
    }
    try config.validate()
    logger.info("Configuration validated successfully")
    return config
  }
}
