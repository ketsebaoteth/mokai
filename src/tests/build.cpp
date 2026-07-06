#include "ext/doctest.h"
#include "config/config.hpp"
#include "graph/graph.hpp"
#include "cli/cli.hpp"
#include <filesystem>
#include <fstream>

TEST_CASE("Build process acts as expected on a single executable target") {
    std::string build_dir = (std::filesystem::current_path() / "test_build_dir").string();
    std::filesystem::remove_all(build_dir);
    std::filesystem::create_directories(build_dir + "/src");
    
    std::ofstream main_cpp(build_dir + "/src/main.cpp");
    main_cpp << "int main() { return 0; }\n";
    main_cpp.close();
    
    std::ofstream toml(build_dir + "/mokai.toml");
    toml << R"([project]
name = "test_app"

[output]
directory = "./out_bin"

[target.test_app]
type = "executable"
sources = ["src/main.cpp"]
)";
    toml.close();
    
    auto original_path = std::filesystem::current_path();
    std::filesystem::current_path(build_dir);
    
    mokai::GlobalOptions ops;
    ops.default_compiler = "gcc";
    mokai::Config conf(".", ops);
    auto manifest = std::make_shared<mokai::ProjectManifest>(conf.getManifest());
    
    auto graph_res = mokai::Graph::Create(manifest, ops);
    if (!graph_res.has_value()) {
        WARN("No compiler found, skipping compilation test.");
    } else {
        auto& graph = graph_res.value();
        auto edges = graph.getEdges();
        auto build_order = graph.computeBuildOrder(edges);
        
        REQUIRE(build_order.size() == 1);
        
        bool ok = graph.BuildAllTree(build_order);
        CHECK(ok == true);
        
        CHECK((std::filesystem::exists("./out_bin/debug/test_app") || std::filesystem::exists("./out_bin/debug/test_app.exe")));
    }
    
    std::filesystem::current_path(original_path);
    std::filesystem::remove_all(build_dir);
}

TEST_CASE("Build process handles libraries and dependencies") {
    std::string build_dir = (std::filesystem::current_path() / "test_build_lib_dir").string();
    std::filesystem::remove_all(build_dir);
    std::filesystem::create_directories(build_dir + "/src");
    
    std::ofstream lib_cpp(build_dir + "/src/lib.cpp");
    lib_cpp << "int do_math() { return 42; }\n";
    lib_cpp.close();

    std::ofstream main_cpp(build_dir + "/src/main.cpp");
    main_cpp << "int do_math();\nint main() { return do_math() - 42; }\n";
    main_cpp.close();
    
    std::ofstream toml(build_dir + "/mokai.toml");
    toml << R"([project]
name = "test_proj"

[output]
directory = "./out_bin"

[target.test_lib]
type = "static_library"
sources = ["src/lib.cpp"]

[target.test_app]
type = "executable"
sources = ["src/main.cpp"]
depends_on = ["test_lib"]
)";
    toml.close();
    
    auto original_path = std::filesystem::current_path();
    std::filesystem::current_path(build_dir);
    
    mokai::GlobalOptions ops;
    ops.default_compiler = "gcc";
    mokai::Config conf(".", ops);
    auto manifest = std::make_shared<mokai::ProjectManifest>(conf.getManifest());
    
    auto graph_res = mokai::Graph::Create(manifest, ops);
    if (!graph_res.has_value()) {
        WARN("No compiler found, skipping compilation test.");
    } else {
        auto& graph = graph_res.value();
        auto edges = graph.getEdges();
        auto build_order = graph.computeBuildOrder(edges);
        
        REQUIRE(build_order.size() == 2);
        
        bool ok = graph.BuildAllTree(build_order);
        CHECK(ok == true);
        
        CHECK((std::filesystem::exists("./out_bin/debug/test_app") || std::filesystem::exists("./out_bin/debug/test_app.exe")));
    }
    
    std::filesystem::current_path(original_path);
    std::filesystem::remove_all(build_dir);
}

#include <thread>
#include <chrono>

TEST_CASE("Incremental build skips unmodified files and rebuilds modified ones") {
    std::string build_dir = (std::filesystem::current_path() / "test_incr_build_dir").string();
    std::filesystem::remove_all(build_dir);
    std::filesystem::create_directories(build_dir + "/src");
    
    std::ofstream main_cpp(build_dir + "/src/main.cpp");
    main_cpp << "int main() { return 0; }\n";
    main_cpp.close();
    
    std::ofstream toml(build_dir + "/mokai.toml");
    toml << R"([project]
name = "test_app"

[output]
directory = "./out_bin"

[target.test_app]
type = "executable"
sources = ["src/main.cpp"]
)";
    toml.close();
    
    auto original_path = std::filesystem::current_path();
    std::filesystem::current_path(build_dir);
    
    mokai::GlobalOptions ops;
    ops.default_compiler = "gcc";
    mokai::Config conf(".", ops);
    auto manifest = std::make_shared<mokai::ProjectManifest>(conf.getManifest());
    
    auto graph_res = mokai::Graph::Create(manifest, ops);
    if (!graph_res.has_value()) {
        WARN("No compiler found, skipping compilation test.");
    } else {
        auto& graph = graph_res.value();
        auto edges = graph.getEdges();
        auto build_order = graph.computeBuildOrder(edges);
        
        bool ok1 = graph.BuildAllTree(build_order);
        CHECK(ok1 == true);
        
        std::string obj_path_linux = "./out_bin/debug/obj/test_app/src_main.cpp.o";
        std::string obj_path_win = "./out_bin/debug/obj/test_app/src_main.cpp.obj";
        std::string active_obj_path = std::filesystem::exists(obj_path_linux) ? obj_path_linux : obj_path_win;
        
        REQUIRE(std::filesystem::exists(active_obj_path));
        auto first_build_time = std::filesystem::last_write_time(active_obj_path);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1500)); 
        
        bool ok2 = graph.BuildAllTree(build_order);
        CHECK(ok2 == true);
        
        auto second_build_time = std::filesystem::last_write_time(active_obj_path);
        CHECK(first_build_time == second_build_time);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1500)); 
        std::ofstream main_cpp_mod(build_dir + "/src/main.cpp");
        main_cpp_mod << "int main() { return 1; }\n";
        main_cpp_mod.close();
        
        bool ok3 = graph.BuildAllTree(build_order);
        CHECK(ok3 == true);
        
        auto third_build_time = std::filesystem::last_write_time(active_obj_path);
        CHECK(third_build_time > second_build_time);
    }

    std::filesystem::current_path(original_path);
    std::filesystem::remove_all(build_dir);
}
