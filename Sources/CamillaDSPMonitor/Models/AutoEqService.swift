import Foundation

struct AutoEqHeadphone: Identifiable, Sendable, Hashable, Codable {
  let id: String
  let name: String
  let path: String

  var downloadPath: String {
    path.replacingOccurrences(of: " ", with: "%20")
  }
}

final actor AutoEqService: Sendable {
  static let shared = AutoEqService()

  private var allHeadphones: [AutoEqHeadphone] = []
  private var isLoaded = false

  private let cacheKey = "AutoEqHeadphonesCache"

  func fetchAllHeadphones(forceRefresh: Bool = false) async throws -> [AutoEqHeadphone] {
    if !forceRefresh && isLoaded { return allHeadphones }

    if !forceRefresh, let cached = loadHeadphonesFromCache() {
      allHeadphones = cached
      isLoaded = true
      return allHeadphones
    }

    let url = URL(
      string: "https://api.github.com/repos/jaakkopasanen/AutoEq/git/trees/master?recursive=1")!
    var request = URLRequest(url: url)
    request.setValue("CamillaDSP-Monitor", forHTTPHeaderField: "User-Agent")

    let (data, _) = try await URLSession.shared.data(for: request)
    let root = try JSONDecoder().decode(GitHubTree.self, from: data)

    allHeadphones = root.tree.compactMap { item in
      guard item.path.hasPrefix("results/"),
        item.path.hasSuffix(" ParametricEQ.txt"),
        let fileName = item.path.components(separatedBy: "/").last
      else { return nil }

      let headphoneName = fileName.replacingOccurrences(of: " ParametricEQ.txt", with: "")
      return AutoEqHeadphone(id: item.sha, name: headphoneName, path: item.path)
    }

    saveHeadphonesToCache(allHeadphones)
    isLoaded = true
    return allHeadphones
  }

  func fetchParametricEQ(for headphone: AutoEqHeadphone) async throws -> String {
    let baseUrl = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/"
    guard let url = URL(string: baseUrl + headphone.downloadPath) else {
      throw NSError(
        domain: "AutoEqService", code: 1, userInfo: [NSLocalizedDescriptionKey: "Invalid URL"])
    }

    let (data, _) = try await URLSession.shared.data(from: url)
    guard let text = String(data: data, encoding: .utf8) else {
      throw NSError(
        domain: "AutoEqService", code: 2,
        userInfo: [NSLocalizedDescriptionKey: "Failed to decode text"])
    }

    return text
  }

  private func loadHeadphonesFromCache() -> [AutoEqHeadphone]? {
    guard let data = UserDefaults.standard.data(forKey: cacheKey) else { return nil }
    return try? JSONDecoder().decode([AutoEqHeadphone].self, from: data)
  }

  private func saveHeadphonesToCache(_ headphones: [AutoEqHeadphone]) {
    if let data = try? JSONEncoder().encode(headphones) {
      UserDefaults.standard.set(data, forKey: cacheKey)
    }
  }
}

struct GitHubTree: Codable {
  let tree: [GitHubFile]
}

struct GitHubFile: Codable {
  let path: String
  let sha: String
}
