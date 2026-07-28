#pragma once

#include <kumo/math/math.h>
#include <kumo/scene/camera.h>

namespace kumo::facade {

// App-layer camera control (ADR 0039): drag rotates, scroll zooms. The pose is
// written to the scene camera only on user input; otherwise the caller syncs
// this from the camera, so agent camera_set calls are not overwritten. Shared
// by every shell (GLFW dev viewer, SwiftUI product GUI) via EngineRuntime; a
// shell only collects raw input and forwards deltas.
class OrbitCamera {
public:
    void rotate(float dx, float dy);
    void zoom(float scroll);
    void apply(scene::Camera& camera) const;

    // Adopts the camera's aim as the new pivot: the target moves onto the view
    // ray at the previous pivot distance, so the next drag orbits the agent's
    // framing instead of snapping back to the old target. Imported pitch is
    // clamped into the range rotate() allows.
    void syncFrom(const scene::Camera& camera);

private:
    float yaw_ = 0.0f;
    float pitch_ = 0.15f;
    float distance_ = 3.0f;
    math::float3 target_{0.0f, 0.0f, 0.0f};
};

} // namespace kumo::facade
