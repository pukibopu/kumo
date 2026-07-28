#include <doctest/doctest.h>

#include <kumo/facade/frame_dirty.h>

using namespace kumo::facade;

TEST_CASE("FrameDirty: initial pending equals slots") {
    FrameDirty dirty(2);
    CHECK(dirty.consume());
    CHECK(dirty.consume());
    CHECK(!dirty.consume());
}

TEST_CASE("FrameDirty: consume drains to false and stays false") {
    FrameDirty dirty(1);
    CHECK(dirty.consume());
    CHECK(!dirty.consume());
    CHECK(!dirty.consume());
}

TEST_CASE("FrameDirty: mark refills pending back to slots") {
    FrameDirty dirty(2);
    CHECK(dirty.consume());
    CHECK(dirty.consume());
    CHECK(!dirty.consume());

    dirty.mark();
    CHECK(dirty.consume());
    CHECK(dirty.consume());
    CHECK(!dirty.consume());
}

TEST_CASE("FrameDirty: mark resets pending rather than accumulating on top of it") {
    FrameDirty dirty(2);
    CHECK(dirty.consume()); // pending: 1
    dirty.mark();           // back to 2, not 3
    CHECK(dirty.consume()); // pending: 1
    CHECK(dirty.consume()); // pending: 0
    CHECK(!dirty.consume());
}

TEST_CASE("FrameDirty: interleaved mark/consume keeps rendering while changes keep arriving") {
    FrameDirty dirty(2);
    CHECK(dirty.consume()); // pending: 1
    dirty.mark();           // a change lands mid-drain: back to 2
    CHECK(dirty.consume()); // pending: 1
    dirty.mark();           // another change lands: back to 2
    CHECK(dirty.consume()); // pending: 1
    CHECK(dirty.consume()); // pending: 0
    CHECK(!dirty.consume());
}

TEST_CASE("FrameDirty: zero slots never requests a render") {
    FrameDirty dirty(0);
    CHECK(!dirty.consume());
    dirty.mark();
    CHECK(!dirty.consume());
}
