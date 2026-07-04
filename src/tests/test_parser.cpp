#include "ext/doctest.h"
#include "config/tom.hpp"
#include <string>

TEST_CASE("tom::Parser handles standard scalar values") {
    tom::Parser parser;
    parser.parse(R"(
        [project]
        name = "test"
        version = "1.0.0"
        flag = true
        inline_comment = "value" # this is a comment
    )");

    CHECK(parser.find_value("project", "name") == "test");
    CHECK(parser.find_value("project", "version") == "1.0.0");
    CHECK(parser.find_value("project", "flag") == "true");
    CHECK(parser.find_value("project", "inline_comment") == "value");
}

TEST_CASE("tom::Parser handles simple arrays") {
    tom::Parser parser;
    parser.parse(R"(
        [target.test]
        sources = ["src/a.cpp", "src/b.cpp"]
    )");

    std::vector<std::string> arr;
    for (const auto& f : parser.fields) {
        if (f.scope == "target.test" && f.key == "sources") {
            arr.push_back(f.value);
        }
    }
    REQUIRE(arr.size() == 2);
    CHECK(arr[0] == "src/a.cpp");
    CHECK(arr[1] == "src/b.cpp");
}

TEST_CASE("tom::Parser handles inline array tables and comments") {
    tom::Parser parser;
    parser.parse(R"(
        # Top level comment
        [group[0]]
        patterns = ["test1.cpp", "test2.cpp"]
    )");

    std::vector<std::string> arr;
    for (const auto& f : parser.fields) {
        if (f.scope == "group[0]" && f.key == "patterns") {
            arr.push_back(f.value);
        }
    }
    REQUIRE(arr.size() == 2);
    CHECK(arr[0] == "test1.cpp");
    CHECK(arr[1] == "test2.cpp");
}

TEST_CASE("tom::Parser evaluates object blocks inline mapping logic") {
    tom::Parser parser;
    parser.parse(R"(
        [project]
        version_from = { file = "version.txt", pattern = "v" }
    )");

    CHECK(parser.find_value("project.version_from", "file") == "version.txt");
    CHECK(parser.find_value("project.version_from", "pattern") == "v");
}
