import DSPAudio
import DSPConfig
import DSPEngine
import DSPLogging
import DSPPipeline
import DSPServer
import Foundation

struct FaderState: Codable {
  var volume: Float
  var mute: Bool
}

struct StateFileContent: Codable {
  var config_path: String?
  var volume: Float
  var mute: Bool
  var faders: [FaderState]
}

struct DSPCLI {
  static func printUsage() {
    print(
      """
      Usage: dsp-cli [CONFIGFILE] [OPTIONS]
        CONFIGFILE        Path to JSON/YAML configuration file.

      Options:
        -c, --check       Check config file and exit.
        -s, --statefile   Use the given file to persist volume/mute state.
        -w, --wait        Wait for config from websocket (starts inactive).
        --no_config       Ignore config file in statefile and start without.
        -p, --port        Port for the WebSocket control server.
        -a, --address     IP address to bind WebSocket server to (defaults to 127.0.0.1).
        -l, --loglevel    Log level (trace, debug, info, warn, error). Defaults to info.
        -o, --logfile     Write logs to the given file path.
        -g, --gain        Initial gain in dB for main volume control.
        --gain1           Initial gain in dB for Aux1 fader.
        --gain2           Initial gain in dB for Aux2 fader.
        --gain3           Initial gain in dB for Aux3 fader.
        --gain4           Initial gain in dB for Aux4 fader.
        -m, --mute        Start with main volume control muted.
        --mute1           Start with Aux1 fader muted.
        --mute2           Start with Aux2 fader muted.
        --mute3           Start with Aux3 fader muted.
        --mute4           Start with Aux4 fader muted.
        -r, --samplerate  Override samplerate in config.
        -n, --channels    Override number of channels of capture device in config.
      """)
  }

  static func faderForIndex(_ idx: Int) -> Fader? {
    switch idx {
    case 0: return .main
    case 1: return .aux1
    case 2: return .aux2
    case 3: return .aux3
    case 4: return .aux4
    default: return nil
    }
  }

  static func main() async {
    let arguments = CommandLine.arguments

    var configPath: String?
    var stateFilePath: String?
    var checkOnly = false
    var port: UInt16?
    var bindAddress = "127.0.0.1"
    var waitConfig = false
    var noConfig = false
    var logLevel = "info"
    var initialGains: [Fader: Double] = [:]
    var initialMutes: [Fader: Bool] = [:]
    var samplerateOverride: Int?
    var channelsOverride: Int?

    var i = 1
    while i < arguments.count {
      let arg = arguments[i]
      switch arg {
      case "-c", "--check":
        checkOnly = true
        i += 1
      case "-w", "--wait":
        waitConfig = true
        i += 1
      case "--no_config":
        noConfig = true
        i += 1
      case "-s", "--statefile":
        if i + 1 < arguments.count {
          stateFilePath = arguments[i + 1]
          i += 2
        } else {
          print("Error: Missing value for \(arg)")
          return
        }
      case "-p", "--port":
        if i + 1 < arguments.count, let p = UInt16(arguments[i + 1]) {
          port = p
          i += 2
        } else {
          print("Error: Invalid port for \(arg)")
          return
        }
      case "-a", "--address":
        if i + 1 < arguments.count {
          bindAddress = arguments[i + 1]
          i += 2
        } else {
          print("Error: Missing value for \(arg)")
          return
        }
      case "-l", "--loglevel":
        if i + 1 < arguments.count {
          logLevel = arguments[i + 1]
          i += 2
        } else {
          print("Error: Missing value for \(arg)")
          return
        }
      case "-o", "--logfile":
        if i + 1 < arguments.count {
          let path = arguments[i + 1]
          print(
            "Note: Native file logging is not supported. Please redirect stdout/stderr instead: > \(path) 2>&1"
          )
          i += 2
        } else {
          print("Error: Missing value for \(arg)")
          return
        }
      case "-g", "--gain":
        if i + 1 < arguments.count, let val = Double(arguments[i + 1]) {
          initialGains[.main] = val
          i += 2
        } else {
          print("Error: Invalid gain value")
          return
        }
      case "--gain1":
        if i + 1 < arguments.count, let val = Double(arguments[i + 1]) {
          initialGains[.aux1] = val
          i += 2
        } else {
          print("Error: Invalid gain1 value")
          return
        }
      case "--gain2":
        if i + 1 < arguments.count, let val = Double(arguments[i + 1]) {
          initialGains[.aux2] = val
          i += 2
        } else {
          print("Error: Invalid gain2 value")
          return
        }
      case "--gain3":
        if i + 1 < arguments.count, let val = Double(arguments[i + 1]) {
          initialGains[.aux3] = val
          i += 2
        } else {
          print("Error: Invalid gain3 value")
          return
        }
      case "--gain4":
        if i + 1 < arguments.count, let val = Double(arguments[i + 1]) {
          initialGains[.aux4] = val
          i += 2
        } else {
          print("Error: Invalid gain4 value")
          return
        }
      case "-m", "--mute":
        initialMutes[.main] = true
        i += 1
      case "--mute1":
        initialMutes[.aux1] = true
        i += 1
      case "--mute2":
        initialMutes[.aux2] = true
        i += 1
      case "--mute3":
        initialMutes[.aux3] = true
        i += 1
      case "--mute4":
        initialMutes[.aux4] = true
        i += 1
      case "-r", "--samplerate":
        if i + 1 < arguments.count, let val = Int(arguments[i + 1]) {
          samplerateOverride = val
          i += 2
        } else {
          print("Error: Invalid samplerate value")
          return
        }
      case "-n", "--channels":
        if i + 1 < arguments.count, let val = Int(arguments[i + 1]) {
          channelsOverride = val
          i += 2
        } else {
          print("Error: Invalid channels value")
          return
        }
      default:
        if !arg.hasPrefix("-") {
          configPath = arg
          i += 1
        } else {
          print("Unknown option: \(arg)")
          printUsage()
          return
        }
      }
    }

    let level: LogLevel
    switch logLevel.lowercased() {
    case "debug": level = .debug
    case "info": level = .info
    case "warn", "warning": level = .warn
    case "error": level = .error
    default: level = .info
    }
    MutableLogLevel.current = level

    var stateFile: StateFileContent?
    if let sPath = stateFilePath {
      let sUrl = URL(fileURLWithPath: sPath)
      if let sData = try? Data(contentsOf: sUrl),
        let s = try? JSONDecoder().decode(StateFileContent.self, from: sData)
      {
        stateFile = s
        if configPath == nil && !noConfig {
          configPath = s.config_path
        }
      }
    }

    if checkOnly {
      guard let path = configPath else {
        print("Error: Missing config file to check.")
        return
      }
      let url = URL(fileURLWithPath: path)
      do {
        let data = try Data(contentsOf: url)
        _ = try ConfigLoader.parse(json: String(data: data, encoding: .utf8) ?? "")
        print("Configuration is valid.")
        exit(0)
      } catch {
        print("Configuration check failed: \(error)")
        exit(1)
      }
    }

    var config: DSPConfiguration?
    if !waitConfig {
      guard let path = configPath else {
        print("Error: Missing required configuration file.")
        printUsage()
        return
      }
      let url = URL(fileURLWithPath: path)
      do {
        let data = try Data(contentsOf: url)
        var parsed = try ConfigLoader.parse(json: String(data: data, encoding: .utf8) ?? "")
        if let sr = samplerateOverride {
          parsed.devices.samplerate = sr
        }
        if let ch = channelsOverride {
          parsed.devices.capture.channels = ch
        }
        config = parsed
      } catch {
        print("Failed to load configuration: \(error)")
        return
      }
    }

    let engine = SwiftDSPEngine()
    if let config = config {
      do {
        let data = try JSONEncoder().encode(config)
        let jsonStr = String(data: data, encoding: .utf8) ?? "{}"
        try await engine.setConfig(json: jsonStr)
        print("Engine started successfully.")
      } catch {
        print("Error starting engine: \(error)")
        return
      }
    } else {
      print("Starting engine in inactive state (waiting for websocket configuration)...")
    }

    // Set initial gains & mutes
    let params = await engine.getProcessingParameters()
    if let params = params {
      for (fader, gain) in initialGains {
        params.setTargetVolume(PrcFmt(gain), for: fader)
      }
      for (fader, mute) in initialMutes {
        params.setMuted(mute, for: fader)
      }
    }

    // Load state from state file if present
    if let sFile = stateFile {
      if let params = params {
        params.setTargetVolume(PrcFmt(sFile.volume), for: .main)
        params.setMuted(sFile.mute, for: .main)
        for (idx, fState) in sFile.faders.enumerated() {
          if let fader = faderForIndex(idx + 1) {
            params.setTargetVolume(PrcFmt(fState.volume), for: fader)
            params.setMuted(fState.mute, for: fader)
          }
        }
      }
    }

    let activeConfigPath = ActiveConfigPath(initialPath: configPath)

    // Start state saver task loop
    if let sPath = stateFilePath {
      Task {
        while true {
          try? await Task.sleep(nanoseconds: 1_000_000_000)  // 1s
          if let params = await engine.getProcessingParameters() {
            var fadersList: [FaderState] = []
            for f in [Fader.aux1, .aux2, .aux3, .aux4] {
              fadersList.append(
                FaderState(
                  volume: Float(params.targetVolume(for: f)),
                  mute: params.isMuted(for: f)
                ))
            }
            let activePath = activeConfigPath.value
            let content = StateFileContent(
              config_path: activePath,
              volume: Float(params.targetVolume(for: .main)),
              mute: params.isMuted(for: .main),
              faders: fadersList
            )
            if let data = try? JSONEncoder().encode(content) {
              try? data.write(to: URL(fileURLWithPath: sPath))
            }
          }
        }
      }
    }

    // Start WebSocket server
    if let p = port {
      let s = WebSocketServer(port: p, host: bindAddress, activePath: activeConfigPath)
      s.setEngine(engine)
      do {
        try s.start()
        print("WebSocket server running on \(bindAddress):\(p)")
      } catch {
        print("Error starting WebSocket server: \(error)")
      }
    }

    print("Press Ctrl+C to stop.")
    while true {
      try? await Task.sleep(nanoseconds: 3_600_000_000_000)  // 1 hour
    }
  }
}

await DSPCLI.main()
