#include <doctest/doctest.h>

#include <kumo/core/asset_name.h>
#include <kumo/core/file.h>
#include <kumo/core/file_watcher.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

TEST_CASE("readTextFile roundtrip and missing file") {
    fs::path path = fs::temp_directory_path() / "kumo_read_roundtrip.txt";
    const std::string content = "line1\nline2\n\tindented";
    {
        std::ofstream out(path, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    auto result = kumo::readTextFile(path);
    REQUIRE(result.has_value());
    CHECK(*result == content);

    std::error_code ec;
    fs::remove(path, ec);

    auto missing = kumo::readTextFile(fs::temp_directory_path() / "kumo_missing_9d3f.txt");
    CHECK_FALSE(missing.has_value());
    CHECK_FALSE(missing.error().empty());
}

TEST_CASE("FileWatcher fires once when mtime changes") {
    fs::path path = fs::temp_directory_path() / "kumo_watch_change.txt";
    {
        std::ofstream out(path, std::ios::binary);
        out << "a";
    }

    int fires = 0;
    fs::path fired;
    kumo::FileWatcher watcher(std::chrono::milliseconds(0));
    watcher.watch(path, [&](const fs::path& p) {
        ++fires;
        fired = p;
    });

    watcher.poll();
    CHECK(fires == 0);

    std::error_code ec;
    auto previous = fs::last_write_time(path, ec);
    REQUIRE_FALSE(ec);
    fs::last_write_time(path, previous + std::chrono::seconds(2), ec);
    REQUIRE_FALSE(ec);

    watcher.poll();
    CHECK(fires == 1);
    CHECK(fired == path);

    watcher.poll();
    CHECK(fires == 1);

    fs::remove(path, ec);
}

TEST_CASE("isPlainAssetName accepts ordinary names, rejects traversal and hidden names") {
    CHECK(kumo::isPlainAssetName("sand"));
    CHECK(kumo::isPlainAssetName("Damaged_Helmet-01"));
    CHECK(kumo::isPlainAssetName("day.hdr"));

    CHECK_FALSE(kumo::isPlainAssetName(""));
    CHECK_FALSE(kumo::isPlainAssetName("."));
    CHECK_FALSE(kumo::isPlainAssetName(".."));
    CHECK_FALSE(kumo::isPlainAssetName(".hidden"));
    CHECK_FALSE(kumo::isPlainAssetName("../x"));
    CHECK_FALSE(kumo::isPlainAssetName("a/b"));
    CHECK_FALSE(kumo::isPlainAssetName("a\\b"));
    CHECK_FALSE(kumo::isPlainAssetName("/tmp/x"));
    CHECK_FALSE(kumo::isPlainAssetName("models/../../etc/passwd"));
}

TEST_CASE("isPlainAssetPath accepts one category component, rejects deeper nesting and traversal") {
    CHECK(kumo::isPlainAssetPath("sand"));        // single component: still fine
    CHECK(kumo::isPlainAssetPath("props/crate")); // exactly one '/'
    CHECK(kumo::isPlainAssetPath("Nature_Kit/bridge-stone"));

    CHECK_FALSE(kumo::isPlainAssetPath("props/crate/lid")); // two '/': over maxComponents
    CHECK_FALSE(kumo::isPlainAssetPath(""));
    CHECK_FALSE(kumo::isPlainAssetPath("props/"));       // empty leaf component
    CHECK_FALSE(kumo::isPlainAssetPath("/crate"));       // empty (absolute) first component
    CHECK_FALSE(kumo::isPlainAssetPath("props//crate")); // empty middle component
    CHECK_FALSE(kumo::isPlainAssetPath("../props/crate"));
    CHECK_FALSE(kumo::isPlainAssetPath("props/../crate"));
    CHECK_FALSE(kumo::isPlainAssetPath("props/.crate")); // leaf starts with '.'
    CHECK_FALSE(kumo::isPlainAssetPath(".props/crate"));
    CHECK_FALSE(kumo::isPlainAssetPath("props\\crate"));
    CHECK_FALSE(kumo::isPlainAssetPath("props/crate", 1)); // maxComponents=1: no slash allowed
    CHECK(kumo::isPlainAssetPath("crate", 1));
}

TEST_CASE("FileWatcher fires when a missing file appears") {
    fs::path path = fs::temp_directory_path() / "kumo_watch_appear.txt";
    std::error_code ec;
    fs::remove(path, ec);

    int fires = 0;
    kumo::FileWatcher watcher(std::chrono::milliseconds(0));
    watcher.watch(path, [&](const fs::path&) { ++fires; });

    watcher.poll();
    CHECK(fires == 0);

    {
        std::ofstream out(path, std::ios::binary);
        out << "x";
    }

    watcher.poll();
    CHECK(fires == 1);

    fs::remove(path, ec);
}
