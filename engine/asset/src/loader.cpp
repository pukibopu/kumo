#include <kumo/asset/asset.h>

#include <kumo/core/log.h>

#include <cgltf.h>
#include <stb_image.h>

#include <climits>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace kumo::asset {
namespace {

const cgltf_accessor* findAttribute(const cgltf_primitive& prim, cgltf_attribute_type type,
                                    cgltf_int setIndex) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        const cgltf_attribute& attr = prim.attributes[i];
        if (attr.type == type && attr.index == setIndex) {
            return attr.data;
        }
    }
    return nullptr;
}

std::int32_t textureIndex(const cgltf_data* data, const cgltf_texture* tex) {
    if (tex == nullptr) {
        return -1;
    }
    return static_cast<std::int32_t>(tex - data->textures);
}

std::expected<TextureData, std::string> decodeImage(const cgltf_image& image,
                                                    const std::filesystem::path& gltfDir,
                                                    const std::string& pathStr) {
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* pixels = nullptr;

    if (image.buffer_view != nullptr) {
        const cgltf_buffer_view& view = *image.buffer_view;
        if (view.buffer == nullptr || view.buffer->data == nullptr) {
            return std::unexpected(std::format("glTF image buffer not loaded in {}", pathStr));
        }
        if (view.offset + view.size > view.buffer->size) {
            return std::unexpected(
                std::format("glTF image buffer view out of range in {}", pathStr));
        }
        if (view.size > static_cast<cgltf_size>(INT_MAX)) {
            return std::unexpected(std::format("glTF image too large to decode in {}", pathStr));
        }
        const auto* base = static_cast<const std::uint8_t*>(view.buffer->data);
        const std::uint8_t* bytes = base + view.offset;
        pixels = stbi_load_from_memory(bytes, static_cast<int>(view.size), &w, &h, &comp, 4);
    } else if (image.uri != nullptr && std::strncmp(image.uri, "data:", 5) != 0) {
        std::filesystem::path file = gltfDir / image.uri;
        pixels = stbi_load(file.string().c_str(), &w, &h, &comp, 4);
    } else {
        return std::unexpected(std::format("unsupported glTF image source in {}", pathStr));
    }

    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        return std::unexpected(std::format("failed to decode glTF image in {} ({})", pathStr,
                                           reason != nullptr ? reason : "unknown"));
    }

    TextureData tex;
    tex.width = static_cast<std::uint32_t>(w);
    tex.height = static_cast<std::uint32_t>(h);
    tex.srgb = false;
    const std::size_t byteCount = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    tex.rgba.assign(pixels, pixels + byteCount);
    stbi_image_free(pixels);
    return tex;
}

std::expected<MeshData, std::string> buildMesh(const cgltf_data* data, const cgltf_primitive& prim,
                                               const std::string& pathStr, bool& tangentMissing) {
    if (prim.type != cgltf_primitive_type_triangles) {
        return std::unexpected(std::format("non-triangle primitive in {}", pathStr));
    }

    const cgltf_accessor* pos = findAttribute(prim, cgltf_attribute_type_position, 0);
    const cgltf_accessor* nrm = findAttribute(prim, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* tan = findAttribute(prim, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor* uv = findAttribute(prim, cgltf_attribute_type_texcoord, 0);

    if (pos == nullptr) {
        return std::unexpected(std::format("primitive missing POSITION in {}", pathStr));
    }
    if (nrm == nullptr) {
        return std::unexpected(std::format("primitive missing NORMAL in {}", pathStr));
    }
    if (tan == nullptr) {
        tangentMissing = true;
    }

    MeshData mesh;
    const cgltf_size vertexCount = pos->count;
    mesh.vertices.reserve(vertexCount);
    for (cgltf_size i = 0; i < vertexCount; ++i) {
        Vertex v{};
        float p[3];
        float n[3];
        if (cgltf_accessor_read_float(pos, i, p, 3) == 0 ||
            cgltf_accessor_read_float(nrm, i, n, 3) == 0) {
            return std::unexpected(std::format("failed to read vertex attribute in {}", pathStr));
        }
        v.px = p[0];
        v.py = p[1];
        v.pz = p[2];
        v.nx = n[0];
        v.ny = n[1];
        v.nz = n[2];

        if (tan != nullptr) {
            float t[4];
            if (cgltf_accessor_read_float(tan, i, t, 4) == 0) {
                return std::unexpected(std::format("failed to read TANGENT in {}", pathStr));
            }
            v.tx = t[0];
            v.ty = t[1];
            v.tz = t[2];
            v.tw = t[3];
        } else {
            v.tx = 1.0f;
            v.ty = 0.0f;
            v.tz = 0.0f;
            v.tw = 1.0f;
        }

        if (uv != nullptr) {
            float c[2];
            if (cgltf_accessor_read_float(uv, i, c, 2) == 0) {
                return std::unexpected(std::format("failed to read TEXCOORD_0 in {}", pathStr));
            }
            v.u = c[0];
            v.v = c[1];
        } else {
            v.u = 0.0f;
            v.v = 0.0f;
        }
        mesh.vertices.push_back(v);
    }

    if (prim.indices != nullptr) {
        mesh.indices.reserve(prim.indices->count);
        for (cgltf_size i = 0; i < prim.indices->count; ++i) {
            mesh.indices.push_back(
                static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
        }
    } else {
        mesh.indices.reserve(vertexCount);
        for (cgltf_size i = 0; i < vertexCount; ++i) {
            mesh.indices.push_back(static_cast<std::uint32_t>(i));
        }
    }

    mesh.materialIndex =
        prim.material != nullptr ? static_cast<std::int32_t>(prim.material - data->materials) : -1;
    return mesh;
}

} // namespace

std::expected<SceneAsset, std::string> loadGltf(const std::filesystem::path& path) {
    const std::string pathStr = path.string();

    cgltf_options options{};
    cgltf_data* raw = nullptr;
    if (cgltf_parse_file(&options, pathStr.c_str(), &raw) != cgltf_result_success) {
        return std::unexpected(std::format("failed to parse glTF file: {}", pathStr));
    }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, &cgltf_free);

    if (cgltf_load_buffers(&options, data.get(), pathStr.c_str()) != cgltf_result_success) {
        return std::unexpected(std::format("failed to load glTF buffers: {}", pathStr));
    }

    const std::filesystem::path gltfDir = path.parent_path();
    SceneAsset out;

    out.textures.reserve(data->textures_count);
    for (cgltf_size i = 0; i < data->textures_count; ++i) {
        const cgltf_texture& tex = data->textures[i];
        if (tex.image == nullptr) {
            return std::unexpected(std::format("glTF texture has no image in {}", pathStr));
        }
        auto decoded = decodeImage(*tex.image, gltfDir, pathStr);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        out.textures.push_back(std::move(*decoded));
    }

    // A glTF image may feed an sRGB slot (baseColor/emissive) in one material and a
    // linear slot (normal/MR/occlusion) in another; a single srgb flag decodes one
    // use wrong. Track the color space each reference claims and, on conflict,
    // duplicate the texel data for the second color space and remap that reference.
    std::vector<std::optional<bool>> claimed(out.textures.size());
    std::unordered_map<std::int64_t, std::int32_t> duplicates;
    auto resolveColorSpace = [&](std::int32_t index, bool srgb) -> std::int32_t {
        if (index < 0) {
            return index;
        }
        const auto i = static_cast<std::size_t>(index);
        if (!claimed[i].has_value()) {
            claimed[i] = srgb;
            out.textures[i].srgb = srgb;
            return index;
        }
        if (*claimed[i] == srgb) {
            return index;
        }
        const std::int64_t key = (static_cast<std::int64_t>(index) << 1) | (srgb ? 1 : 0);
        if (auto it = duplicates.find(key); it != duplicates.end()) {
            return it->second;
        }
        TextureData copy = out.textures[i];
        copy.srgb = srgb;
        const auto dup = static_cast<std::int32_t>(out.textures.size());
        out.textures.push_back(std::move(copy));
        duplicates.emplace(key, dup);
        return dup;
    };

    out.materials.reserve(data->materials_count);
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& src = data->materials[i];
        MaterialData mat;
        if (src.has_pbr_metallic_roughness != 0) {
            const cgltf_pbr_metallic_roughness& pbr = src.pbr_metallic_roughness;
            for (int c = 0; c < 4; ++c) {
                mat.baseColor[c] = pbr.base_color_factor[c];
            }
            mat.metallic = pbr.metallic_factor;
            mat.roughness = pbr.roughness_factor;
            mat.baseColorTexture = textureIndex(data.get(), pbr.base_color_texture.texture);
            mat.metallicRoughnessTexture =
                textureIndex(data.get(), pbr.metallic_roughness_texture.texture);
        }
        for (int c = 0; c < 3; ++c) {
            mat.emissive[c] = src.emissive_factor[c];
        }
        mat.normalTexture = textureIndex(data.get(), src.normal_texture.texture);
        mat.occlusionTexture = textureIndex(data.get(), src.occlusion_texture.texture);
        mat.emissiveTexture = textureIndex(data.get(), src.emissive_texture.texture);

        mat.baseColorTexture = resolveColorSpace(mat.baseColorTexture, true);
        mat.emissiveTexture = resolveColorSpace(mat.emissiveTexture, true);
        mat.normalTexture = resolveColorSpace(mat.normalTexture, false);
        mat.metallicRoughnessTexture = resolveColorSpace(mat.metallicRoughnessTexture, false);
        mat.occlusionTexture = resolveColorSpace(mat.occlusionTexture, false);
        out.materials.push_back(mat);
    }

    bool tangentMissing = false;
    std::vector<std::vector<std::int32_t>> primIndices(data->meshes_count);
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& mesh = data->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
            auto built = buildMesh(data.get(), mesh.primitives[pi], pathStr, tangentMissing);
            if (!built) {
                return std::unexpected(built.error());
            }
            primIndices[mi].push_back(static_cast<std::int32_t>(out.meshes.size()));
            out.meshes.push_back(std::move(*built));
        }
    }

    if (tangentMissing) {
        logInfo("glTF {} has primitives without tangents; using default (1,0,0,1)", pathStr);
    }

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (node.mesh == nullptr) {
            continue;
        }
        cgltf_float wm[16];
        cgltf_node_transform_world(&node, wm);
        const math::float4x4 world(wm[0], wm[1], wm[2], wm[3], wm[4], wm[5], wm[6], wm[7], wm[8],
                                   wm[9], wm[10], wm[11], wm[12], wm[13], wm[14], wm[15]);
        const auto meshIdx = static_cast<cgltf_size>(node.mesh - data->meshes);
        for (std::int32_t flat : primIndices[meshIdx]) {
            NodeInstance inst;
            inst.name = node.name != nullptr ? node.name : "";
            inst.worldTransform = world;
            inst.meshIndex = flat;
            out.nodes.push_back(std::move(inst));
        }
    }

    return out;
}

std::expected<HdrImage, std::string> loadHdr(const std::filesystem::path& path) {
    const std::string pathStr = path.string();
    int w = 0;
    int h = 0;
    int comp = 0;
    float* pixels = stbi_loadf(pathStr.c_str(), &w, &h, &comp, 4);
    if (pixels == nullptr) {
        const char* reason = stbi_failure_reason();
        return std::unexpected(std::format("failed to load HDR image: {} ({})", pathStr,
                                           reason != nullptr ? reason : "unknown"));
    }

    HdrImage img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    const std::size_t count = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
    img.rgba.assign(pixels, pixels + count);
    stbi_image_free(pixels);
    return img;
}

} // namespace kumo::asset
