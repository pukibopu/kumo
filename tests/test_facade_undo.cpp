#include <doctest/doctest.h>

#include <kumo/asset/procedural_sky.h>
#include <kumo/facade/undo_stack.h>

#include <vector>

using namespace kumo;
using namespace kumo::facade;
using MaterialTextureIndices = renderer::ForwardRenderer::MaterialTextureIndices;

namespace {

// A minimal SceneState fixture: varying camera.position.x is enough to
// distinguish snapshots without a GPU renderer or loaded scene.
struct Fixture {
    SceneState current;
    UndoStack stack;

    Fixture() : stack([this] { return current; }, [this](const SceneState& s) { current = s; }) {}

    float x() const { return current.world.camera.position.x; }
    void setX(float v) { current.world.camera.position.x = v; }

    // beginPending + commitPending, the common case where the change is
    // known to have happened.
    void commitChange(const char* label, float newX) {
        stack.beginPending(label);
        setX(newX);
        stack.commitPending();
    }
};

} // namespace

TEST_CASE("UndoStack: begin+commit records the pre-change snapshot; undo/redo move across it") {
    Fixture f;
    f.setX(1.0f);
    f.commitChange("move", 2.0f);

    CHECK(f.stack.canUndo());
    CHECK(!f.stack.canRedo());

    REQUIRE(f.stack.undo());
    CHECK(f.x() == doctest::Approx(1.0f));
    CHECK(!f.stack.canUndo());
    CHECK(f.stack.canRedo());

    REQUIRE(f.stack.redo());
    CHECK(f.x() == doctest::Approx(2.0f));
    CHECK(f.stack.canUndo());
    CHECK(!f.stack.canRedo());
}

TEST_CASE("UndoStack: beginPending captures immediately; commitPending is what makes it a step") {
    Fixture f;
    f.setX(1.0f);
    f.stack.beginPending("move");
    CHECK(f.stack.hasPending());
    CHECK(!f.stack.canUndo()); // not a step yet

    f.setX(2.0f);
    f.stack.commitPending();
    CHECK(!f.stack.hasPending());
    REQUIRE(f.stack.canUndo());

    REQUIRE(f.stack.undo());
    CHECK(f.x() == doctest::Approx(1.0f)); // captured at beginPending, before the mutation
}

TEST_CASE("UndoStack: commitPending without a pending snapshot is a no-op") {
    Fixture f;
    f.setX(5.0f);
    f.stack.commitPending();
    CHECK(!f.stack.hasPending());
    CHECK(!f.stack.canUndo());
    CHECK(f.x() == doctest::Approx(5.0f));
}

TEST_CASE("UndoStack: begin+discard produces no undo step and leaves current state untouched") {
    Fixture f;
    f.setX(1.0f);
    f.stack.beginPending("move");
    f.setX(2.0f);

    f.stack.discardPending();
    CHECK(!f.stack.hasPending());
    CHECK(!f.stack.canUndo());
    CHECK(f.x() == doctest::Approx(2.0f)); // discardPending never touches current state
}

TEST_CASE("UndoStack: a new beginPending discards a stale, uncommitted pending snapshot") {
    Fixture f;
    f.setX(1.0f);
    f.stack.beginPending("abandoned"); // would capture x=1 if committed
    f.setX(2.0f);

    f.stack.beginPending("kept"); // supersedes "abandoned" without committing it
    f.setX(3.0f);
    f.stack.commitPending();

    CHECK(!f.stack.canRedo());
    REQUIRE(f.stack.undoLabel() != nullptr);
    CHECK(*f.stack.undoLabel() == "kept");
    REQUIRE(f.stack.undo());
    CHECK(f.x() == doctest::Approx(2.0f)); // the "kept" capture, not the "abandoned" one
    CHECK(!f.stack.canUndo());
}

TEST_CASE("UndoStack: undo() discards a pending snapshot before undoing") {
    Fixture f;
    f.setX(1.0f);
    f.commitChange("committed", 2.0f); // undo stack: [committed] snapshot x=1

    f.setX(3.0f);
    f.stack.beginPending("abandoned"); // captures x=3, never committed
    f.setX(4.0f);

    REQUIRE(f.stack.undo());
    CHECK(!f.stack.hasPending());
    CHECK(f.x() == doctest::Approx(1.0f)); // the committed snapshot, not the abandoned x=3
    CHECK(!f.stack.canUndo());
    REQUIRE(f.stack.canRedo());
    CHECK(*f.stack.redoLabel() == "committed");
}

TEST_CASE("UndoStack: redo() discards a pending snapshot before redoing") {
    Fixture f;
    f.setX(1.0f);
    f.commitChange("committed", 2.0f);
    REQUIRE(f.stack.undo());
    CHECK(f.stack.canRedo());

    f.stack.beginPending("abandoned");
    CHECK(f.stack.hasPending());

    REQUIRE(f.stack.redo());
    CHECK(!f.stack.hasPending());
    CHECK(f.x() == doctest::Approx(2.0f));
    CHECK(!f.stack.canRedo());
}

TEST_CASE("UndoStack: undo()/redo() on an empty stack fail without side effects") {
    Fixture f;
    f.setX(5.0f);
    CHECK(!f.stack.undo());
    CHECK(f.x() == doctest::Approx(5.0f));
    CHECK(!f.stack.redo());
    CHECK(f.x() == doctest::Approx(5.0f));
}

TEST_CASE("UndoStack: labels track the top of each stack and move between them") {
    Fixture f;
    CHECK(f.stack.undoLabel() == nullptr);
    CHECK(f.stack.redoLabel() == nullptr);

    f.commitChange("first", 1.0f);
    f.commitChange("second", 2.0f);

    REQUIRE(f.stack.undoLabel() != nullptr);
    CHECK(*f.stack.undoLabel() == "second");

    REQUIRE(f.stack.undo());
    REQUIRE(f.stack.redoLabel() != nullptr);
    CHECK(*f.stack.redoLabel() == "second");
    REQUIRE(f.stack.undoLabel() != nullptr);
    CHECK(*f.stack.undoLabel() == "first");
}

TEST_CASE("UndoStack: depth caps the undo history, dropping the oldest entries") {
    SceneState current;
    UndoStack stack([&current] { return current; },
                    [&current](const SceneState& s) { current = s; },
                    /*depth=*/2);

    current.world.camera.position.x = 0.0f;
    stack.beginPending("r0");
    current.world.camera.position.x = 1.0f;
    stack.commitPending(); // snapshot x=0; undo depth: [r0]
    stack.beginPending("r1");
    current.world.camera.position.x = 2.0f;
    stack.commitPending(); // snapshot x=1; undo depth: [r0, r1]
    stack.beginPending("r2");
    current.world.camera.position.x = 3.0f;
    stack.commitPending(); // snapshot x=2; size 3 > depth 2, evicts r0: [r1, r2]

    REQUIRE(stack.undo()); // reverts the r2-labeled change: 3 -> 2
    CHECK(current.world.camera.position.x == doctest::Approx(2.0f));
    REQUIRE(stack.undo()); // reverts the r1-labeled change: 2 -> 1
    CHECK(current.world.camera.position.x == doctest::Approx(1.0f));
    // r0's snapshot (x=0) was evicted by the depth cap: nothing left to undo.
    CHECK(!stack.canUndo());
}

TEST_CASE("SceneState::environment equality gates whether applySceneState would re-bake") {
    // No GPU here: this only exercises the struct-level equality the facade
    // uses to decide whether undo/redo needs to re-bake an IBL environment.
    SceneState a;
    SceneState b;
    CHECK(a.environment == b.environment); // both nullopt: an unrelated edit's snapshot

    const EnvironmentSource sky{.file = "", .sky = asset::ProceduralSkyDesc{}};
    a.environment = sky;
    CHECK(a.environment != b.environment); // one has a procedural sky, the other the loaded HDR

    b.environment = sky;
    CHECK(a.environment == b.environment); // identical source: no re-bake needed

    EnvironmentSource changed = sky;
    changed.sky.sunIntensity = sky.sky.sunIntensity + 1.0f;
    b.environment = changed;
    CHECK(a.environment != b.environment); // same preset shape, different tuning: re-bake needed

    const EnvironmentSource file{.file = "sunset.hdr"};
    b.environment = file;
    CHECK(a.environment != b.environment); // a procedural sky vs. a named HDR file
}

TEST_CASE("MaterialTextureIndices equality distinguishes any differing slot") {
    MaterialTextureIndices a;
    MaterialTextureIndices b;
    CHECK(a == b); // both default-constructed (-1 in every slot)

    b.baseColor = 3;
    CHECK(a != b);

    b = a;
    b.emissive = 7;
    CHECK(a != b); // a slot other than baseColor also counts
}

TEST_CASE("SceneState::materialTextures round-trips through undo/redo like every other field") {
    // Struct-level only (ADR 0044 follow-up): no GPU renderer here, just the
    // field capture/apply plumbing SceneState/UndoStack already exercise for
    // materials/shaderSources above; EngineRuntime::captureSceneState/
    // applySceneState wire this field to renderer_.materialTextureIndices()/
    // setMaterialTextures() outside kumo_tests' reach.
    Fixture f;
    f.current.materialTextures = {MaterialTextureIndices{.baseColor = 1}};
    f.stack.beginPending("bind");
    f.current.materialTextures[0].baseColor = 2;
    f.stack.commitPending();

    REQUIRE(f.stack.undo());
    REQUIRE(f.current.materialTextures.size() == 1);
    CHECK(f.current.materialTextures[0].baseColor == 1);

    REQUIRE(f.stack.redo());
    CHECK(f.current.materialTextures[0].baseColor == 2);
}

TEST_CASE("UndoStack: redo mirrors undo, moving state forward and back to undo") {
    Fixture f;
    f.setX(1.0f);
    f.commitChange("a", 2.0f);
    f.commitChange("b", 3.0f);

    REQUIRE(f.stack.undo());
    CHECK(f.x() == doctest::Approx(2.0f));
    REQUIRE(f.stack.undo());
    CHECK(f.x() == doctest::Approx(1.0f));
    CHECK(!f.stack.canUndo());

    REQUIRE(f.stack.redo());
    CHECK(f.x() == doctest::Approx(2.0f));
    REQUIRE(f.stack.redo());
    CHECK(f.x() == doctest::Approx(3.0f));
    CHECK(!f.stack.canRedo());
}
