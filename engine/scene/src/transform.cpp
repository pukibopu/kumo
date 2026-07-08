#include <kumo/scene/transform.h>

namespace kumo::scene {

math::float4x4 Transform::matrix() const {
    return math::trs(position, rotation, scale);
}

} // namespace kumo::scene
