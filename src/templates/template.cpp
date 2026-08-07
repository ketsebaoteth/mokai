#include "template.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>

constexpr std::string_view DEFAULT_TOML = R"([project]
name = "{{PROJECT_NAME}}"
version = "0.1.0"
cpp_version = "{{CPP_VERSION}}"

[target.{{PROJECT_NAME}}]
type = "executable"
sources = ["src/main.cpp"]
)";

constexpr std::string_view DEFAULT_CPP = R"(#include <iostream>

int main() {
    std::cout << "Hello, World!\n";
    return 0;
}
)";

TemplateGen::TemplateGen() { registerTemplates(); }

void TemplateGen::registerTemplates() {
  m_templates["minimal"] = ProjectTemplate{
      .name = "minimal",
      .description = "Minimal C++ application",
      .files = {{"mokai.toml", DEFAULT_TOML}, {"src/main.cpp", DEFAULT_CPP}}};
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

  auto replaceAll = [&](std::string_view target, const std::string &value) {
    size_t pos = 0;
    while ((pos = result.find(target, pos)) != std::string::npos) {
      result.replace(pos, target.length(), value);
      pos += value.length();
    }
  };

  std::string upper_project_name = project_name;
  std::transform(upper_project_name.begin(), upper_project_name.end(),
                 upper_project_name.begin(), ::toupper);

  replaceAll("{{PROJECT_NAME}}", project_name);
  replaceAll("{{UPPER_PROJECT_NAME}}", upper_project_name);
  replaceAll("{{CPP_VERSION}}", cpp_version);

  return result;
}

bool TemplateGen::create(const std::string &template_name,
                         const fs::path &output_dir,
                         const std::string &project_name,
                         const std::string &cpp_version) {
  auto it = m_templates.find(template_name);
  if (it == m_templates.end()) {
    // Fallback to default template if specified template isn't found
    it = m_templates.begin();
  }

  const auto &project_template = it->second;

  for (const auto &file : project_template.files) {
    std::string processed_path =
        replacePlaceholders(file.relative_path, project_name, cpp_version);
    fs::path target_file_path = output_dir / processed_path;

    fs::create_directories(target_file_path.parent_path());

    std::ofstream out(target_file_path);
    if (!out.is_open()) {
      std::cerr << "Error: Failed to write file: " << target_file_path << "\n";
      return false;
    }

    out << replacePlaceholders(file.content, project_name, cpp_version);
  }

  return true;
}
