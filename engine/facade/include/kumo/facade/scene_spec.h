#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::facade {

// One element of the director's plan; only `name` is mandatory in the wire
// JSON, the two intents feed the Build and Materials stages respectively.
struct SceneSpecElement {
    std::string name;
    std::string build;
    std::string materialIntent;
};

// The director's scene plan (MC), parsed leniently from a model reply. `raw`
// keeps the JSON text verbatim: it is what gets persisted (SavedScene::
// director_spec) and re-parsed on load, so the struct never needs to
// round-trip through a serializer. Sub-objects the pipeline forwards but
// never interprets (camera/lighting/budgets) stay as compact JSON strings.
struct SceneSpec {
    std::string raw;
    std::string styleTier; // "realistic" | "stylized" | free-form
    std::vector<std::string> palette;
    std::string cameraJson;
    std::vector<std::string> assets;
    std::vector<SceneSpecElement> elements;
    std::string lightingJson;
    std::vector<std::string> banned;
    std::string budgetsJson;
};

// Model replies wrap JSON in prose and ```json fences: this finds the first
// balanced top-level object (string-literal aware) and parses that. Errors:
// no object found, malformed JSON, or a plan with neither elements nor
// assets (nothing actionable).
std::expected<SceneSpec, std::string> parseSceneSpec(std::string_view text);

enum class SpecStage { Build, Materials };

// The stage-scoped plan as a user-message prefix (MC-4): Build gets
// layout/assets/lighting/camera plus each element's build intent, Materials
// gets each element's material intent; style tier, palette and bans go to
// both. Empty spec parts are omitted.
std::string sliceSpec(const SceneSpec& spec, SpecStage stage);

} // namespace kumo::facade
