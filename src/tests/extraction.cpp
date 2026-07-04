#include "ext/doctest.h"
#include "config/config.hpp"
#include "graph/types.hpp"
#include "graph/condition/ConditionEval.hpp"
#include "cli/cli.hpp"
#include <fstream>
#include <filesystem>

TEST_CASE("Field Extractor parses conditional fields in template.toml format") {
    std::string temp_dir = "test_extraction_dir";
    std::filesystem::create_directories(temp_dir);
    
    std::ofstream out(temp_dir + "/mokai.toml");
    out << R"([project]
name = "test_proj"
version = "0.1.0"
cpp_version = "c++23"

[target.MOKAI]
type = "executable"
sources = ["src/main.cpp"]

[[target.MOKAI.sources_if]]
condition = "os == linux"
patterns = ["src/linux.cpp", "src/linux_other.cpp"]

[[target.MOKAI.flags_if]]
condition = "os == linux"
flags = ["-pthread"]

[[target.MOKAI.properties_if]]
condition = "os == windows"
defines = ["WIN32"]

[[target.MOKAI.system_libs_if]]
condition = "os == linux"
libs = ["m"]
)";
    out.close();

    mokai::GlobalOptions ops;
    mokai::Config conf(temp_dir, ops);
    
    const auto& manifest = conf.getManifest();
    REQUIRE(manifest.targets.size() == 1);
    const auto& tgt = manifest.targets[0];
    
    CHECK(tgt.sources.size() == 1);
    CHECK(tgt.sources_if.size() == 1);
    CHECK(tgt.sources_if[0].condition == "os == linux");
    CHECK(tgt.sources_if[0].patterns.size() == 2);
    
    CHECK(tgt.flags_if.size() == 1);
    CHECK(tgt.flags_if[0].condition == "os == linux");
    
    CHECK(tgt.properties_if.size() == 1);
    CHECK(tgt.properties_if[0].condition == "os == windows");
    
    CHECK(tgt.system_libs_if.size() == 1);
    CHECK(tgt.system_libs_if[0].condition == "os == linux");

    std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Target active evaluation correctly scopes conditionals") {
    mokai::Target tgt;
    tgt.system_libs = {"core"};
    tgt.system_libs_if.push_back({"arch == arm", {"math"}});
    tgt.system_libs_if.push_back({"arch == x86", {"avx"}});
    
    mokai::ConditionEngine eng_arm;
    eng_arm.setVariable("arch", "arm");
    auto eval_arm = [&](const std::string& c){ return eng_arm.evaluate(c); };
    
    auto libs_arm = tgt.getActiveSystemLibs(eval_arm);
    CHECK(libs_arm.size() == 2);
    if(libs_arm.size() == 2) {
        CHECK(libs_arm[0] == "core");
        CHECK(libs_arm[1] == "math");
    }

    mokai::ConditionEngine eng_x86;
    eng_x86.setVariable("arch", "x86");
    auto eval_x86 = [&](const std::string& c){ return eng_x86.evaluate(c); };
    
    auto libs_x86 = tgt.getActiveSystemLibs(eval_x86);
    CHECK(libs_x86.size() == 2);
    if(libs_x86.size() == 2) {
        CHECK(libs_x86[0] == "core");
        CHECK(libs_x86[1] == "avx");
    }
}
