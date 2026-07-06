#include "ext/doctest.h"
#include "templates/template.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

TEST_CASE("TemplateGen generates console template correctly") {
    std::string test_dir = (std::filesystem::current_path() / "test_template_dir").string();
    std::filesystem::remove_all(test_dir);
    
    TemplateGen gen;
    bool success = gen.create("console", test_dir, "my_app", "c++26");
    REQUIRE(success == true);

    std::ifstream toml(test_dir + "/mokai.toml");
    REQUIRE(toml.is_open());
    std::stringstream buffer;
    buffer << toml.rdbuf();
    std::string toml_content = buffer.str();
    
    CHECK(toml_content.find("name = \"my_app\"") != std::string::npos);
    CHECK(toml_content.find("cpp_version = \"c++26\"") != std::string::npos);
    CHECK(toml_content.find("[target.my_app]") != std::string::npos);
    
    std::ifstream main_cpp(test_dir + "/src/main.cpp");
    REQUIRE(main_cpp.is_open());

    std::filesystem::remove_all(test_dir);
}

TEST_CASE("TemplateGen replaces macros like UPPER_PROJECT_NAME") {
    std::string test_dir = (std::filesystem::current_path() / "test_lib_dir").string();
    std::filesystem::remove_all(test_dir);
    
    TemplateGen gen;
    bool success = gen.create("library", test_dir, "core_math", "c++11");
    REQUIRE(success == true);

    std::ifstream toml(test_dir + "/mokai.toml");
    REQUIRE(toml.is_open());
    std::stringstream buffer;
    buffer << toml.rdbuf();
    std::string toml_content = buffer.str();
    
    CHECK(toml_content.find("defines_required = [\"USING_CORE_MATH=1\"]") != std::string::npos);
    CHECK(std::filesystem::exists(test_dir + "/include/public/core_math.hpp"));
    CHECK(std::filesystem::exists(test_dir + "/src/core_math.cpp"));

    std::filesystem::remove_all(test_dir);
}
