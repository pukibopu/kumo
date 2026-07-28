import AppKit
import SwiftUI

// Sidebar (ADR 0044): a flat list of every entity, name + id. Reloaded by the
// app-level timer/refresh hook; this view itself holds no engine state.
struct SceneTreeView: View {
    @ObservedObject var holder: EngineHolder
    let entities: [KumoEntityInfo]
    @Binding var selection: String?

    var body: some View {
        Group {
            if holder.engine == nil {
                Text("引擎启动中…")
                    .foregroundStyle(.secondary)
                    .padding()
            } else {
                List(entities, id: \.entityId, selection: $selection) { entity in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(entity.name.isEmpty ? entity.primitive : entity.name)
                        Text(entity.entityId)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
        .navigationTitle("场景")
    }
}

// Inspector (ADR 0044): transform/material editing plus a read-only shader
// viewer for the selected entity. Every committed edit records exactly one
// undo point via EngineRuntime::beginEdit before the corresponding
// setEntity*/clearEntityShader call.
struct InspectorView: View {
    @ObservedObject var holder: EngineHolder
    let entityId: String?
    let refreshToken: Int
    let onChanged: () -> Void

    @State private var detail: KumoEntityDetail?

    @State private var positionX = 0.0
    @State private var positionY = 0.0
    @State private var positionZ = 0.0
    @State private var eulerX = 0.0
    @State private var eulerY = 0.0
    @State private var eulerZ = 0.0
    @State private var scaleX = 1.0
    @State private var scaleY = 1.0
    @State private var scaleZ = 1.0

    @State private var baseColor = Color.white
    @State private var metallic = 0.0
    @State private var roughness = 1.0
    @State private var emissiveR = 0.0
    @State private var emissiveG = 0.0
    @State private var emissiveB = 0.0

    @State private var shaderSource = ""
    @State private var generatedPath: String?

    var body: some View {
        Group {
            if holder.engine == nil {
                Text("引擎启动中…")
                    .foregroundStyle(.secondary)
                    .padding()
            } else if let detail, detail.found {
                Form {
                    Section("实体") {
                        LabeledContent("名称", value: detail.name)
                        LabeledContent("ID", value: detail.entityId)
                        if !detail.primitive.isEmpty {
                            LabeledContent("类型", value: detail.primitive)
                        }
                    }
                    Section("变换") {
                        transformRow("位置", x: $positionX, y: $positionY, z: $positionZ)
                        transformRow("旋转", x: $eulerX, y: $eulerY, z: $eulerZ)
                        transformRow("缩放", x: $scaleX, y: $scaleY, z: $scaleZ)
                    }
                    if detail.hasMaterial {
                        materialSection
                    }
                    if detail.hasCustomShader {
                        shaderSection
                    }
                }
                .formStyle(.grouped)
            } else {
                Text("未选择实体")
                    .foregroundStyle(.secondary)
                    .padding()
            }
        }
        .frame(minWidth: 280, idealWidth: 300, maxWidth: 340)
        .onAppear { loadDetail() }
        .onChange(of: entityId) { _, _ in loadDetail() }
        .onChange(of: refreshToken) { _, _ in loadDetail() }
    }

    @ViewBuilder
    private var materialSection: some View {
        Section("材质") {
            ColorPicker("基础色", selection: $baseColor, supportsOpacity: true)
                .onChange(of: baseColor) { _, _ in commitMaterial(beginNewEdit: true) }
            LabeledContent("金属度") {
                Slider(value: $metallic, in: 0...1) { began in
                    if began { holder.engine?.beginEdit("inspector") }
                }
            }
            .onChange(of: metallic) { _, _ in commitMaterial(beginNewEdit: false) }
            LabeledContent("粗糙度") {
                Slider(value: $roughness, in: 0...1) { began in
                    if began { holder.engine?.beginEdit("inspector") }
                }
            }
            .onChange(of: roughness) { _, _ in commitMaterial(beginNewEdit: false) }
            HStack {
                Text("自发光")
                TextField("R", value: $emissiveR, format: .number)
                    .onSubmit { commitMaterial(beginNewEdit: true) }
                TextField("G", value: $emissiveG, format: .number)
                    .onSubmit { commitMaterial(beginNewEdit: true) }
                TextField("B", value: $emissiveB, format: .number)
                    .onSubmit { commitMaterial(beginNewEdit: true) }
            }
        }
    }

    @ViewBuilder
    private var shaderSection: some View {
        Section("Shader") {
            ScrollView {
                Text(shaderSource)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 220)
            HStack {
                Button("还原为标准 PBR") { clearShader() }
                Button("在访达中显示") { revealInFinder() }
                    .disabled(generatedPath == nil)
            }
        }
    }

    @ViewBuilder
    private func transformRow(_ label: String, x: Binding<Double>, y: Binding<Double>,
                              z: Binding<Double>) -> some View {
        HStack {
            Text(label).frame(width: 40, alignment: .leading)
            TextField("X", value: x, format: .number).onSubmit(commitTransform)
            TextField("Y", value: y, format: .number).onSubmit(commitTransform)
            TextField("Z", value: z, format: .number).onSubmit(commitTransform)
        }
    }

    private func loadDetail() {
        guard let engine = holder.engine, let entityId else {
            detail = nil
            return
        }
        let loaded = engine.entityDetail(entityId)
        detail = loaded
        guard loaded.found else { return }

        positionX = Double(loaded.positionX)
        positionY = Double(loaded.positionY)
        positionZ = Double(loaded.positionZ)
        eulerX = Double(loaded.eulerX)
        eulerY = Double(loaded.eulerY)
        eulerZ = Double(loaded.eulerZ)
        scaleX = Double(loaded.scaleX)
        scaleY = Double(loaded.scaleY)
        scaleZ = Double(loaded.scaleZ)

        if loaded.hasMaterial {
            baseColor = Color(red: Double(loaded.baseColorR), green: Double(loaded.baseColorG),
                              blue: Double(loaded.baseColorB), opacity: Double(loaded.baseColorA))
            metallic = Double(loaded.metallic)
            roughness = Double(loaded.roughness)
            emissiveR = Double(loaded.emissiveR)
            emissiveG = Double(loaded.emissiveG)
            emissiveB = Double(loaded.emissiveB)
        }

        if loaded.hasCustomShader {
            shaderSource = engine.entityShaderSource(entityId) ?? ""
            generatedPath = engine.generatedShaderPath(entityId)
        } else {
            shaderSource = ""
            generatedPath = nil
        }
    }

    private func commitTransform() {
        guard let engine = holder.engine, let entityId else { return }
        engine.beginEdit("inspector")
        _ = engine.setEntityTransform(entityId, px: Float(positionX), py: Float(positionY),
                                      pz: Float(positionZ), rx: Float(eulerX), ry: Float(eulerY),
                                      rz: Float(eulerZ), sx: Float(scaleX), sy: Float(scaleY),
                                      sz: Float(scaleZ))
        onChanged()
    }

    private func commitMaterial(beginNewEdit: Bool) {
        guard let engine = holder.engine, let entityId else { return }
        if beginNewEdit {
            engine.beginEdit("inspector")
        }
        let rgba = rgbaComponents(of: baseColor)
        _ = engine.setEntityMaterial(entityId, r: rgba.r, g: rgba.g, b: rgba.b, a: rgba.a,
                                     metallic: Float(metallic), roughness: Float(roughness),
                                     er: Float(emissiveR), eg: Float(emissiveG), eb: Float(emissiveB))
        onChanged()
    }

    private func clearShader() {
        guard let engine = holder.engine, let entityId else { return }
        // Records its own undo point (EngineRuntime::clearEntityShader).
        _ = engine.clearEntityShader(entityId)
        loadDetail()
        onChanged()
    }

    private func revealInFinder() {
        guard let generatedPath else { return }
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: generatedPath)])
    }

    private func rgbaComponents(of color: Color) -> (r: Float, g: Float, b: Float, a: Float) {
        let resolved = NSColor(color).usingColorSpace(.sRGB) ?? NSColor(color)
        return (Float(resolved.redComponent), Float(resolved.greenComponent),
                Float(resolved.blueComponent), Float(resolved.alphaComponent))
    }
}
