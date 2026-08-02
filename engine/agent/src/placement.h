#pragma once

#include <kumo/math/math.h>
#include <kumo/scene/transform.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace kumo::agent::placement {

// Deterministic, CPU-side placement math shared by scene_add_entity (tool
// layer), EngineRuntime::instantiateModel (facade) and scatter sampling. Every
// candidate bound is computed BEFORE any scene or renderer mutation.

// Y translation that puts `aabb.min.y` exactly at `clearance`.
float snapDeltaY(const math::Aabb& aabb, float clearance);

// Union of world-space boxes; nullopt when the span is empty.
std::optional<math::Aabb> aggregateAabb(std::span<const math::Aabb> boxes);

// Sub-millimeter penetration on any axis is numeric noise from transforms and
// snapping, never a real contact; shared by every predicate below and by
// scene_validate.
inline constexpr float kNoiseEps = 1e-3f;

// Fixed support tolerance; an unbounded user value would legalize deep overlap as support.
inline constexpr float kSupportTolerance = 0.02f;

// Per-axis penetration depths when `a` and `b` intersect on all three axes;
// nullopt otherwise.
std::optional<math::float3> overlapDepth(const math::Aabb& a, const math::Aabb& b);

// True when the overlap is only a box resting on another: Y penetration within
// `margin` AND located at the interface between the lower box's top and the
// upper box's bottom. The interface condition is what keeps thin geometry
// honest — a 1 cm wall buried sideways in a couch has a small X depth but is
// not support contact and must count as a collision.
bool supportContact(const math::Aabb& a, const math::Aabb& b, const math::float3& depth,
                    float margin);

// Real collision: overlapping on all axes, beyond noise, and not merely
// support contact (margin bounds the tolerated support penetration).
bool meaningfulOverlap(const math::Aabb& a, const math::Aabb& b, float margin);

struct Conflict {
    std::size_t index = 0; // into the `existing` span
    math::float3 depth{0.0f, 0.0f, 0.0f};
};

std::vector<Conflict> findConflicts(const math::Aabb& candidate,
                                    std::span<const math::Aabb> existing, float margin);

// Representative conflict for error reporting: the one hardest to dismiss
// (largest minimal-axis penetration). `conflicts` must be non-empty.
const Conflict& deepestConflict(std::span<const Conflict> conflicts);

// First conflict-free position on a deterministic ring search around
// `requested` in the XZ plane (Y kept): ring radius grows by the candidate's
// larger XZ extent plus margin, 8 fixed compass directions per ring, at most
// `maxRings` rings. Nullopt when every probe conflicts.
std::optional<math::float3> suggestPosition(const math::Aabb& candidate,
                                            const math::float3& requested,
                                            std::span<const math::Aabb> existing, float margin,
                                            int maxRings);

struct ScatterParams {
    std::int64_t count = 0;
    float area[2] = {0.0f, 0.0f};
    math::float3 center{0.0f, 0.0f, 0.0f};
    std::int64_t seed = 0;
    float scaleJitter = 0.0f;
    float rotationJitterDeg = 0.0f;
    float minSpacing = 0.0f;
    bool avoidExisting = false;
    std::int64_t maxAttempts = 0; // resolved default: max(10 x count, 64)
};

struct ScatterFailure {
    std::int64_t requested = 0;
    std::int64_t accepted = 0;
    std::int64_t attempts = 0;
};

// Deterministic rejection sampling: candidates draw x/z (and yaw/scale only
// when their jitter is non-zero) in a fixed order from one seeded engine, so
// identical arguments always produce identical output. A candidate instance's
// footprint is `groupLocalAabb` transformed by the candidate transform; it is
// rejected when closer than `minSpacing` (XZ, edge to edge) to an accepted
// instance, or, with `avoidExisting`, meaningfully overlapping (margin
// `existingMargin`, shallow support contact ignored) any box in `existing`.
// Fails without output when `count` cannot be placed within `maxAttempts`.
std::expected<std::vector<scene::Transform>, ScatterFailure>
sampleScatter(const ScatterParams& params, const math::Aabb& groupLocalAabb,
              std::span<const math::Aabb> existing, float existingMargin);

} // namespace kumo::agent::placement
