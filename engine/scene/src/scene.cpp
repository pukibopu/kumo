#include <kumo/scene/scene.h>

namespace kumo::scene {

bool Scene::addLight(const Light& light) {
    if (lights.size() >= kMaxLights) {
        return false;
    }
    lights.push_back(light);
    return true;
}

} // namespace kumo::scene
