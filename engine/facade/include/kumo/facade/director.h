#pragma once

#include <kumo/agent/session.h>
#include <kumo/facade/scene_spec.h>

#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kumo::facade {

// One issue from the critic's verdict; `target` routes it to a repair stage
// ("build" covers layout, lighting and camera; "materials" goes to the
// shader session).
struct CriticIssue {
    std::string target;   // "build" | "materials"
    std::string severity; // "low" | "medium" | "high"
    std::string note;
};

struct CriticVerdict {
    bool pass = false;
    double score = 0.0; // 0-10
    std::vector<CriticIssue> issues;
    std::string praise;
    std::string raw;
};

// Lenient like parseSceneSpec: finds the first JSON object in a model reply.
std::expected<CriticVerdict, std::string> parseCriticVerdict(std::string_view text);

// Orchestrates the director pipeline (MC): Directing -> Building ->
// Materials -> Critique -> (RepairBuild -> RepairMaterials -> Critique) x N.
// Ticked from EngineRuntime::pump() on the main thread; the sessions run
// their own workers, so a tick only submits/polls and never blocks. Turn
// protocol: a stage records expected = completedTurns()+1 before submitting;
// finding MORE completed turns than expected means something external
// injected a message mid-pipeline, and the stage message is resubmitted so
// the model acts on the pipeline's instruction last.
class Director {
public:
    enum class State {
        Idle,
        Directing,
        Building,
        Materials,
        Critique,
        RepairBuild,
        RepairMaterials,
        Done,
        Failed,
        Cancelled
    };
    enum class Tier { Standard, Hero };

    struct Event {
        enum class Kind { Stage, Note, Screenshots, Verdict, Error };
        Kind kind = Kind::Note;
        State stage = State::Idle;
        std::string text;
        std::vector<std::string> imagePaths; // Screenshots only
    };

    // Injected by the composition root; `scene` is the only hard requirement.
    // Null `director` degrades to a direct single-agent build, null `shader`
    // skips the material stages, null `critic` skips critique (Done after
    // build/materials). All sessions outlive the Director.
    struct Hooks {
        agent::AgentSession* scene = nullptr;
        agent::AgentSession* shader = nullptr;
        agent::AgentSession* director = nullptr;
        agent::AgentSession* critic = nullptr;
        // The viewer_screenshot tool entry point (already main-thread); takes
        // its argsJson, returns its result JSON. Null skips screenshots (the
        // critic then judges from the transcript text alone).
        std::function<std::string(std::string_view argsJson)> screenshot;
        // asset_list result JSON for the director's context; null/empty omits.
        std::function<std::string()> assetList;
        // Top-k spec-template reference texts for a brief (MR spec entries);
        // null/empty omits.
        std::function<std::vector<std::string>(std::string_view brief, int k)> specTemplates;
        // Persists the accepted plan (EngineRuntime keeps it for saveScene).
        std::function<void(const std::string& rawSpec)> storeSpec;
    };

    explicit Director(Hooks hooks);

    // False when already running or no scene session; `brief` is the user's
    // one-line (or longer) creative request.
    bool start(std::string brief, Tier tier);
    // Soft cancel: the in-flight stage completes, then the pipeline stops.
    void cancel();
    void tick();

    State state() const { return state_; }
    // True from start() until a terminal state; EngineRuntime refuses session
    // reloads and the shell disables chat input while this holds.
    bool active() const;
    const SceneSpec& spec() const { return spec_; }
    std::vector<Event> drainEvents();

    static const char* stateName(State state);

private:
    struct PendingTurn {
        agent::AgentSession* session = nullptr;
        std::string message;
        std::vector<agent::UserImage> images;
        std::string imageDetail;
        std::size_t expected = 0;
        bool submitted = false;
        int parseRetriesLeft = 0;
    };

    void enterStage(State state);
    void beginTurn(agent::AgentSession* session, std::string message,
                   std::vector<agent::UserImage> images = {}, std::string imageDetail = {},
                   int parseRetries = 0);
    // Returns the finished turn's assistant text once per completion.
    bool pollTurn(std::string& outText);
    void onStageReply(const std::string& text);
    void fail(std::string reason);
    void finish(State terminal);
    void pushEvent(Event event);
    void startCritique();
    void routeVerdict(const CriticVerdict& verdict);
    std::string directorMessage() const;
    std::string critiqueMessage() const;

    Hooks hooks_;
    State state_ = State::Idle;
    Tier tier_ = Tier::Standard;
    std::string brief_;
    SceneSpec spec_;
    bool haveSpec_ = false;
    CriticVerdict verdict_;
    int critiqueRound_ = 0;      // 1-based once critique starts
    int repairLoopsLeft_ = 0;    // per tier
    int specRetriesLeft_ = 1;    // one corrective re-prompt for an unusable plan
    int verdictRetriesLeft_ = 1; // per critique round
    bool cancelRequested_ = false;
    bool repairMaterialsQueued_ = false;
    std::string repairBuildText_;
    std::string repairMaterialsText_;
    PendingTurn pending_;
    std::vector<Event> events_;
};

} // namespace kumo::facade
