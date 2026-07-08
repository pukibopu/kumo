#include <doctest/doctest.h>

#include <kumo/math/math.h>
#include <kumo/scene/scene.h>

#include <cmath>
#include <cstddef>

using namespace kumo;

TEST_CASE("SlotMap insert/get/remove and stale-generation invalidation") {
    scene::SlotMap<int> map;

    scene::EntityId a = map.insert(10);
    scene::EntityId b = map.insert(20);
    CHECK(map.size() == 2);
    REQUIRE(map.get(a) != nullptr);
    REQUIRE(map.get(b) != nullptr);
    CHECK(*map.get(a) == 10);
    CHECK(*map.get(b) == 20);

    CHECK(map.remove(a));
    CHECK(map.get(a) == nullptr);
    CHECK(map.size() == 1);
    CHECK_FALSE(map.remove(a));

    scene::EntityId c = map.insert(30);
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK(map.get(a) == nullptr);
    REQUIRE(map.get(c) != nullptr);
    CHECK(*map.get(c) == 30);

    std::size_t count = 0;
    map.forEach([&](scene::EntityId, int&) { ++count; });
    CHECK(count == 2);
    CHECK(count == map.size());
}

TEST_CASE("Scene::addLight caps at 16") {
    scene::Scene s;
    for (int i = 0; i < 16; ++i) {
        CHECK(s.addLight(scene::Light{}));
    }
    CHECK(s.lights().size() == scene::Scene::kMaxLights);
    CHECK_FALSE(s.addLight(scene::Light{}));
    CHECK(s.lights().size() == 16);

    REQUIRE(s.light(0) != nullptr);
    CHECK(s.light(16) == nullptr);

    s.clearLights();
    CHECK(s.lights().empty());
}

TEST_CASE("Camera lookAt yields a view that puts the eye at the origin") {
    scene::Camera cam;
    cam.position = {0.0f, 0.0f, 5.0f};
    cam.lookAt({0.0f, 0.0f, 0.0f});

    math::float4x4 view = cam.view();
    math::float4 eyeInView = view * math::float4(cam.position, 1.0f);
    CHECK(std::abs(eyeInView.x) < 1e-4f);
    CHECK(std::abs(eyeInView.y) < 1e-4f);
    CHECK(std::abs(eyeInView.z) < 1e-4f);

    math::float4 targetInView = view * math::float4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(targetInView.z < 0.0f);
}
