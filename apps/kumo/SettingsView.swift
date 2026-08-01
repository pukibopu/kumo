import Security
import SwiftUI

// Minimal Keychain wrapper (ADR 0044 slice G4): generic-password items keyed
// by service "dev.kumo.app" / account = the env-var name the value seeds
// (see KumoEngine.mm's applyKeychainEnv). No external dependency, just the
// Security framework's C API.
enum KeychainStore {
    private static let service = "dev.kumo.app"

    private static func query(account: String) -> [String: Any] {
        [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
    }

    static func read(account: String) -> String? {
        var attributes = query(account: account)
        attributes[kSecReturnData as String] = true
        attributes[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(attributes as CFDictionary, &item)
        guard status == errSecSuccess, let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func write(account: String, value: String) {
        let data = Data(value.utf8)
        let base = query(account: account)
        if SecItemCopyMatching(base as CFDictionary, nil) == errSecSuccess {
            SecItemUpdate(base as CFDictionary, [kSecValueData as String: data] as CFDictionary)
        } else {
            var newItem = base
            newItem[kSecValueData as String] = data
            SecItemAdd(newItem as CFDictionary, nil)
        }
    }

    static func delete(account: String) {
        SecItemDelete(query(account: account) as CFDictionary)
    }
}

// Settings scene (ADR 0044 slice G4): per-agent endpoint overrides plus a
// global provider section write straight into kumo.config.json (the same
// file the engine loads); API keys go to the Keychain only, never into that
// file. Saving hot-applies (Item D): the Keychain keys are re-seeded into the
// process env with overwrite, then KumoEngine.reloadAgentSessions() rebuilds
// both agent sessions from the fresh config/env — refused (unchanged) while a
// session is mid-turn, surfaced inline rather than retried automatically.
struct SettingsView: View {
    @ObservedObject var holder: EngineHolder

    @AppStorage("restoreSceneOnLaunch") private var restoreSceneOnLaunch = false

    @State private var globalType = "anthropic"
    @State private var globalBaseUrl = ""
    @State private var globalModel = ""
    @State private var maxTokens = "4096"
    @State private var requestTimeoutSeconds = "120"
    @State private var confirmDestructive = false
    @State private var summaryThresholdTokens = "8000"

    @State private var sceneType = ""
    @State private var sceneBaseUrl = ""
    @State private var sceneModel = ""

    @State private var shaderType = ""
    @State private var shaderBaseUrl = ""
    @State private var shaderModel = ""

    @State private var openAIKey = ""
    @State private var anthropicKey = ""

    @State private var loaded = false
    @State private var statusMessage: String?

    var body: some View {
        Group {
            if holder.engine == nil {
                Text("引擎启动中，稍后再打开设置。")
                    .foregroundStyle(.secondary)
                    .padding()
            } else {
                Form {
                    agentSection(title: "场景", type: $sceneType, baseUrl: $sceneBaseUrl,
                                model: $sceneModel)
                    agentSection(title: "Shader", type: $shaderType, baseUrl: $shaderBaseUrl,
                                model: $shaderModel)
                    globalSection
                    apiKeySection
                    Section("启动") {
                        Toggle("启动时恢复上次场景", isOn: $restoreSceneOnLaunch)
                    }
                    Section {
                        if let statusMessage {
                            Text(statusMessage)
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                        HStack {
                            Spacer()
                            Button("保存", action: save)
                        }
                    }
                }
                .formStyle(.grouped)
            }
        }
        .frame(width: 440, height: 560)
        .onAppear(perform: load)
    }

    @ViewBuilder
    private func agentSection(title: String, type: Binding<String>, baseUrl: Binding<String>,
                              model: Binding<String>) -> some View {
        Section(title) {
            Picker("协议", selection: type) {
                Text("继承全局").tag("")
                Text("Anthropic").tag("anthropic")
                Text("OpenAI").tag("openai")
            }
            TextField("Base URL", text: baseUrl)
            TextField("模型", text: model)
        }
    }

    private var globalSection: some View {
        Section("全局") {
            Picker("协议", selection: $globalType) {
                Text("Anthropic").tag("anthropic")
                Text("OpenAI").tag("openai")
            }
            TextField("Base URL", text: $globalBaseUrl)
            TextField("模型", text: $globalModel)
            TextField("Max Tokens", text: $maxTokens)
            TextField("请求超时（秒）", text: $requestTimeoutSeconds)
            Toggle("破坏性操作需确认", isOn: $confirmDestructive)
            TextField("历史摘要阈值（tokens）", text: $summaryThresholdTokens)
        }
    }

    private var apiKeySection: some View {
        Section("API Keys") {
            SecureField("OPENAI_API_KEY", text: $openAIKey)
            SecureField("ANTHROPIC_API_KEY", text: $anthropicKey)
            Text("密钥保存在系统钥匙串，不写入配置文件。")
                .font(.footnote)
                .foregroundStyle(.secondary)
        }
    }

    private func load() {
        guard !loaded, let engine = holder.engine else { return }
        loaded = true

        openAIKey = KeychainStore.read(account: "OPENAI_API_KEY") ?? ""
        anthropicKey = KeychainStore.read(account: "ANTHROPIC_API_KEY") ?? ""

        guard let data = FileManager.default.contents(atPath: engine.configPath()),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return
        }

        if let provider = json["provider"] as? [String: Any] {
            globalType = provider["type"] as? String ?? globalType
            globalBaseUrl = provider["base_url"] as? String ?? globalBaseUrl
            globalModel = provider["model"] as? String ?? globalModel
            if let value = provider["max_tokens"] { maxTokens = "\(value)" }
            if let value = provider["request_timeout_seconds"] {
                requestTimeoutSeconds = "\(value)"
            }
        }
        if let agents = json["agents"] as? [String: Any] {
            confirmDestructive = agents["confirm_destructive"] as? Bool ?? confirmDestructive
            if let value = agents["summary_threshold_tokens"] {
                summaryThresholdTokens = "\(value)"
            }
            if let scene = agents["scene"] as? [String: Any] {
                sceneType = scene["type"] as? String ?? sceneType
                sceneBaseUrl = scene["base_url"] as? String ?? sceneBaseUrl
                sceneModel = scene["model"] as? String ?? sceneModel
            }
            if let shader = agents["shader"] as? [String: Any] {
                shaderType = shader["type"] as? String ?? shaderType
                shaderBaseUrl = shader["base_url"] as? String ?? shaderBaseUrl
                shaderModel = shader["model"] as? String ?? shaderModel
            }
        }
    }

    // Reads the existing file (if any), overlays only the fields this form
    // owns, and writes back pretty-printed. api_key fields are never touched
    // here, whatever they already hold (empty or not) passes through as-is.
    private func save() {
        guard let engine = holder.engine else { return }

        if openAIKey.isEmpty {
            KeychainStore.delete(account: "OPENAI_API_KEY")
        } else {
            KeychainStore.write(account: "OPENAI_API_KEY", value: openAIKey)
        }
        if anthropicKey.isEmpty {
            KeychainStore.delete(account: "ANTHROPIC_API_KEY")
        } else {
            KeychainStore.write(account: "ANTHROPIC_API_KEY", value: anthropicKey)
        }

        let path = engine.configPath()
        var root: [String: Any] = [:]
        if let data = FileManager.default.contents(atPath: path),
           let existing = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            root = existing
        }

        var provider = root["provider"] as? [String: Any] ?? [:]
        provider["type"] = globalType
        provider["base_url"] = globalBaseUrl
        provider["model"] = globalModel
        provider["max_tokens"] = Int(maxTokens) ?? 4096
        provider["request_timeout_seconds"] = Int(requestTimeoutSeconds) ?? 120
        root["provider"] = provider

        var agents = root["agents"] as? [String: Any] ?? [:]
        agents["confirm_destructive"] = confirmDestructive
        agents["summary_threshold_tokens"] = Int(summaryThresholdTokens) ?? 8000

        var scene = agents["scene"] as? [String: Any] ?? [:]
        scene["type"] = sceneType
        scene["base_url"] = sceneBaseUrl
        scene["model"] = sceneModel
        agents["scene"] = scene

        var shader = agents["shader"] as? [String: Any] ?? [:]
        shader["type"] = shaderType
        shader["base_url"] = shaderBaseUrl
        shader["model"] = shaderModel
        agents["shader"] = shader

        root["agents"] = agents

        let options: JSONSerialization.WritingOptions = [.prettyPrinted, .sortedKeys]
        guard JSONSerialization.isValidJSONObject(root),
              let data = try? JSONSerialization.data(withJSONObject: root, options: options) else {
            statusMessage = "保存失败：无法序列化配置。"
            return
        }
        do {
            try data.write(to: URL(fileURLWithPath: path), options: .atomic)
        } catch {
            statusMessage = "保存失败：\(error.localizedDescription)"
            return
        }

        // Settings hot apply (Item D): reseeds the process env (overwrite,
        // unlike the launch-time seed) and rebuilds both agent sessions from
        // the config/env just written. False means a session is mid-turn; do
        // not retry automatically, the user saves again once it is done.
        if engine.reloadAgentSessions() {
            statusMessage = "已生效，会话已重置"
        } else {
            statusMessage = "会话进行中，完成后再保存生效"
        }
    }
}
