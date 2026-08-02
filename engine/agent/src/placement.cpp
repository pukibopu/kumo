#include "placement.h"

#include <algorithm>
#include <array>
#include <random>
#include <utility>

namespace kumo::agent::placement {

float snapDeltaY(const math::Aabb& aabb, float clearance) {
    return clearance - aabb.min.y;
}

std::optional<math::Aabb> aggregateAabb(std::span<const math::Aabb> boxes) {
    if (boxes.empty()) {
        return std::nullopt;
    }
    math::Aabb out = boxes.front();
    for (std::size_t i = 1; i < boxes.size(); ++i) {
        out.min = glm::min(out.min, boxes[i].min);
        out.max = glm::max(out.max, boxes[i].max);
    }
    return out;
}

std::optional<math::float3> overlapDepth(const math::Aabb& a, const math::Aabb& b) {
    const float dx = std::min(a.max.x, b.max.x) - std::max(a.min.x, b.min.x);
    const float dy = std::min(a.max.y, b.max.y) - std::max(a.min.y, b.min.y);
    const float dz = std::min(a.max.z, b.max.z) - std::max(a.min.z, b.min.z);
    if (dx <= 0.0f || dy <= 0.0f || dz <= 0.0f) {
        return std::nullopt;
    }
    return math::float3{dx, dy, dz};
}

bool supportContact(const math::Aabb& a, const math::Aabb& b, const math::float3& depth,
                    float margin) {
    if (depth.y > margin) {
        return false;
    }
    // Vertically ordered: the shallow Y interval sits where one box's bottom
    // meets the other's top. A thin box buried mid-height in a tall one also
    // has a small Y depth (its own height) but fails this interface test.
    return a.min.y >= b.max.y - margin || b.min.y >= a.max.y - margin;
}

bool meaningfulOverlap(const math::Aabb& a, const math::Aabb& b, float margin) {
    const std::optional<math::float3> depth = overlapDepth(a, b);
    if (!depth.has_value()) {
        return false;
    }
    if (std::min({depth->x, depth->y, depth->z}) <= kNoiseEps) {
        return false;
    }
    return !supportContact(a, b, *depth, margin);
}

std::vector<Conflict> findConflicts(const math::Aabb& candidate,
                                    std::span<const math::Aabb> existing, float margin) {
    std::vector<Conflict> conflicts;
    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (meaningfulOverlap(candidate, existing[i], margin)) {
            conflicts.push_back({.index = i, .depth = *overlapDepth(candidate, existing[i])});
        }
    }
    return conflicts;
}

const Conflict& deepestConflict(std::span<const Conflict> conflicts) {
    const auto minAxis = [](const math::float3& d) { return std::min({d.x, d.y, d.z}); };
    const Conflict* deepest = &conflicts.front();
    for (const Conflict& conflict : conflicts) {
        if (minAxis(conflict.depth) > minAxis(deepest->depth)) {
            deepest = &conflict;
        }
    }
    return *deepest;
}

std::optional<math::float3> suggestPosition(const math::Aabb& candidate,
                                            const math::float3& requested,
                                            std::span<const math::Aabb> existing, float margin,
                                            int maxRings) {
    const float extentX = candidate.max.x - candidate.min.x;
    const float extentZ = candidate.max.z - candidate.min.z;
    const float step = std::max({extentX, extentZ, 0.1f}) + margin;
    // Fixed compass order keeps the first free probe deterministic.
    constexpr std::array<std::array<float, 2>, 8> kDirections{{{1.0f, 0.0f},
                                                               {-1.0f, 0.0f},
                                                               {0.0f, 1.0f},
                                                               {0.0f, -1.0f},
                                                               {1.0f, 1.0f},
                                                               {-1.0f, 1.0f},
                                                               {1.0f, -1.0f},
                                                               {-1.0f, -1.0f}}};
    for (int ring = 1; ring <= maxRings; ++ring) {
        const float radius = step * static_cast<float>(ring);
        for (const auto& dir : kDirections) {
            const math::float3 offset{dir[0] * radius, 0.0f, dir[1] * radius};
            const math::Aabb shifted{candidate.min + offset, candidate.max + offset};
            if (findConflicts(shifted, existing, margin).empty()) {
                return requested + offset;
            }
        }
    }
    return std::nullopt;
}

std::expected<std::vector<scene::Transform>, ScatterFailure>
sampleScatter(const ScatterParams& params, const math::Aabb& groupLocalAabb,
              std::span<const math::Aabb> existing, float existingMargin) {
    const std::int64_t maxAttempts =
        params.maxAttempts > 0 ? params.maxAttempts : std::max<std::int64_t>(params.count * 10, 64);

    std::mt19937 rng(static_cast<std::uint32_t>(static_cast<std::uint64_t>(params.seed)));
    std::uniform_real_distribution<float> xDist(-params.area[0] * 0.5f, params.area[0] * 0.5f);
    std::uniform_real_distribution<float> zDist(-params.area[1] * 0.5f, params.area[1] * 0.5f);
    std::uniform_real_distribution<float> yawDist(-params.rotationJitterDeg,
                                                  params.rotationJitterDeg);
    std::uniform_real_distribution<float> scaleDist(1.0f - params.scaleJitter,
                                                    1.0f + params.scaleJitter);

    std::vector<scene::Transform> accepted;
    std::vector<math::Aabb> footprints;
    accepted.reserve(static_cast<std::size_t>(params.count));
    footprints.reserve(static_cast<std::size_t>(params.count));

    std::int64_t attempts = 0;
    while (std::cmp_less(accepted.size(), params.count) && attempts < maxAttempts) {
        ++attempts;
        scene::Transform t;
        t.position = {params.center.x + xDist(rng), params.center.y, params.center.z + zDist(rng)};
        const float yaw = params.rotationJitterDeg > 0.0f ? yawDist(rng) : 0.0f;
        t.rotation = math::quatFromEulerDegrees({0.0f, yaw, 0.0f});
        const float factor = params.scaleJitter > 0.0f ? scaleDist(rng) : 1.0f;
        t.scale = {factor, factor, factor};

        const math::Aabb footprint = math::transformAabb(groupLocalAabb, t.matrix());

        // Spacing versus already-accepted instances: XZ edge-to-edge distance
        // must reach minSpacing, tested by inflating one footprint and
        // intersecting in XZ only — a plane group has a zero-height AABB, so a
        // 3D test would never see the Y axis overlap and spacing would be
        // silently ignored. Skipped entirely at minSpacing 0 — the legacy
        // scatter allowed overlapping instances, and identical arguments must
        // keep identical output.
        bool ok = true;
        if (params.minSpacing > 0.0f) {
            for (const math::Aabb& prior : footprints) {
                const float dx = std::min(footprint.max.x + params.minSpacing, prior.max.x) -
                                 std::max(footprint.min.x - params.minSpacing, prior.min.x);
                const float dz = std::min(footprint.max.z + params.minSpacing, prior.max.z) -
                                 std::max(footprint.min.z - params.minSpacing, prior.min.z);
                if (dx > 0.0f && dz > 0.0f) {
                    ok = false;
                    break;
                }
            }
        }
        if (ok && params.avoidExisting) {
            for (const math::Aabb& other : existing) {
                if (meaningfulOverlap(footprint, other, existingMargin)) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) {
            continue;
        }
        accepted.push_back(t);
        footprints.push_back(footprint);
    }

    if (std::cmp_less(accepted.size(), params.count)) {
        return std::unexpected(
            ScatterFailure{.requested = params.count,
                           .accepted = static_cast<std::int64_t>(accepted.size()),
                           .attempts = attempts});
    }
    return accepted;
}

} // namespace kumo::agent::placement
