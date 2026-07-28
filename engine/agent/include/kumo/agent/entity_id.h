#pragma once

#include <kumo/scene/slot_map.h>

#include <optional>
#include <string>
#include <string_view>

namespace kumo::agent {

// Wire shape for scene::EntityId, shared by scene_tools, shader_tools and the
// facade inspector API: "index:generation".
std::string formatEntityId(scene::EntityId id);
std::optional<scene::EntityId> parseEntityId(std::string_view text);

} // namespace kumo::agent
