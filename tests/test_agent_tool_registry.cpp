#include <doctest/doctest.h>

#include <kumo/agent/tool_registry.h>

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using namespace kumo::agent;
using nlohmann::json;

namespace {

ToolDef makeDef(const char* name) {
    return {.name = name, .description = "test tool", .parametersSchema = R"({"type":"object"})"};
}

json invokeParsed(const ToolRegistry& registry, const char* name, const char* args) {
    const json result = json::parse(registry.invoke(name, args), nullptr, false);
    REQUIRE(result.is_object());
    return result;
}

} // namespace

TEST_CASE("ToolRegistry rejects empty and duplicate names") {
    ToolRegistry registry;
    CHECK(!registry.add(makeDef(""), [](std::string_view) { return "{}"; }));
    CHECK(registry.add(makeDef("a"), [](std::string_view) { return "{}"; }));
    CHECK(!registry.add(makeDef("a"), [](std::string_view) { return "{}"; }));
    CHECK(!registry.add(makeDef("b"), nullptr));
    CHECK(registry.defs().size() == 1);
}

TEST_CASE("ToolRegistry keeps registration order and finds by name") {
    ToolRegistry registry;
    registry.add(makeDef("first"), [](std::string_view) { return "{}"; });
    registry.add(makeDef("second"), [](std::string_view) { return "{}"; });
    REQUIRE(registry.defs().size() == 2);
    CHECK(registry.defs()[0].name == "first");
    CHECK(registry.defs()[1].name == "second");
    CHECK(registry.find("second") != nullptr);
    CHECK(registry.find("missing") == nullptr);
}

TEST_CASE("ToolRegistry invoke passes arguments through to the handler") {
    ToolRegistry registry;
    std::string seen;
    registry.add(makeDef("echo"), [&](std::string_view args) {
        seen = args;
        return std::string(R"({"status":"ok"})");
    });
    const json result = invokeParsed(registry, "echo", R"({"v":1})");
    CHECK(result["status"] == "ok");
    CHECK(seen == R"({"v":1})");
}

TEST_CASE("ToolRegistry reports unknown tools as error JSON") {
    ToolRegistry registry;
    const json result = invokeParsed(registry, "nope", "{}");
    CHECK(result["status"] == "error");
    CHECK(result["message"].get<std::string>().find("nope") != std::string::npos);
}

TEST_CASE("ToolRegistry rejects malformed and non-object arguments without calling the handler") {
    ToolRegistry registry;
    bool called = false;
    registry.add(makeDef("t"), [&](std::string_view) {
        called = true;
        return std::string("{}");
    });
    CHECK(invokeParsed(registry, "t", "not json")["status"] == "error");
    CHECK(invokeParsed(registry, "t", "[1,2]")["status"] == "error");
    CHECK(!called);
    // Empty arguments are allowed for parameterless tools.
    registry.invoke("t", "");
    CHECK(called);
}

TEST_CASE("ToolRegistry converts handler exceptions into error JSON") {
    ToolRegistry registry;
    registry.add(makeDef("boom"),
                 [](std::string_view) -> std::string { throw std::runtime_error("dependency"); });
    const json result = invokeParsed(registry, "boom", "{}");
    CHECK(result["status"] == "error");
    CHECK(result["message"] == "dependency");
}

TEST_CASE("ToolRegistry::setBeforeInvoke fires with the tool name before the handler runs") {
    ToolRegistry registry;
    std::vector<std::string> order;
    registry.add(makeDef("t"), [&](std::string_view) {
        order.push_back("handler");
        return std::string("{}");
    });
    registry.setBeforeInvoke([&](std::string_view name) { order.push_back(std::string(name)); });

    registry.invoke("t", "{}");

    REQUIRE(order.size() == 2);
    CHECK(order[0] == "t");
    CHECK(order[1] == "handler");
}

TEST_CASE("ToolRegistry: neither hook fires for an unknown tool name") {
    ToolRegistry registry;
    bool beforeFired = false;
    bool afterFired = false;
    registry.setBeforeInvoke([&](std::string_view) { beforeFired = true; });
    registry.setAfterInvoke([&](std::string_view, std::string_view) { afterFired = true; });

    invokeParsed(registry, "missing", "{}");

    CHECK(!beforeFired);
    CHECK(!afterFired);
}

TEST_CASE("ToolRegistry: neither hook fires for malformed or non-object arguments") {
    ToolRegistry registry;
    bool beforeFired = false;
    bool afterFired = false;
    registry.add(makeDef("t"), [](std::string_view) { return std::string("{}"); });
    registry.setBeforeInvoke([&](std::string_view) { beforeFired = true; });
    registry.setAfterInvoke([&](std::string_view, std::string_view) { afterFired = true; });

    invokeParsed(registry, "t", "not json");
    invokeParsed(registry, "t", "[1,2]");

    CHECK(!beforeFired);
    CHECK(!afterFired);
}

TEST_CASE("ToolRegistry: a null BeforeInvoke hook (the default) is a no-op") {
    ToolRegistry registry;
    bool called = false;
    registry.add(makeDef("t"), [&](std::string_view) {
        called = true;
        return std::string("{}");
    });
    // No setBeforeInvoke call: invoke() must not crash on the null std::function.
    registry.invoke("t", "{}");
    CHECK(called);
}

TEST_CASE("ToolRegistry::setAfterInvoke fires after the handler with its exact result") {
    ToolRegistry registry;
    std::vector<std::string> order;
    registry.add(makeDef("t"), [&](std::string_view) {
        order.push_back("handler");
        return std::string(R"({"status":"ok","value":42})");
    });
    registry.setBeforeInvoke([&](std::string_view name) { order.push_back(std::string(name)); });
    std::string afterName;
    std::string afterResult;
    registry.setAfterInvoke([&](std::string_view name, std::string_view result) {
        order.push_back("after");
        afterName = name;
        afterResult = result;
    });

    registry.invoke("t", "{}");

    REQUIRE(order.size() == 3);
    CHECK(order[0] == "t");
    CHECK(order[1] == "handler");
    CHECK(order[2] == "after");
    CHECK(afterName == "t");
    CHECK(afterResult == R"({"status":"ok","value":42})");
}

TEST_CASE("ToolRegistry::setAfterInvoke sees the converted error JSON on a handler exception") {
    ToolRegistry registry;
    registry.add(makeDef("boom"),
                 [](std::string_view) -> std::string { throw std::runtime_error("dependency"); });
    bool beforeFired = false;
    std::string afterResult;
    registry.setBeforeInvoke([&](std::string_view) { beforeFired = true; });
    registry.setAfterInvoke(
        [&](std::string_view, std::string_view result) { afterResult = result; });

    registry.invoke("boom", "{}");

    CHECK(beforeFired);
    const json parsed = json::parse(afterResult, nullptr, false);
    REQUIRE(parsed.is_object());
    CHECK(parsed["status"] == "error");
    CHECK(parsed["message"] == "dependency");
}

TEST_CASE("ToolRegistry: a null AfterInvoke hook (the default) is a no-op") {
    ToolRegistry registry;
    bool called = false;
    registry.add(makeDef("t"), [&](std::string_view) {
        called = true;
        return std::string("{}");
    });
    // No setAfterInvoke call: invoke() must not crash on the null std::function.
    registry.invoke("t", "{}");
    CHECK(called);
}
