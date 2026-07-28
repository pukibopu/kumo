#include <doctest/doctest.h>

#include <kumo/facade/undo_stack.h>

using namespace kumo;
using namespace kumo::facade;

namespace {

// A minimal SceneState fixture: varying camera.position.x is enough to
// distinguish snapshots without a GPU renderer or loaded scene.
struct Fixture {
    SceneState current;
    UndoStack stack;

    Fixture() : stack([this] { return current; }, [this](const SceneState& s) { current = s; }) {}

    float x() const { return current.world.camera.position.x; }
    void setX(float v) { current.world.camera.position.x = v; }
};

} // namespace

TEST_CASE("UndoStack: undo restores the pre-change snapshot, redo reapplies the change") {
    Fixture f;
    f.setX(1.0f);
    f.stack.recordBefore("move");
    f.setX(2.0f);

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

TEST_CASE("UndoStack: undo()/redo() on an empty stack fail without side effects") {
    Fixture f;
    f.setX(5.0f);
    CHECK(!f.stack.undo());
    CHECK(f.x() == doctest::Approx(5.0f));
    CHECK(!f.stack.redo());
    CHECK(f.x() == doctest::Approx(5.0f));
}

TEST_CASE("UndoStack: a new recordBefore clears the redo stack") {
    Fixture f;
    f.setX(1.0f);
    f.stack.recordBefore("a");
    f.setX(2.0f);
    REQUIRE(f.stack.undo());
    CHECK(f.stack.canRedo());

    f.setX(3.0f);
    f.stack.recordBefore("b");
    CHECK(!f.stack.canRedo());
}

TEST_CASE("UndoStack: labels track the top of each stack and move between them") {
    Fixture f;
    CHECK(f.stack.undoLabel() == nullptr);
    CHECK(f.stack.redoLabel() == nullptr);

    f.stack.recordBefore("first");
    f.setX(1.0f);
    f.stack.recordBefore("second");
    f.setX(2.0f);

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
    stack.recordBefore("r0"); // snapshot x=0; undo depth: [r0]
    current.world.camera.position.x = 1.0f;
    stack.recordBefore("r1"); // snapshot x=1; undo depth: [r0, r1]
    current.world.camera.position.x = 2.0f;
    stack.recordBefore("r2"); // snapshot x=2; size 3 > depth 2, evicts r0: [r1, r2]
    current.world.camera.position.x = 3.0f;

    REQUIRE(stack.undo()); // reverts the r2-labeled change: 3 -> 2
    CHECK(current.world.camera.position.x == doctest::Approx(2.0f));
    REQUIRE(stack.undo()); // reverts the r1-labeled change: 2 -> 1
    CHECK(current.world.camera.position.x == doctest::Approx(1.0f));
    // r0's snapshot (x=0) was evicted by the depth cap: nothing left to undo.
    CHECK(!stack.canUndo());
}

TEST_CASE("UndoStack: redo mirrors undo, moving state forward and back to undo") {
    Fixture f;
    f.setX(1.0f);
    f.stack.recordBefore("a");
    f.setX(2.0f);
    f.stack.recordBefore("b");
    f.setX(3.0f);

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
