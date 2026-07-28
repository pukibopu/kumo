#include <doctest/doctest.h>

#include <kumo/facade/orbit_camera.h>
#include <kumo/math/math.h>
#include <kumo/scene/camera.h>

#include <cmath>

using namespace kumo;
using namespace kumo::facade;

namespace {
constexpr double kEps = 1e-4;
}

TEST_CASE("OrbitCamera: apply then syncFrom is a fixed point") {
    OrbitCamera orbit;
    orbit.rotate(200.0f, -80.0f); // arbitrary drag, away from the defaults
    orbit.zoom(-2.0f);

    scene::Camera camera;
    orbit.apply(camera);
    const math::float3 position1 = camera.position;
    const math::float3 forward1 = camera.rotation * math::float3(0.0f, 0.0f, -1.0f);

    orbit.syncFrom(camera);
    scene::Camera camera2;
    orbit.apply(camera2);
    const math::float3 forward2 = camera2.rotation * math::float3(0.0f, 0.0f, -1.0f);

    CHECK(camera2.position.x == doctest::Approx(position1.x).epsilon(kEps));
    CHECK(camera2.position.y == doctest::Approx(position1.y).epsilon(kEps));
    CHECK(camera2.position.z == doctest::Approx(position1.z).epsilon(kEps));
    CHECK(forward2.x == doctest::Approx(forward1.x).epsilon(kEps));
    CHECK(forward2.y == doctest::Approx(forward1.y).epsilon(kEps));
    CHECK(forward2.z == doctest::Approx(forward1.z).epsilon(kEps));
}

TEST_CASE("OrbitCamera: rotate clamps pitch to +-1.5") {
    OrbitCamera orbitUp;
    orbitUp.rotate(0.0f, 1.0e6f);
    scene::Camera cameraUp;
    orbitUp.apply(cameraUp); // default target (0,0,0), distance 3.0
    CHECK(cameraUp.position.y == doctest::Approx(std::sin(1.5f) * 3.0f).epsilon(kEps));

    OrbitCamera orbitDown;
    orbitDown.rotate(0.0f, -1.0e6f);
    scene::Camera cameraDown;
    orbitDown.apply(cameraDown);
    CHECK(cameraDown.position.y == doctest::Approx(std::sin(-1.5f) * 3.0f).epsilon(kEps));
}

TEST_CASE("OrbitCamera: zoom respects the 0.5 floor") {
    OrbitCamera orbit;
    orbit.zoom(1000.0f); // collapses the multiplier toward 0
    scene::Camera camera;
    orbit.apply(camera);
    CHECK(math::length(camera.position) == doctest::Approx(0.5f).epsilon(kEps));
}

TEST_CASE("OrbitCamera: zoom soft ceiling follows an imported distance, not a fixed 20") {
    scene::Camera camera;
    camera.position = {0.0f, 0.0f, 60.0f};
    camera.lookAt({0.0f, 0.0f, 0.0f});

    OrbitCamera orbit;
    orbit.syncFrom(camera); // imports distance ~60

    scene::Camera check;
    orbit.apply(check);
    CHECK(math::length(check.position) == doctest::Approx(60.0f).epsilon(1e-3));

    // Zoom-out candidate (60 * e^0.12 ~= 67.65) is clamped back down to the
    // imported distance, not the hardcoded 20 ceiling (ADR 0039).
    orbit.zoom(-1.0f);
    orbit.apply(check);
    CHECK(math::length(check.position) == doctest::Approx(60.0f).epsilon(1e-3));

    // Zoom-in is unaffected by the ceiling and walks down smoothly.
    orbit.zoom(1.0f);
    orbit.apply(check);
    CHECK(math::length(check.position) == doctest::Approx(60.0f * std::exp(-0.12f)).epsilon(1e-3));

    orbit.zoom(1.0f);
    orbit.apply(check);
    CHECK(math::length(check.position) ==
          doctest::Approx(60.0f * std::exp(-0.12f) * std::exp(-0.12f)).epsilon(1e-3));

    // The floor still holds after a large zoom-in.
    orbit.zoom(1000.0f);
    orbit.apply(check);
    CHECK(math::length(check.position) == doctest::Approx(0.5f).epsilon(1e-3));
}

TEST_CASE("OrbitCamera: syncFrom adopts an agent-moved camera; apply reproduces its view") {
    scene::Camera camera;
    camera.position = {5.0f, 2.0f, -3.0f};
    camera.lookAt({1.0f, 0.5f, 1.0f});

    OrbitCamera orbit;
    orbit.syncFrom(camera);

    scene::Camera reproduced;
    orbit.apply(reproduced);

    CHECK(reproduced.position.x == doctest::Approx(camera.position.x).epsilon(kEps));
    CHECK(reproduced.position.y == doctest::Approx(camera.position.y).epsilon(kEps));
    CHECK(reproduced.position.z == doctest::Approx(camera.position.z).epsilon(kEps));

    const math::float3 originalForward = camera.rotation * math::float3(0.0f, 0.0f, -1.0f);
    const math::float3 reproducedForward = reproduced.rotation * math::float3(0.0f, 0.0f, -1.0f);
    CHECK(reproducedForward.x == doctest::Approx(originalForward.x).epsilon(kEps));
    CHECK(reproducedForward.y == doctest::Approx(originalForward.y).epsilon(kEps));
    CHECK(reproducedForward.z == doctest::Approx(originalForward.z).epsilon(kEps));
}
