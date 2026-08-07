#include "template.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

constexpr std::string_view CONSOLE_TOML = R"([project]
name = "{{PROJECT_NAME}}"
version = "0.1.0"
cpp_version = "{{CPP_VERSION}}"
default_compiler = "g++"

[target.{{PROJECT_NAME}}]
type = "executable"
sources = ["src/main.cpp"]
flags = ["-O3", "-Wall", "-Wextra"]
)";

constexpr std::string_view CONSOLE_CPP = R"(#include <print>

int main() {
    std::println("Hello, World!");
    return 0;
}
)";

constexpr std::string_view GUI_TOML = R"([project]
name = "{{PROJECT_NAME}}"
version = "0.1.0"
cpp_version = "{{CPP_VERSION}}"
include_dirs = ["include"]

[options]
BUILD_VULKAN_BACKEND = true

[target.{{PROJECT_NAME}}]
type = "executable"
sources = ["src/main.cpp"]
system_libs = ["glfw", "vulkan", "pthread"]

[[target.{{PROJECT_NAME}}.sources_if]]
condition = "options.BUILD_VULKAN_BACKEND == true"
patterns = ["src/vulkan_renderer.cpp"]
)";

constexpr std::string_view GUI_RENDERER_HPP = R"(#pragma once

namespace {{PROJECT_NAME}} {
class Renderer {
public:
    void init();
    void render();
};
}
)";

constexpr std::string_view GUI_MAIN_CPP = R"(#include "renderer.hpp"
#include <print>

int main() {
    std::println("Starting {{PROJECT_NAME}} GUI application...");
    {{PROJECT_NAME}}::Renderer r;
    r.init();
    return 0;
}
)";

constexpr std::string_view GUI_VULKAN_CPP = R"(#include "renderer.hpp"
#include <print>

namespace {{PROJECT_NAME}} {
void Renderer::init() {
    std::println("Initializing Vulkan renderer backend...");
}
void Renderer::render() {
    std::println("Rendering frame...");
}
}
)";

constexpr std::string_view GAME_TOML = R"([project]
name = "{{PROJECT_NAME}}"
version = "0.1.0"
cpp_version = "{{CPP_VERSION}}"
include_dirs = ["include"]

[target.{{PROJECT_NAME}}]
type = "executable"
sources = ["src/main.cpp", "src/game.cpp"]
system_libs = ["X11", "pthread", "m"]
flags = ["-O3", "-march=native"]
)";

constexpr std::string_view GAME_HPP = R"(#pragma once

class Game {
private:
    bool m_running = true;
public:
    void run();
};
)";

constexpr std::string_view GAME_MAIN_CPP = R"(#include "game.hpp"

int main() {
    Game game;
    game.run();
    return 0;
}
)";

constexpr std::string_view GAME_CPP = R"(#include "game.hpp"
#include <print>

void Game::run() {
    std::println("Starting game loop...");
    while (m_running) {
        std::println("Updating frame...");
        m_running = false;
    }
}
)";

constexpr std::string_view LIB_TOML = R"([project]
name = "{{PROJECT_NAME}}"
version = "1.0.0"
license = "MIT"
cpp_version = "{{CPP_VERSION}}"

[target.{{PROJECT_NAME}}]
type = "static_library"
sources = ["src/{{PROJECT_NAME}}.cpp"]
include_dirs = ["include/public"]

[exports]
default_targets = ["{{PROJECT_NAME}}"]
include_dirs = ["include/public"]
defines_required = ["USING_{{UPPER_PROJECT_NAME}}=1"]
)";

constexpr std::string_view LIB_HPP = R"(#pragma once

namespace {{PROJECT_NAME}} {
void perform_operation();
}
)";

constexpr std::string_view LIB_CPP = R"(#include "{{PROJECT_NAME}}.hpp"
#include <print>

namespace {{PROJECT_NAME}} {
void perform_operation() {
    std::println("Library operation performed successfully.");
}
}
)";

TemplateGen::TemplateGen() { registerTemplates(); }

void TemplateGen::registerTemplates() {
  m_templates["console"] = ProjectTemplate{
      .name = "console",
      .description =
          "Console App - A minimal, modern C++ executable using std::print",
      .files = {{"mokai.toml", CONSOLE_TOML}, {"src/main.cpp", CONSOLE_CPP}}};

  m_templates["gui"] = ProjectTemplate{
      .name = "gui",
      .description = "GUI App - Graphics executable workspace configured with "
                     "Vulkan backends",
      .files = {{"mokai.toml", GUI_TOML},
                {"include/renderer.hpp", GUI_RENDERER_HPP},
                {"src/main.cpp", GUI_MAIN_CPP},
                {"src/vulkan_renderer.cpp", GUI_VULKAN_CPP}}};

  m_templates["game"] = ProjectTemplate{
      .name = "game",
      .description =
          "Game Project - Scaffolds a full frame-loop interactive application",
      .files = {{"mokai.toml", GAME_TOML},
                {"include/game.hpp", GAME_HPP},
                {"src/main.cpp", GAME_MAIN_CPP},
                {"src/game.cpp", GAME_CPP}}};

  m_templates["library"] = ProjectTemplate{
      .name = "library",
      .description = "Library Skeleton - Static interface distribution package "
                     "with public export directories",
      .files = {{"mokai.toml", LIB_TOML},
                {"include/public/" + std::string("{{PROJECT_NAME}}") + ".hpp",
                 LIB_HPP},
                {"src/{{PROJECT_NAME}}.cpp", LIB_CPP}}};
}

std::vector<std::pair<std::string, std::string>>
TemplateGen::getAvailableTemplates() const {
  std::vector<std::pair<std::string, std::string>> list;
  for (const auto &[name, tmpl] : m_templates) {
    list.push_back({name, tmpl.description});
  }
  return list;
}

std::string TemplateGen::replacePlaceholders(std::string_view source,
                                             const std::string &project_name,
                                             const std::string &cpp_version) {
  std::string result(source);

  size_t pos = 0;
  while ((pos = result.find("{{PROJECT_NAME}}", pos)) != std::string::npos) {
    result.replace(pos, 16, project_name);
    pos += project_name.length();
  }

  std::string upper_project_name = project_name;
  std::transform(upper_project_name.begin(), upper_project_name.end(),
                 upper_project_name.begin(), ::toupper);

  pos = 0;
  while ((pos = result.find("{{UPPER_PROJECT_NAME}}", pos)) !=
         std::string::npos) {
    result.replace(pos, 22, upper_project_name);
    pos += upper_project_name.length();
  }

  pos = 0;
  while ((pos = result.find("{{CPP_VERSION}}", pos)) != std::string::npos) {
    result.replace(pos, 15, cpp_version);
    pos += cpp_version.length();
  }

  return result;
}

bool TemplateGen::create(const std::string &template_name,
                         const fs::path &output_dir,
                         const std::string &project_name,
                         const std::string &cpp_version) {
  auto it = m_templates.find(template_name);
  if (it == m_templates.end()) {
    std::cerr << "Error: Unknown scaffolding template '" << template_name
              << "'\n";
    return false;
  }

  const auto &project_template = it->second;

  for (const auto &file : project_template.files) {
    std::string processed_path_str =
        replacePlaceholders(file.relative_path, project_name, cpp_version);
    fs::path target_file_path = output_dir / processed_path_str;

    fs::create_directories(target_file_path.parent_path());

    std::ofstream out(target_file_path);
    if (!out.is_open()) {
      std::cerr << "Error: Failed to write scaffolding file to "
                << target_file_path << "\n";
      return false;
    }

    std::string processed_content =
        replacePlaceholders(file.content, project_name, cpp_version);
    out << processed_content;
    out.close();
  }

  return true;
}
