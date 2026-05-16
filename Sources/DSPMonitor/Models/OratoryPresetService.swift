import Foundation

struct OratoryHeadphone: Identifiable, Sendable, Hashable, Codable {
  let id: String
  let name: String
  let path: String

  var downloadPath: String {
    path.replacingOccurrences(of: " ", with: "%20")
  }
}

final actor OratoryPresetService: Sendable {
  static let shared = OratoryPresetService()

  private var allHeadphones: [OratoryHeadphone] = []
  private var isLoaded = false

  private let cacheKey = "OratoryHeadphonesCache"

  func fetchAllHeadphones(forceRefresh: Bool = false) async throws -> [OratoryHeadphone] {
    if !forceRefresh && isLoaded { return allHeadphones }

    if !forceRefresh, let cached = loadHeadphonesFromCache() {
      allHeadphones = cached
      isLoaded = true
      return allHeadphones
    }

    // Directly fetch the tree for results/oratory1990 to ensure perfect isolation
    let url = URL(
      string:
        "https://api.github.com/repos/jaakkopasanen/AutoEq/git/trees/master:results/oratory1990?recursive=1"
    )!
    var request = URLRequest(url: url)
    request.setValue("DSPMonitor", forHTTPHeaderField: "User-Agent")

    let (data, _) = try await URLSession.shared.data(for: request)
    let root = try JSONDecoder().decode(GitHubTree.self, from: data)

    allHeadphones = root.tree.compactMap { item in
      guard item.path.hasSuffix(" ParametricEQ.txt"),
        let fileName = item.path.components(separatedBy: "/").last
      else { return nil }

      let headphoneName = fileName.replacingOccurrences(of: " ParametricEQ.txt", with: "")
      return OratoryHeadphone(id: item.sha, name: headphoneName, path: item.path)
    }

    saveHeadphonesToCache(allHeadphones)
    isLoaded = true
    return allHeadphones
  }

  func fetchParametricEQ(for headphone: OratoryHeadphone) async throws -> String {
    let baseUrl =
      "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/oratory1990/"
    guard let url = URL(string: baseUrl + headphone.downloadPath) else {
      throw NSError(
        domain: "OratoryPresetService", code: 1,
        userInfo: [NSLocalizedDescriptionKey: "Invalid URL"])
    }

    let (data, _) = try await URLSession.shared.data(from: url)
    guard let text = String(data: data, encoding: .utf8) else {
      throw NSError(
        domain: "OratoryPresetService", code: 2,
        userInfo: [NSLocalizedDescriptionKey: "Failed to decode text"])
    }

    return text
  }

  private func loadHeadphonesFromCache() -> [OratoryHeadphone]? {
    guard let data = UserDefaults.standard.data(forKey: cacheKey) else { return nil }
    return try? JSONDecoder().decode([OratoryHeadphone].self, from: data)
  }

  private func saveHeadphonesToCache(_ headphones: [OratoryHeadphone]) {
    if let data = try? JSONEncoder().encode(headphones) {
      UserDefaults.standard.set(data, forKey: cacheKey)
    }
  }
}
