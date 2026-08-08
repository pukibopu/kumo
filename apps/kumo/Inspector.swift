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
// viewer for the selected entity. EngineRuntime::beginEdit opens a pending
// undo point ahead of the corresponding setEntity*/clearEntityShader call,
// which commits it only on success (EngineRuntime's pending-commit model), so
// a rejected edit never leaves a phantom undo step.
struct InspectorView: View {
    @ObservedObject var holder: EngineHolder
    let entityId: String?
    let refreshToken: Int
    let onChanged: () -> Void

    @State private var detail: KumoEntityDetail?
    @State private var surfaceParams: [KumoSurfaceParam] = []

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

    // Debounces ColorPicker's continuous onChange stream (one event per drag
    // step in its popover) into a single undo gesture: edits within 0.75s of
    // each other coalesce, same idea as the metallic/roughness slider gating.
    @State private var lastColorEditAt: Date?
    // Set when a commit setter returns false; cleared by the next successful
    // edit or by selecting a different entity.
    @State private var inspectorError: String?

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
                    if let inspectorError {
                        Section {
                            Text(inspectorError)
                                .font(.caption)
                                .foregroundStyle(.red)
                        }
                    }
                    if !surfaceParams.isEmpty {
                        surfaceParamsSection
                    }
                    if detail.hasCustomShader {
                        shaderSection
                    }
                }
                .formStyle(.grouped)
            } else {
                // Nothing selected: the least intrusive spot for scene-level
                // controls (ADR 0044) rather than a dedicated sidebar section.
                LightPanelView(holder: holder, onChanged: onChanged)
            }
        }
        .frame(minWidth: 280, idealWidth: 300, maxWidth: 340)
        .onAppear { loadDetail() }
        .onChange(of: entityId) { _, _ in
            inspectorError = nil
            loadDetail()
        }
        .onChange(of: refreshToken) { _, _ in loadDetail() }
    }

    @ViewBuilder
    private var materialSection: some View {
        Section("材质") {
            ColorPicker("基础色", selection: $baseColor, supportsOpacity: true)
                .onChange(of: baseColor) { _, _ in
                    let now = Date()
                    let isNewGesture = lastColorEditAt.map { now.timeIntervalSince($0) > 0.75 }
                        ?? true
                    lastColorEditAt = now
                    commitMaterial(beginNewEdit: isNewGesture)
                }
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

    // Surface-function params (MD): floats get an adaptive slider, vec4s a
    // color picker, mirroring the light panel's commit gating.
    @ViewBuilder
    private var surfaceParamsSection: some View {
        Section("表面参数") {
            ForEach(surfaceParams, id: \.name) { param in
                if param.isVec4 {
                    ColorPicker(param.name,
                                selection: Binding(
                                    get: {
                                        Color(red: Double(param.value0),
                                              green: Double(param.value1),
                                              blue: Double(param.value2),
                                              opacity: Double(param.value3))
                                    },
                                    set: { color in
                                        let rgba = rgbaComponents(of: color)
                                        commitSurfaceParam(name: param.name,
                                                           values: [rgba.r, rgba.g, rgba.b,
                                                                    rgba.a])
                                    }),
                                supportsOpacity: true)
                } else {
                    LabeledContent(param.name) {
                        Slider(value: Binding(
                            get: { Double(param.value0) },
                            set: { commitSurfaceParam(name: param.name, values: [Float($0)]) }),
                            in: 0...max(1.0, Double(param.value0) * 2)) { began in
                            if began { holder.engine?.beginEdit("inspector") }
                        }
                    }
                }
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
        surfaceParams = engine.entitySurfaceParams(entityId)
    }

    private func commitTransform() {
        guard let engine = holder.engine, let entityId else { return }
        engine.beginEdit("inspector")
        // With the pending-commit model a failed set never commits an undo
        // step on its own (EngineRuntime::setEntityTransform); the discarded
        // boolean is still fine to ignore for undo purposes, but surface it.
        let ok = engine.setEntityTransform(entityId, px: Float(positionX), py: Float(positionY),
                                           pz: Float(positionZ), rx: Float(eulerX),
                                           ry: Float(eulerY), rz: Float(eulerZ), sx: Float(scaleX),
                                           sy: Float(scaleY), sz: Float(scaleZ))
        inspectorError = ok ? nil : "变换设置失败"
        onChanged()
    }

    private func commitMaterial(beginNewEdit: Bool) {
        guard let engine = holder.engine, let entityId else { return }
        if beginNewEdit {
            engine.beginEdit("inspector")
        }
        let rgba = rgbaComponents(of: baseColor)
        // With the pending-commit model a failed set never commits an undo
        // step on its own (EngineRuntime::setEntityMaterial); surface it.
        let ok = engine.setEntityMaterial(entityId, r: rgba.r, g: rgba.g, b: rgba.b, a: rgba.a,
                                          metallic: Float(metallic), roughness: Float(roughness),
                                          er: Float(emissiveR), eg: Float(emissiveG),
                                          eb: Float(emissiveB))
        inspectorError = ok ? nil : "材质设置失败"
        onChanged()
    }

    private func commitSurfaceParam(name: String, values: [Float]) {
        guard let engine = holder.engine, let entityId else { return }
        let now = Date()
        let isNewGesture = lastColorEditAt.map { now.timeIntervalSince($0) > 0.75 } ?? true
        lastColorEditAt = now
        if isNewGesture { engine.beginEdit("inspector") }
        let ok = engine.setEntitySurfaceParam(entityId, name: name,
                                              values: values.map { NSNumber(value: $0) })
        inspectorError = ok ? nil : "表面参数设置失败"
        surfaceParams = engine.entitySurfaceParams(entityId)
        onChanged()
    }

    private func clearShader() {
        guard let engine = holder.engine, let entityId else { return }
        // Opens and commits its own undo point (EngineRuntime::clearEntityShader).
        let ok = engine.clearEntityShader(entityId)
        inspectorError = ok ? nil : "还原失败"
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

// Sun light (light index 0) + shadow toggle: shown in the inspector slot when
// nothing is selected (ADR 0044), the least intrusive spot for a scene-level
// control. Mirrors apps/viewer/ui.cpp's drawLightPanel/LightSettings, and
// InspectorView's pending-commit undo wiring (beginEdit ahead of the setter
// that commits it, ColorPicker's 0.75s coalescing). Reflects agent-driven
// changes via its own poll timer, same cadence as KumoApp's entity/undo
// refresh; safe against an in-progress local drag because every slider step
// already committed that same value to the engine (a read-back mid-drag is a
// no-op, not a jump).
private struct LightPanelView: View {
    @ObservedObject var holder: EngineHolder
    let onChanged: () -> Void

    @State private var found = false
    @State private var azimuthDeg = 0.0
    @State private var elevationDeg = 0.0
    @State private var intensity = 0.0
    @State private var color = Color.white
    @State private var shadowsOn = true

    @State private var lastColorEditAt: Date?
    @State private var lightError: String?

    private let refreshTimer = Timer.publish(every: 1.5, on: .main, in: .common).autoconnect()

    var body: some View {
        Form {
            Section("光照") {
                if found {
                    LabeledContent("方位角") {
                        Slider(value: $azimuthDeg, in: -180...180) { began in
                            if began { holder.engine?.beginEdit("light") }
                        }
                    }
                    .onChange(of: azimuthDeg) { _, _ in commitLight(beginNewEdit: false) }
                    LabeledContent("高度角") {
                        Slider(value: $elevationDeg, in: -85...85) { began in
                            if began { holder.engine?.beginEdit("light") }
                        }
                    }
                    .onChange(of: elevationDeg) { _, _ in commitLight(beginNewEdit: false) }
                    LabeledContent("强度") {
                        Slider(value: $intensity, in: 0...10) { began in
                            if began { holder.engine?.beginEdit("light") }
                        }
                    }
                    .onChange(of: intensity) { _, _ in commitLight(beginNewEdit: false) }
                    ColorPicker("颜色", selection: $color, supportsOpacity: false)
                        .onChange(of: color) { _, _ in
                            let now = Date()
                            let isNewGesture = lastColorEditAt.map { now.timeIntervalSince($0) > 0.75 }
                                ?? true
                            lastColorEditAt = now
                            commitLight(beginNewEdit: isNewGesture)
                        }
                    if let lightError {
                        Text(lightError)
                            .font(.caption)
                            .foregroundStyle(.red)
                    }
                } else {
                    Text("场景中没有灯光")
                        .foregroundStyle(.secondary)
                }
                Toggle("阴影", isOn: $shadowsOn)
                    .onChange(of: shadowsOn) { _, newValue in
                        holder.engine?.setShadowsEnabled(newValue)
                        onChanged()
                    }
            }
        }
        .formStyle(.grouped)
        .onAppear { loadLight() }
        .onReceive(refreshTimer) { _ in loadLight() }
    }

    private func loadLight() {
        guard let engine = holder.engine else { return }
        let detail = engine.sunLightDetail()
        found = detail.found
        if detail.found {
            azimuthDeg = Double(detail.azimuthDeg)
            elevationDeg = Double(detail.elevationDeg)
            intensity = Double(detail.intensity)
            color = Color(red: Double(detail.colorR), green: Double(detail.colorG),
                          blue: Double(detail.colorB))
        }
        shadowsOn = engine.shadowsEnabled()
    }

    private func commitLight(beginNewEdit: Bool) {
        guard let engine = holder.engine else { return }
        if beginNewEdit {
            engine.beginEdit("light")
        }
        let rgb = rgbComponents(of: color)
        // Pending-commit model (EngineRuntime::setSunLight): a failed set never
        // commits an undo step on its own.
        let ok = engine.setSunLightAzimuthDeg(Float(azimuthDeg), elevationDeg: Float(elevationDeg),
                                             intensity: Float(intensity), r: rgb.r, g: rgb.g,
                                             b: rgb.b)
        lightError = ok ? nil : "光照设置失败"
        onChanged()
    }

    private func rgbComponents(of color: Color) -> (r: Float, g: Float, b: Float) {
        let resolved = NSColor(color).usingColorSpace(.sRGB) ?? NSColor(color)
        return (Float(resolved.redComponent), Float(resolved.greenComponent),
                Float(resolved.blueComponent))
    }
}
