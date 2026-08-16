#include <kumo/facade/director.h>

// Private kumo_agent header (engine/agent/src), same access as engine_runtime.cpp.
#include "base64.h"

#include <kumo/core/log.h>

#include <nlohmann/json.hpp>

#include <format>
#include <utility>

namespace kumo::facade {

namespace {

using nlohmann::json;

constexpr int kMaxSpecChars = 4000;
constexpr int kSpecTemplateCount = 2;

std::string truncated(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
        text += "…";
    }
    return text;
}

} // namespace

std::expected<CriticVerdict, std::string> parseCriticVerdict(std::string_view text) {
    // parseSceneSpec owns the balanced-object scan; reuse it indirectly by
    // scanning here the same way is overkill -- the verdict is small, so a
    // plain find-first '{' / last '}' window parsed leniently suffices only
    // when balanced; use the strict scan via json::parse over that window.
    const std::size_t begin = text.find('{');
    const std::size_t end = text.rfind('}');
    if (begin == std::string_view::npos || end == std::string_view::npos || end < begin) {
        return std::unexpected("no JSON object found in the critic reply");
    }
    json parsed = json::parse(text.substr(begin, end - begin + 1), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        // Fences with trailing prose can break the naive window; retry on the
        // first balanced object only.
        parsed = json::parse(text.substr(begin), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            return std::unexpected("the critic verdict is not valid JSON");
        }
    }
    const auto verdictIt = parsed.find("verdict");
    if (verdictIt == parsed.end() || !verdictIt->is_string()) {
        return std::unexpected("the critic verdict has no 'verdict' field");
    }
    CriticVerdict verdict;
    verdict.raw = parsed.dump(-1, ' ', false, json::error_handler_t::replace);
    verdict.pass = verdictIt->get<std::string>() == "pass";
    if (const auto it = parsed.find("score"); it != parsed.end() && it->is_number()) {
        verdict.score = it->get<double>();
    }
    if (const auto it = parsed.find("praise"); it != parsed.end() && it->is_string()) {
        verdict.praise = it->get<std::string>();
    }
    if (const auto it = parsed.find("issues"); it != parsed.end() && it->is_array()) {
        for (const json& issue : *it) {
            if (!issue.is_object()) {
                continue;
            }
            CriticIssue out;
            for (const auto& [key, member] :
                 std::initializer_list<std::pair<const char*, std::string CriticIssue::*>>{
                     {"target", &CriticIssue::target},
                     {"severity", &CriticIssue::severity},
                     {"note", &CriticIssue::note}}) {
                if (const auto fieldIt = issue.find(key);
                    fieldIt != issue.end() && fieldIt->is_string()) {
                    out.*member = fieldIt->get<std::string>();
                }
            }
            if (!out.note.empty()) {
                verdict.issues.push_back(std::move(out));
            }
        }
    }
    return verdict;
}

Director::Director(Hooks hooks) : hooks_(std::move(hooks)) {}

bool Director::start(std::string brief, Tier tier) {
    if (active() || hooks_.scene == nullptr || brief.empty()) {
        return false;
    }
    state_ = State::Idle;
    brief_ = std::move(brief);
    tier_ = tier;
    spec_ = {};
    haveSpec_ = false;
    verdict_ = {};
    critiqueRound_ = 0;
    repairLoopsLeft_ = tier == Tier::Standard ? 1 : 3;
    specRetriesLeft_ = 1;
    cancelRequested_ = false;
    repairMaterialsQueued_ = false;
    repairBuildText_.clear();
    repairMaterialsText_.clear();
    pending_ = {};

    if (hooks_.director != nullptr) {
        enterStage(State::Directing);
        beginTurn(hooks_.director, directorMessage(), {}, {}, 1);
    } else {
        pushEvent({.kind = Event::Kind::Note,
                   .stage = State::Directing,
                   .text = "no director configured; building directly from the brief"});
        enterStage(State::Building);
        beginTurn(hooks_.scene,
                  std::format("Build this scene: {}\nWhen done, run scene_validate, fix "
                              "warnings, and reply with a one-line summary. Do not take "
                              "screenshots; a separate critic reviews the result.",
                              brief_),
                  {}, {}, 1);
    }
    return true;
}

void Director::cancel() {
    if (active()) {
        cancelRequested_ = true;
    }
}

bool Director::active() const {
    return state_ != State::Idle && state_ != State::Done && state_ != State::Failed &&
           state_ != State::Cancelled;
}

std::vector<Director::Event> Director::drainEvents() {
    return std::exchange(events_, {});
}

const char* Director::stateName(State state) {
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Directing:
        return "directing";
    case State::Building:
        return "building";
    case State::Materials:
        return "materials";
    case State::Critique:
        return "critique";
    case State::RepairBuild:
        return "repair_build";
    case State::RepairMaterials:
        return "repair_materials";
    case State::Done:
        return "done";
    case State::Failed:
        return "failed";
    case State::Cancelled:
        return "cancelled";
    }
    return "idle";
}

void Director::enterStage(State state) {
    state_ = state;
    pushEvent({.kind = Event::Kind::Stage, .stage = state, .text = stateName(state)});
}

void Director::beginTurn(agent::AgentSession* session, std::string message,
                         std::vector<agent::UserImage> images, std::string imageDetail,
                         int parseRetries) {
    pending_ = {};
    pending_.session = session;
    pending_.message = std::move(message);
    pending_.images = std::move(images);
    pending_.imageDetail = std::move(imageDetail);
    pending_.parseRetriesLeft = parseRetries;
}

bool Director::pollTurn(std::string& outText) {
    if (pending_.session == nullptr) {
        return false;
    }
    if (!pending_.submitted) {
        pending_.expected = pending_.session->completedTurns() + 1;
        if (!pending_.session->submit(pending_.message, pending_.images, pending_.imageDetail)) {
            return false; // session busy with a foreign turn; retry next tick
        }
        pending_.submitted = true;
        return false;
    }
    if (pending_.session->busy()) {
        return false;
    }
    const std::size_t completed = pending_.session->completedTurns();
    if (completed < pending_.expected) {
        return false;
    }
    if (completed > pending_.expected) {
        // Something injected extra turns mid-pipeline: resubmit so the stage
        // instruction is what the model acts on last.
        pushEvent({.kind = Event::Kind::Note,
                   .stage = state_,
                   .text = "external message detected mid-stage; resubmitting the stage "
                           "instruction"});
        pending_.submitted = false;
        return false;
    }
    outText = pending_.session->lastAssistantText();
    return true;
}

void Director::tick() {
    if (!active()) {
        return;
    }
    std::string reply;
    if (pollTurn(reply)) {
        agent::AgentSession* const finishedSession = pending_.session;
        const int retriesLeft = pending_.parseRetriesLeft;
        pending_ = {};
        // An empty reply (provider error, tool-round exhaustion) gets one
        // resubmission before failing the pipeline.
        if (reply.empty()) {
            if (retriesLeft > 0) {
                pushEvent({.kind = Event::Kind::Note,
                           .stage = state_,
                           .text = "stage produced no reply; retrying once"});
                beginTurn(finishedSession,
                          "Your previous turn produced no reply. Please continue and finish "
                          "the instruction above.",
                          {}, {}, 0);
                return;
            }
            fail("stage produced no reply after a retry");
            return;
        }
        onStageReply(reply);
    }
}

void Director::onStageReply(const std::string& text) {
    if (cancelRequested_) {
        finish(State::Cancelled);
        return;
    }
    switch (state_) {
    case State::Directing: {
        auto spec = parseSceneSpec(text);
        if (!spec.has_value()) {
            // One corrective round: models occasionally wrap or clip the JSON.
            if (specRetriesLeft_ <= 0) {
                fail(std::format("unusable scene plan: {}", spec.error()));
                return;
            }
            --specRetriesLeft_;
            pushEvent({.kind = Event::Kind::Note,
                       .stage = state_,
                       .text = std::format("plan rejected ({}); asking for a corrected one",
                                           spec.error())});
            beginTurn(hooks_.director,
                      std::format("That reply could not be used: {}. Reply again with ONLY "
                                  "the scene-plan JSON object, no prose, no code fences.",
                                  spec.error()),
                      {}, {}, 0);
            return;
        }
        spec_ = std::move(*spec);
        haveSpec_ = true;
        if (hooks_.storeSpec) {
            hooks_.storeSpec(spec_.raw);
        }
        pushEvent({.kind = Event::Kind::Note,
                   .stage = state_,
                   .text = std::format("plan accepted: {} elements, style '{}'",
                                       spec_.elements.size(), spec_.styleTier)});
        enterStage(State::Building);
        beginTurn(hooks_.scene,
                  sliceSpec(spec_, SpecStage::Build) +
                      "\nWhen done, run scene_validate, fix warnings, and reply with a "
                      "one-line summary. Do not take screenshots; a separate critic reviews "
                      "the result.",
                  {}, {}, 1);
        return;
    }
    case State::Building: {
        if (hooks_.shader != nullptr) {
            enterStage(State::Materials);
            const std::string slice =
                haveSpec_ ? sliceSpec(spec_, SpecStage::Materials)
                          : std::format("Improve the materials of the scene just built for: "
                                        "{}\nUse scene_list to find entities.",
                                        brief_);
            beginTurn(hooks_.shader, slice + "\nReply with a one-line summary when done.", {}, {},
                      1);
            return;
        }
        startCritique();
        return;
    }
    case State::Materials:
        startCritique();
        return;
    case State::Critique: {
        auto verdict = parseCriticVerdict(text);
        if (!verdict.has_value()) {
            if (verdictRetriesLeft_ <= 0) {
                // A critic that cannot produce a verdict must not sink the
                // built scene: deliver as-is.
                pushEvent({.kind = Event::Kind::Note,
                           .stage = state_,
                           .text = "unusable verdict after a retry; delivering as-is"});
                finish(State::Done);
                return;
            }
            --verdictRetriesLeft_;
            pushEvent({.kind = Event::Kind::Note,
                       .stage = state_,
                       .text = std::format("verdict rejected ({}); asking for a corrected one",
                                           verdict.error())});
            beginTurn(hooks_.critic,
                      std::format("That reply could not be used: {}. Reply again with ONLY "
                                  "the verdict JSON object.",
                                  verdict.error()),
                      {}, {}, 0);
            return;
        }
        verdict_ = std::move(*verdict);
        pushEvent({.kind = Event::Kind::Verdict, .stage = state_, .text = verdict_.raw});
        routeVerdict(verdict_);
        return;
    }
    case State::RepairBuild:
        if (repairMaterialsQueued_ && hooks_.shader != nullptr) {
            repairMaterialsQueued_ = false;
            enterStage(State::RepairMaterials);
            beginTurn(hooks_.shader, repairMaterialsText_, {}, {}, 1);
            return;
        }
        startCritique();
        return;
    case State::RepairMaterials:
        startCritique();
        return;
    default:
        return;
    }
}

void Director::routeVerdict(const CriticVerdict& verdict) {
    if (verdict.pass) {
        finish(State::Done);
        return;
    }
    if (repairLoopsLeft_ <= 0) {
        pushEvent({.kind = Event::Kind::Note,
                   .stage = state_,
                   .text = "repair budget exhausted; delivering as-is"});
        finish(State::Done);
        return;
    }
    repairBuildText_.clear();
    repairMaterialsText_.clear();
    std::string buildIssues;
    std::string materialIssues;
    for (const CriticIssue& issue : verdict.issues) {
        std::string& bucket =
            issue.target == "materials" && hooks_.shader != nullptr ? materialIssues : buildIssues;
        bucket += std::format("- [{}] {}\n", issue.severity.empty() ? "medium" : issue.severity,
                              issue.note);
    }
    if (buildIssues.empty() && materialIssues.empty()) {
        finish(State::Done);
        return;
    }
    --repairLoopsLeft_;
    if (!buildIssues.empty()) {
        repairBuildText_ = "The critic reviewed the scene and requests fixes (layout, "
                           "lighting and camera included):\n" +
                           buildIssues +
                           "Fix these, re-run scene_validate, then reply with a "
                           "one-line summary.";
    }
    if (!materialIssues.empty()) {
        repairMaterialsText_ = "The critic reviewed the scene and requests material fixes:\n" +
                               materialIssues + "Fix these, then reply with a one-line summary.";
    }
    if (!repairBuildText_.empty()) {
        repairMaterialsQueued_ = !repairMaterialsText_.empty();
        enterStage(State::RepairBuild);
        beginTurn(hooks_.scene, repairBuildText_, {}, {}, 1);
        return;
    }
    enterStage(State::RepairMaterials);
    beginTurn(hooks_.shader, repairMaterialsText_, {}, {}, 1);
}

void Director::startCritique() {
    if (cancelRequested_) {
        finish(State::Cancelled);
        return;
    }
    if (hooks_.critic == nullptr) {
        finish(State::Done);
        return;
    }
    ++critiqueRound_;
    verdictRetriesLeft_ = 1;
    enterStage(State::Critique);

    // Round table (MC-6): a cheap establishing look first, the diagnostic
    // multi-view at full size on every re-check.
    const bool firstRound = critiqueRound_ == 1;
    const char* viewsJson = firstRound ? R"(["main"])" : R"(["main","clay","normal"])";
    const int longSide = firstRound ? 640 : 1024;
    const std::string detail = firstRound ? "low" : "high";

    std::vector<agent::UserImage> images;
    std::vector<std::string> paths;
    if (hooks_.screenshot) {
        const std::string result = hooks_.screenshot(std::format(
            R"({{"views":{},"long_side":{},"detail":"{}"}})", viewsJson, longSide, detail));
        const json parsed = json::parse(result, nullptr, false);
        if (parsed.is_object() && parsed.value("status", "") == "ok" &&
            parsed.contains("image_paths")) {
            for (const json& path : parsed["image_paths"]) {
                if (!path.is_string()) {
                    continue;
                }
                if (auto encoded = agent::detail::base64EncodeFile(path.get<std::string>())) {
                    images.push_back(agent::UserImage{.base64 = std::move(*encoded)});
                    paths.push_back(path.get<std::string>());
                }
            }
        }
    }
    if (!paths.empty()) {
        pushEvent({.kind = Event::Kind::Screenshots,
                   .stage = state_,
                   .text = std::format("round {} views", critiqueRound_),
                   .imagePaths = paths});
    }
    beginTurn(hooks_.critic, critiqueMessage(), std::move(images), detail, 1);
}

void Director::fail(std::string reason) {
    pushEvent({.kind = Event::Kind::Error, .stage = state_, .text = std::move(reason)});
    finish(State::Failed);
}

void Director::finish(State terminal) {
    state_ = terminal;
    pending_ = {};
    pushEvent({.kind = Event::Kind::Stage, .stage = terminal, .text = stateName(terminal)});
}

void Director::pushEvent(Event event) {
    events_.push_back(std::move(event));
}

std::string Director::directorMessage() const {
    std::string out = std::format(
        "Creative brief: {}\n\n"
        "Plan a single 3D scene for this brief. Reply with ONLY one JSON object (no prose, "
        "no code fences) with these keys:\n"
        "style_tier: \"realistic\" or \"stylized\" -- pick one, never mix;\n"
        "palette: 3-5 hex colors;\n"
        "camera: {{\"fov_y_deg\", \"elevation_deg\", \"notes\"}};\n"
        "assets: library asset names to use (ONLY names from the library snapshot below);\n"
        "elements: 4-10 of {{\"name\", \"build\" (what to construct, placement, scale — "
        "reference concrete library asset names from the snapshot below; props must come "
        "from the library, primitives only for architecture), "
        "\"material_intent\" (surface look)}};\n"
        "lighting: {{\"environment\", \"key\", \"fill\", \"rim\"}};\n"
        "banned: things that must not appear;\n"
        "budgets: {{\"entities\", \"lights\"}} -- stay modest (<=80 entities, <=6 lights).\n",
        brief_);
    if (hooks_.assetList) {
        const std::string assets = hooks_.assetList();
        if (!assets.empty()) {
            out += std::format("\nAsset library snapshot:\n{}\n", truncated(assets, kMaxSpecChars));
        }
    }
    if (hooks_.specTemplates) {
        const std::vector<std::string> references =
            hooks_.specTemplates(brief_, kSpecTemplateCount);
        for (const std::string& reference : references) {
            out += std::format("\nReference plan (tone only -- never copy it wholesale):\n{}\n",
                               truncated(reference, kMaxSpecChars / 2));
        }
    }
    return out;
}

std::string Director::critiqueMessage() const {
    std::string out = std::format("You are reviewing round {} of a scene built for the brief: {}\n",
                                  critiqueRound_, brief_);
    if (haveSpec_) {
        out += std::format("The director's plan was:\n{}\n", truncated(spec_.raw, kMaxSpecChars));
    }
    out += "Judge the attached screenshots against six points: composition, focal point, "
           "layering, materials, lighting, detail. Props visibly cobbled from bare "
           "primitives count as a build issue — real library models should stand in. "
           "Clay/normal views (when present) reveal shape and shading problems the lit "
           "view hides.\n"
           "Reply with ONLY one JSON object: {\"verdict\": \"pass\"|\"revise\", \"score\": "
           "0-10, \"issues\": [{\"target\": \"build\"|\"materials\", \"severity\": "
           "\"low\"|\"medium\"|\"high\", \"note\": \"...\"}], \"praise\": \"...\"}. "
           "Lighting and camera issues take target \"build\". Pass when the scene serves "
           "the brief well; do not chase perfection.";
    return out;
}

} // namespace kumo::facade
