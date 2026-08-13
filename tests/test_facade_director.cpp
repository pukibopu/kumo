#include <doctest/doctest.h>

#include <kumo/agent/fake_provider.h>
#include <kumo/agent/session.h>
#include <kumo/agent/tool_registry.h>
#include <kumo/core/main_thread_queue.h>
#include <kumo/facade/director.h>
#include <kumo/facade/scene_spec.h>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace kumo;
using namespace kumo::facade;

namespace {

agent::ChatMessage reply(std::string text) {
    agent::ChatMessage message;
    message.role = agent::Role::Assistant;
    message.text = std::move(text);
    message.stopReason = agent::StopReason::EndTurn;
    return message;
}

constexpr const char* kPlanJson = R"({
  "style_tier": "realistic",
  "palette": ["#223344", "#aabbcc"],
  "camera": {"fov_y_deg": 45},
  "assets": ["asphalt"],
  "elements": [
    {"name": "street", "build": "a long asphalt plane", "material_intent": "wet asphalt"},
    {"name": "lamp", "build": "a pole with a glowing head", "material_intent": "emissive glass"}
  ],
  "lighting": {"key": "cool moonlight"},
  "banned": ["daylight"],
  "budgets": {"entities": 40, "lights": 4}
})";

constexpr const char* kPassVerdict =
    R"({"verdict": "pass", "score": 8.5, "issues": [], "praise": "reads well"})";

constexpr const char* kReviseVerdict = R"({
  "verdict": "revise", "score": 5,
  "issues": [
    {"target": "build", "severity": "high", "note": "camera too high"},
    {"target": "materials", "severity": "medium", "note": "street looks dry"}
  ]})";

// One fake-backed session per role; sessions the test leaves null degrade the
// pipeline exactly like an unconfigured role.
struct Rig {
    MainThreadQueue queue;
    agent::ToolRegistry registry; // deliberately empty
    std::optional<agent::FakeProvider> sceneProvider, shaderProvider, directorProvider,
        criticProvider;
    std::optional<agent::AgentSession> scene, shader, director, critic;
    std::vector<std::string> storedSpecs;
    int assetListCalls = 0;
    int templateCalls = 0;

    agent::AgentSession& make(std::optional<agent::FakeProvider>& provider,
                              std::optional<agent::AgentSession>& session,
                              std::vector<agent::ChatMessage> script) {
        provider.emplace(std::move(script), "script exhausted");
        session.emplace(*provider, registry, queue, nullptr,
                        agent::AgentSession::Desc{.model = "fake"});
        return *session;
    }

    Director::Hooks hooks() {
        Director::Hooks out;
        out.scene = scene.has_value() ? &*scene : nullptr;
        out.shader = shader.has_value() ? &*shader : nullptr;
        out.director = director.has_value() ? &*director : nullptr;
        out.critic = critic.has_value() ? &*critic : nullptr;
        out.assetList = [this] {
            ++assetListCalls;
            return std::string(R"({"textures":[{"name":"asphalt"}]})");
        };
        out.specTemplates = [this](std::string_view, int) {
            ++templateCalls;
            return std::vector<std::string>{"{\"caption\": \"night reference\"}"};
        };
        out.storeSpec = [this](const std::string& raw) { storedSpecs.push_back(raw); };
        return out;
    }

    // Ticks until a terminal state; the sessions run real worker threads.
    Director::State run(Director& pipeline, int maxMillis = 4000) {
        for (int i = 0; i < maxMillis && pipeline.active(); ++i) {
            queue.drain();
            pipeline.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pipeline.state();
    }
};

bool hasEventText(const std::vector<Director::Event>& events, std::string_view needle) {
    for (const Director::Event& event : events) {
        if (event.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("parseSceneSpec strips fences and prose, and reads every field") {
    const std::string wrapped =
        std::string("Here is the plan:\n```json\n") + kPlanJson + "\n```\nGood luck!";
    const auto spec = parseSceneSpec(wrapped);
    REQUIRE(spec.has_value());
    CHECK(spec->styleTier == "realistic");
    CHECK(spec->palette.size() == 2);
    CHECK(spec->assets == std::vector<std::string>{"asphalt"});
    REQUIRE(spec->elements.size() == 2);
    CHECK(spec->elements[0].name == "street");
    CHECK(spec->elements[0].build == "a long asphalt plane");
    CHECK(spec->elements[1].materialIntent == "emissive glass");
    CHECK(!spec->cameraJson.empty());
    CHECK(!spec->lightingJson.empty());
    CHECK(spec->banned == std::vector<std::string>{"daylight"});
    CHECK(!spec->budgetsJson.empty());
    CHECK(spec->raw.front() == '{');
}

TEST_CASE("parseSceneSpec survives braces inside strings and rejects unusable plans") {
    const auto braces = parseSceneSpec(R"(note {"elements":[{"name":"a{b}c","build":"x"}]} end)");
    REQUIRE(braces.has_value());
    CHECK(braces->elements[0].name == "a{b}c");
    CHECK(!parseSceneSpec("no json here").has_value());
    CHECK(!parseSceneSpec(R"({"style_tier": "realistic"})").has_value()); // nothing actionable
    CHECK(!parseSceneSpec(R"({{{)").has_value());
}

TEST_CASE("sliceSpec routes build and material intents to their stages") {
    const auto spec = parseSceneSpec(kPlanJson);
    REQUIRE(spec.has_value());
    const std::string build = sliceSpec(*spec, SpecStage::Build);
    CHECK(build.find("a long asphalt plane") != std::string::npos);
    CHECK(build.find("cool moonlight") != std::string::npos);
    CHECK(build.find("asphalt") != std::string::npos);
    CHECK(build.find("wet asphalt") == std::string::npos); // material intents stay out
    const std::string materials = sliceSpec(*spec, SpecStage::Materials);
    CHECK(materials.find("wet asphalt") != std::string::npos);
    CHECK(materials.find("emissive glass") != std::string::npos);
    CHECK(materials.find("a long asphalt plane") == std::string::npos);
    CHECK(materials.find("realistic") != std::string::npos); // style goes to both
}

TEST_CASE("parseCriticVerdict reads verdicts and rejects malformed ones") {
    const auto pass = parseCriticVerdict(std::string("Verdict:\n") + kPassVerdict);
    REQUIRE(pass.has_value());
    CHECK(pass->pass);
    CHECK(pass->score == doctest::Approx(8.5));
    CHECK(pass->praise == "reads well");
    const auto revise = parseCriticVerdict(kReviseVerdict);
    REQUIRE(revise.has_value());
    CHECK(!revise->pass);
    REQUIRE(revise->issues.size() == 2);
    CHECK(revise->issues[0].target == "build");
    CHECK(revise->issues[1].severity == "medium");
    CHECK(!parseCriticVerdict("nothing").has_value());
    CHECK(!parseCriticVerdict(R"({"score": 5})").has_value());
}

TEST_CASE("director pipeline happy path: plan, build, materials, critique pass") {
    Rig rig;
    rig.make(rig.directorProvider, rig.director, {reply(kPlanJson)});
    rig.make(rig.sceneProvider, rig.scene, {reply("built the street scene")});
    rig.make(rig.shaderProvider, rig.shader, {reply("materials applied")});
    rig.make(rig.criticProvider, rig.critic, {reply(kPassVerdict)});

    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("a rainy night street", Director::Tier::Standard));
    CHECK(!pipeline.start("again", Director::Tier::Standard)); // already running
    CHECK(rig.run(pipeline) == Director::State::Done);

    REQUIRE(rig.storedSpecs.size() == 1);
    CHECK(rig.storedSpecs[0].find("street") != std::string::npos);
    CHECK(rig.assetListCalls == 1);
    CHECK(rig.templateCalls == 1);
    CHECK(pipeline.spec().elements.size() == 2);

    // The build stage received the plan slice, the material stage its intents.
    const auto& sceneRequests = rig.sceneProvider->requests();
    REQUIRE(sceneRequests.size() == 1);
    CHECK(sceneRequests[0].messages.back().text.find("a long asphalt plane") != std::string::npos);
    const auto& shaderRequests = rig.shaderProvider->requests();
    REQUIRE(shaderRequests.size() == 1);
    CHECK(shaderRequests[0].messages.back().text.find("wet asphalt") != std::string::npos);
    // The director saw the asset snapshot and the tone reference.
    const auto& directorRequests = rig.directorProvider->requests();
    REQUIRE(directorRequests.size() == 1);
    CHECK(directorRequests[0].messages.back().text.find("asphalt") != std::string::npos);
    CHECK(directorRequests[0].messages.back().text.find("night reference") != std::string::npos);

    const std::vector<Director::Event> events = pipeline.drainEvents();
    CHECK(hasEventText(events, "plan accepted"));
    CHECK(hasEventText(events, "done"));
}

TEST_CASE("director retries once on an unusable plan, then fails on a second one") {
    Rig rig;
    rig.make(rig.directorProvider, rig.director, {reply("sorry, no plan"), reply(kPlanJson)});
    rig.make(rig.sceneProvider, rig.scene, {reply("built")});

    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("brief", Director::Tier::Standard));
    CHECK(rig.run(pipeline) == Director::State::Done); // no shader/critic: done after build
    CHECK(hasEventText(pipeline.drainEvents(), "plan rejected"));

    Rig failing;
    failing.make(failing.directorProvider, failing.director,
                 {reply("garbage"), reply("more garbage")});
    failing.make(failing.sceneProvider, failing.scene, {});
    Director doomed(failing.hooks());
    REQUIRE(doomed.start("brief", Director::Tier::Standard));
    CHECK(failing.run(doomed) == Director::State::Failed);
}

TEST_CASE("director degrades to a direct build without a director session") {
    Rig rig;
    rig.make(rig.sceneProvider, rig.scene, {reply("built directly")});
    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("a cozy interior corner", Director::Tier::Standard));
    CHECK(rig.run(pipeline) == Director::State::Done);
    const auto& requests = rig.sceneProvider->requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].messages.back().text.find("a cozy interior corner") != std::string::npos);
    CHECK(rig.storedSpecs.empty());
}

TEST_CASE("a revise verdict routes issues to repair stages, then re-critiques") {
    Rig rig;
    rig.make(rig.directorProvider, rig.director, {reply(kPlanJson)});
    rig.make(rig.sceneProvider, rig.scene,
             {reply("built"), reply("camera fixed")}); // build + repair_build
    rig.make(rig.shaderProvider, rig.shader,
             {reply("materials applied"), reply("street wetted")}); // materials + repair
    rig.make(rig.criticProvider, rig.critic, {reply(kReviseVerdict), reply(kPassVerdict)});

    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("brief", Director::Tier::Standard));
    CHECK(rig.run(pipeline) == Director::State::Done);

    const auto& sceneRequests = rig.sceneProvider->requests();
    REQUIRE(sceneRequests.size() == 2);
    CHECK(sceneRequests[1].messages.back().text.find("camera too high") != std::string::npos);
    const auto& shaderRequests = rig.shaderProvider->requests();
    REQUIRE(shaderRequests.size() == 2);
    CHECK(shaderRequests[1].messages.back().text.find("street looks dry") != std::string::npos);
    // Two critique rounds ran.
    REQUIRE(rig.criticProvider->requests().size() == 2);
}

TEST_CASE("the standard tier's repair budget caps revise loops") {
    Rig rig;
    rig.make(rig.directorProvider, rig.director, {reply(kPlanJson)});
    rig.make(rig.sceneProvider, rig.scene, {reply("built"), reply("fixed")});
    // Every verdict says revise; budget 1 means exactly two critiques run.
    rig.make(rig.criticProvider, rig.critic,
             {reply(kReviseVerdict), reply(kReviseVerdict), reply(kReviseVerdict)});

    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("brief", Director::Tier::Standard));
    CHECK(rig.run(pipeline) == Director::State::Done);
    CHECK(rig.criticProvider->requests().size() == 2);
    CHECK(hasEventText(pipeline.drainEvents(), "budget exhausted"));
}

TEST_CASE("an external mid-stage injection triggers a resubmission") {
    Rig rig;
    rig.make(rig.directorProvider, rig.director, {reply(kPlanJson)});
    // Script: the pipeline's first build turn (finishes unobserved), the
    // injected foreign turn, then the resubmitted build turn.
    rig.make(
        rig.sceneProvider, rig.scene,
        {reply("built but unobserved"), reply("foreign reply"), reply("built after resubmit")});

    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("brief", Director::Tier::Standard));
    // Reach Building with its turn still unsubmitted (the transition tick
    // only queues it; submission happens on the next tick).
    for (int i = 0; i < 4000 && pipeline.state() != Director::State::Building; ++i) {
        pipeline.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(pipeline.state() == Director::State::Building);
    pipeline.tick(); // submits the build turn
    while (rig.scene->busy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // The build turn completed but the pipeline has not polled it yet; a
    // foreign turn slipping in now leaves completedTurns beyond expected.
    REQUIRE(rig.scene->submit("user butts in"));
    while (rig.scene->busy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(rig.run(pipeline) == Director::State::Done);
    CHECK(rig.sceneProvider->requests().size() == 3);
    CHECK(hasEventText(pipeline.drainEvents(), "resubmitting"));
}

TEST_CASE("cancel stops the pipeline at the next stage boundary") {
    Rig rig;
    rig.make(rig.directorProvider, rig.director, {reply(kPlanJson)});
    rig.make(rig.sceneProvider, rig.scene, {reply("built")});
    Director pipeline(rig.hooks());
    REQUIRE(pipeline.start("brief", Director::Tier::Standard));
    pipeline.cancel();
    CHECK(rig.run(pipeline) == Director::State::Cancelled);
}
