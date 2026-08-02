#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <kumo/asset/asset.h>
#include <kumo/gpu/gpu.h>
#include <kumo/math/math.h>
#include <kumo/renderer/forward_renderer.h>
#include <kumo/renderer/ibl.h>
#include <kumo/scene/scene.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace kumo;

namespace {

constexpr std::uint32_t kSize = 512;
// Fail when more than 1% of pixels differ by more than 3/255 in any channel.
constexpr int kChannelTolerance = 3;
constexpr double kMaxDifferingRatio = 0.01;

// Renderer + helmet scene reused across cases: the IBL bake dominates setup
// cost and must run once, not per render.
struct HelmetHarness {
    gpu::Device& device;
    renderer::ForwardRenderer forward;
    scene::Scene world;
    gpu::Ptr<gpu::Texture> output;

    explicit HelmetHarness(gpu::Device& deviceIn) : device(deviceIn) {
        auto sceneAsset =
            asset::loadGltf(std::filesystem::path(KUMO_ASSET_DIR) / "models" / "DamagedHelmet.glb");
        REQUIRE_MESSAGE(sceneAsset.has_value(), sceneAsset.error_or(""));
        auto hdr = asset::loadHdr(std::filesystem::path(KUMO_ASSET_DIR) / "env" /
                                  "studio_small_09_2k.hdr");
        REQUIRE_MESSAGE(hdr.has_value(), hdr.error_or(""));

        REQUIRE(forward.init(device, gpu::TextureFormat::BGRA8Unorm));
        const renderer::ibl::Environment environment = renderer::ibl::bake(device, *hdr);
        REQUIRE(environment.valid());
        REQUIRE(forward.loadScene(*sceneAsset, environment));
        forward.resize({kSize, kSize});

        // Mirrors the viewer defaults: orbit yaw 0 / pitch 0.15 / distance 3 and
        // a white directional light from azimuth -45, elevation 40 degrees.
        for (const asset::NodeInstance& node : sceneAsset->nodes) {
            if (node.meshIndex < 0) {
                continue;
            }
            const math::Trs trs = math::decomposeTrs(node.worldTransform);
            scene::Entity entity;
            entity.transform = {trs.translation, trs.rotation, trs.scale};
            entity.meshIndex = node.meshIndex;
            entity.materialIndex =
                sceneAsset->meshes[static_cast<std::size_t>(node.meshIndex)].materialIndex;
            world.entities.insert(entity);
        }
        world.camera.position = {0.0f, 3.0f * std::sin(0.15f), 3.0f * std::cos(0.15f)};
        world.camera.lookAt({0.0f, 0.0f, 0.0f});
        const float az = math::radians(-45.0f);
        const float el = math::radians(40.0f);
        world.addLight({.type = scene::LightType::Directional,
                        .intensity = 3.0f,
                        .direction = -math::float3{std::cos(el) * std::sin(az), std::sin(el),
                                                   std::cos(el) * std::cos(az)}});

        output = device.createTexture({
            .size = {kSize, kSize},
            .format = gpu::TextureFormat::BGRA8Unorm,
            .usage = gpu::TextureUsage::RenderTarget | gpu::TextureUsage::CopySrc,
        });
        REQUIRE(output != nullptr);
    }

    std::vector<std::uint8_t> render() {
        gpu::Ptr<gpu::CommandEncoder> encoder = device.queue().createCommandEncoder();
        forward.render(*encoder, world, output.get());
        encoder->finishAndSubmit();
        device.queue().waitIdle();

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSize) * kSize * 4);
        REQUIRE(device.queue().readTexture(*output, pixels.data(), kSize * 4, {kSize, kSize}));
        for (std::size_t i = 0; i < pixels.size(); i += 4) {
            std::swap(pixels[i], pixels[i + 2]); // BGRA -> RGBA
        }
        return pixels;
    }
};

// Pixels whose any channel differs by more than kChannelTolerance.
std::size_t countDiffering(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t differing = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])) >
                kChannelTolerance) {
                ++differing;
                break;
            }
        }
    }
    return differing;
}

void compareToBaseline(const std::vector<std::uint8_t>& actual, const char* backendName) {
    const std::filesystem::path baselinePath =
        std::filesystem::path(KUMO_GOLDEN_DIR) / backendName / "helmet.png";

    if (std::getenv("KUMO_UPDATE_GOLDEN") != nullptr) {
        std::error_code ec;
        std::filesystem::create_directories(baselinePath.parent_path(), ec);
        REQUIRE(asset::writePng(baselinePath, kSize, kSize, actual.data()));
        MESSAGE("golden baseline updated: ", baselinePath.string());
        return;
    }

    // Keep the actual around for inspection regardless of the outcome.
    const std::filesystem::path actualPath = std::filesystem::path(KUMO_GOLDEN_ACTUAL_DIR) /
                                             ("golden_actual_" + std::string(backendName) + ".png");
    asset::writePng(actualPath, kSize, kSize, actual.data());

    auto baseline = asset::loadImage(baselinePath);
    REQUIRE_MESSAGE(
        baseline.has_value(),
        "missing baseline (run with KUMO_UPDATE_GOLDEN=1 to create): ", baseline.error_or(""));
    REQUIRE(baseline->width == kSize);
    REQUIRE(baseline->height == kSize);

    std::size_t differing = 0;
    for (std::size_t i = 0; i < actual.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) {
            if (std::abs(static_cast<int>(actual[i + c]) -
                         static_cast<int>(baseline->rgba[i + c])) > kChannelTolerance) {
                ++differing;
                break;
            }
        }
    }
    const double ratio = static_cast<double>(differing) / (static_cast<double>(kSize) * kSize);
    CHECK_MESSAGE(ratio <= kMaxDifferingRatio, "differing pixel ratio ", ratio,
                  "; actual saved to ", actualPath.string());
}

} // namespace

TEST_CASE("golden image: helmet (metal)") {
    gpu::Ptr<gpu::Device> device = gpu::createDevice();
    REQUIRE(device != nullptr);
    HelmetHarness harness(*device);
    compareToBaseline(harness.render(), "metal");
}

TEST_CASE("debug views render distinct, non-black images and restore cleanly (metal)") {
    using DebugView = renderer::ForwardRenderer::DebugView;
    gpu::Ptr<gpu::Device> device = gpu::createDevice();
    REQUIRE(device != nullptr);
    HelmetHarness harness(*device);

    const std::vector<std::uint8_t> main = harness.render();
    const double total = static_cast<double>(kSize) * kSize;
    for (const DebugView view : {DebugView::Clay, DebugView::Normal, DebugView::Depth}) {
        harness.forward.setDebugView(view);
        const std::vector<std::uint8_t> image = harness.render();
        // Substantially different from the lit render, and not a black frame.
        CHECK(static_cast<double>(countDiffering(image, main)) / total > 0.05);
        std::size_t lit = 0;
        for (std::size_t i = 0; i < image.size(); i += 4) {
            if (image[i] > 8 || image[i + 1] > 8 || image[i + 2] > 8) {
                ++lit;
            }
        }
        CHECK(static_cast<double>(lit) / total > 0.05);
    }

    // Back to None: the frame must match the original render again.
    harness.forward.setDebugView(DebugView::None);
    const std::vector<std::uint8_t> restored = harness.render();
    CHECK(static_cast<double>(countDiffering(restored, main)) / total <= kMaxDifferingRatio);
}
