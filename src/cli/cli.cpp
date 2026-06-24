#include "cli.hpp"
#include "config/config.hpp"
#include "graph/graph.hpp"
#include "log/log.h"
#include <algorithm>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace mokai {

// Nuxt Cli-inspired ANSI Color Definitions

Cli::Cli() {
  log::Logger log;
  m_logger = log;
  log.SetPrefix("mokai");
};

int Cli::Run(int argc, char *argv[]) {
  if (argc == 1) {
    std::cout << Style::Green << "🟢 mokai " << Style::Dim << "v0.1.0\n"
              << Style::Reset;
    std::cout << Style::Dim << "Run " << Style::Reset
              << "mokai --help [command]" << Style::Dim << " to learn more.\n\n"
              << Style::Reset;
    std::cout << Style::Bold << "Available commands:\n" << Style::Reset;
    logSupportedCommands();
    return static_cast<int>(ExitCode::UsageError);
  }
  initCommands();
  auto result = ParseCliArgs(argc, argv);
  if (!result.has_value()) {
    switch (result.error()) {
    case CliError::UnknownCommand:
      logSupportedCommands();
      return static_cast<int>(ExitCode::CommandNotFound);

    case CliError::InvalidArguments:
      return static_cast<int>(ExitCode::UsageError);

    case CliError::BuildFailed:
      return static_cast<int>(ExitCode::GeneralFailure);

    default:
      return static_cast<int>(ExitCode::GeneralFailure);
    }
  }
  return static_cast<int>(ExitCode::Success);
}

std::expected<std::monostate, CliError> Cli::ParseCliArgs(int argc,
                                                          char *argv[]) {
  std::string command = argv[1];

  std::vector<std::string> cmdArgs;
  for (int i = 2; i < argc; i++) {
    cmdArgs.push_back(argv[i]);
  }

  auto cmdToDispath = m_supported_commands.find(command);
  if (cmdToDispath != m_supported_commands.end()) {
    auto result = cmdToDispath->second.callback(cmdArgs);
    return result;
  } else {
    std::cout << Style::Red << Style::Error << "Unknown command: " << command
              << "\n"
              << Style::Reset;
    std::cout << Style::Dim << "Available commands:\n" << Style::Reset;
    logSupportedCommands();
    return std::unexpected(CliError::UnknownCommand);
  }
}

void printWrapped(const std::string &text, size_t max_line_length = 80,
                  const std::string &indent = "                 ") {
  size_t start = 0;
  bool first_line = true;

  while (start < text.length()) {
    size_t current_limit =
        first_line ? max_line_length : (max_line_length - indent.length());
    size_t len = text.length() - start;

    if (len > current_limit) {
      size_t break_pt = text.rfind(' ', start + current_limit);

      if (break_pt == std::string::npos || break_pt < start) {
        break_pt = start + current_limit;
      }

      if (!first_line)
        std::cout << indent;
      std::cout << text.substr(start, break_pt - start) << "\n";

      start = break_pt + 1;
      first_line = false;
    } else {
      if (!first_line)
        std::cout << indent;
      std::cout << text.substr(start) << "\n";
      break;
    }
  }
}

std::expected<std::monostate, CliError>
Cli::handleHelp(const std::vector<std::string> &args) {
  if (args.size() == 0) {
    std::cout << Style::Red << Style::Error
              << "No command specified for help.\n"
              << Style::Reset;
    logSupportedCommands();
    return std::unexpected(CliError::InvalidArguments);
  }
  std::string helpCommand = args[0];
  auto help = m_supported_commands.find(helpCommand);
  if (help != m_supported_commands.end()) {
    std::cout << "\n " << Style::Green << "Command:" << Style::Reset << " "
              << help->first << "\n";
    std::cout << " " << Style::Cyan << "Usage:" << Style::Reset << "    "
              << help->second.usage.data() << "\n";
    std::cout << " " << Style::Dim << "Details:" << Style::Reset << " ";
    printWrapped(help->second.explanation.data(), 80, "          ");
    std::cout << "\n";
    return std::unexpected(CliError::InvalidArguments);
  } else {
    std::cout << Style::Red << Style::Error << "Unsupported command.\n"
              << Style::Reset;
    logSupportedCommands();
    return std::unexpected(CliError::UnknownCommand);
  }
}

std::expected<std::monostate, CliError>
Cli::handleBuild(const std::vector<std::string> &args) {
  fs::path workingDir = fs::current_path();

  std::cout << Style::Cyan << Style::Arrow
            << "Initializing build target pipeline...\n"
            << Style::Reset;
  Config config(workingDir.string());
  Graph graph(config.getManifest());

  auto buildOrder = graph.computeBuildOrder(graph.getEdges());
  if (buildOrder.empty()) {
    std::cout << Style::Red << Style::Error
              << "Build pipeline aborted: cyClic or invalid dependencies.\n"
              << Style::Reset;
    return std::unexpected(CliError::BuildFailed);
  }

  if (!graph.BuildAllTree(buildOrder)) {
    std::cout << Style::Red << Style::Error
              << "Compilation sequence dropped out with errors.\n"
              << Style::Reset;
    return std::unexpected(CliError::BuildFailed);
  }

  std::cout << "\n"
            << Style::Green << Style::Success
            << "Build completed successfully.\n"
            << Style::Reset;
  return {};
}

// Interactive Multi-Choice Component with Navigation and Viewport Scrolling
static size_t promptChoice(const std::string &title,
                           const std::vector<std::string> &options,
                           size_t default_idx = 0) {
  if (options.empty())
    return 0;

  size_t current_idx = default_idx;
  const size_t max_visible = 5;

  struct termios orig_termios;
  tcgetattr(STDIN_FILENO, &orig_termios);

  struct termios raw = orig_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

  std::cout << "\033[?25l";

  bool selecting = true;
  bool first_render = true;

  while (selecting) {
    size_t start_idx = 0;
    if (current_idx >= max_visible) {
      start_idx = current_idx - max_visible + 1;
    }

    if (!first_render) {
      size_t lines_to_clear = std::min(options.size(), max_visible) + 2;
      for (size_t i = 0; i < lines_to_clear; ++i) {
        std::cout << "\033[A\033[2K";
      }
    }
    first_render = false;

    std::cout << Style::Cyan << "● " << Style::Reset << title << "\n";

    for (size_t i = start_idx;
         i < std::min(options.size(), start_idx + max_visible); ++i) {
      if (i == current_idx) {
        std::cout << "  " << Style::Green << "❯ ⦿ " << options[i]
                  << Style::Reset << "\n";
      } else {
        std::cout << "    " << Style::Dim << "○ " << options[i] << Style::Reset
                  << "\n";
      }
    }

    std::cout << "  " << Style::Dim
              << "(Use ↑/↓ or j/k to navigate. Enter to select";
    if (options.size() > max_visible) {
      std::cout << " | item " << (current_idx + 1) << "/" << options.size();
    }
    std::cout << ")" << Style::Reset << "\n";
    std::flush(std::cout);

    char ch;
    if (read(STDIN_FILENO, &ch, 1) == 1) {
      if (ch == '\n' || ch == '\r') {
        selecting = false;
      } else if (ch == 'k') {
        if (current_idx > 0)
          current_idx--;
      } else if (ch == 'j') {
        if (current_idx < options.size() - 1)
          current_idx++;
      } else if (ch == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
            read(STDIN_FILENO, &seq[1], 1) == 1) {
          if (seq[0] == '[') {
            if (seq[1] == 'A' && current_idx > 0)
              current_idx--;
            else if (seq[1] == 'B' && current_idx < options.size() - 1)
              current_idx++;
          }
        }
      } else if (ch == 3) {
        std::cout << "\033[?25h";
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        std::cout << Style::Red << "\nAborted via interrupt.\n" << Style::Reset;
        exit(130);
      }
    }
  }

  std::cout << "\033[?25h";
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);

  size_t lines_to_clear = std::min(options.size(), max_visible) + 2;
  for (size_t i = 0; i < lines_to_clear; ++i) {
    std::cout << "\033[A\033[2K";
  }
  std::cout << Style::Green << Style::Success << title << " " << Style::Dim
            << "› " << Style::Reset << options[current_idx] << "\n";

  return current_idx;
}

// Nuxt styled text prompts
static std::string promptText(const std::string &prompt,
                              const std::string &default_val = "") {
  std::cout << Style::Green << Style::Arrow << Style::Reset << prompt;
  if (!default_val.empty()) {
    std::cout << Style::Dim << " (" << default_val << ")" << Style::Reset;
  }
  std::cout << " ";

  std::string input;
  std::getline(std::cin, input);

  input.erase(0, input.find_first_not_of(" \t\r\n"));
  input.erase(input.find_last_not_of(" \t\r\n") + 1);

  return input.empty() ? default_val : input;
}

// High-Performance Interactive Package Routing Subsystem Addition Handler
std::expected<std::monostate, CliError>
Cli::handlePackageAdd(const std::vector<std::string> &args) {
  fs::path tomlPath = fs::current_path() / "mokai.toml";
  if (!fs::exists(tomlPath)) {
    std::cout << Style::Red << Style::Error
              << "Active workspace error: No 'mokai.toml' file found in "
                 "current context root.\n"
              << Style::Reset;
    return std::unexpected(CliError::InvalidWorkspace);
  }

  std::string chosenPackage = "";

  if (!args.empty()) {
    chosenPackage = args[0];
  } else {
    std::cout << "\n"
              << Style::Green << "📦 Mokai Package Add " << Style::Dim
              << "– Inject a workspace dependency\n"
              << Style::Reset;
    std::cout << Style::Dim
              << "──────────────────────────────────────────────────\n\n"
              << Style::Reset;

    const char *homeEnv = std::getenv("HOME");
    fs::path homePath = homeEnv ? fs::path(homeEnv) : fs::current_path();
    fs::path registryDir = homePath / ".mokai" / "registry";

    if (!fs::exists(registryDir) || fs::is_empty(registryDir)) {
      std::cout << Style::Yellow << Style::Info
                << "Central configuration map is uninitialized. Run a target "
                   "build sequence to sync index assets.\n"
                << Style::Reset;
      chosenPackage =
          promptText("Enter package target manually (e.g., sfml@3.0.0):");
    } else {
      std::vector<std::string> availablePackages;
      for (const auto &entry : fs::directory_iterator(registryDir)) {
        if (!entry.is_directory() && entry.path().extension() == ".toml") {
          availablePackages.push_back(entry.path().stem().string());
        }
      }
      std::sort(availablePackages.begin(), availablePackages.end());

      if (availablePackages.empty()) {
        chosenPackage = promptText(
            "No packages tracked in registry maps. Define entity manually:");
      } else {
        size_t pkgIdx = promptChoice(
            "Select a package to introduce into workspace targets:",
            availablePackages, 0);
        std::string pkgName = availablePackages[pkgIdx];

        // Parse central blueprint maps to extract valid SemVer constraint
        // options
        fs::path regFile = registryDir / (pkgName + ".toml");
        std::ifstream rFile(regFile.string());
        std::stringstream rStream;
        rStream << rFile.rdbuf();
        std::string rContent = rStream.str();

        std::vector<std::string> constraintOptions;
        size_t pos = 0;
        while ((pos = rContent.find("version_range", pos)) !=
               std::string::npos) {
          size_t quoteStart = rContent.find('"', pos);
          if (quoteStart != std::string::npos) {
            size_t quoteEnd = rContent.find('"', quoteStart + 1);
            if (quoteEnd != std::string::npos) {
              std::string vRange =
                  rContent.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
              if (std::ranges::find(constraintOptions, vRange) ==
                  constraintOptions.end()) {
                constraintOptions.push_back(vRange);
              }
            }
          }
          pos += 13;
        }

        if (constraintOptions.empty()) {
          std::string customVer =
              promptText("No baseline ranges found. Enter explicit revision "
                         "rule constraint (or blank for latest):");
          chosenPackage =
              customVer.empty() ? pkgName : pkgName + "@" + customVer;
        } else {
          constraintOptions.push_back("Custom Spec Constraint...");
          size_t verIdx = promptChoice(
              "Select valid SemVer matching target limits:", constraintOptions,
              0);

          if (constraintOptions[verIdx] == "Custom Spec Constraint...") {
            std::string customVer = promptText(
                "Define constraint parameters manually (e.g., >=1.2.0):");
            chosenPackage =
                customVer.empty() ? pkgName : pkgName + "@" + customVer;
          } else {
            std::string chosenRange = constraintOptions[verIdx];
            std::string cleanVer = promptText(
                "Specify target configuration version code (matches range: " +
                    chosenRange + "):",
                "1.0.0");
            chosenPackage = pkgName + "@" + cleanVer;
          }
        }
      }
    }
  }

  if (chosenPackage.empty()) {
    std::cout
        << Style::Red << Style::Error
        << "Aborted package insertion: Selection expression sequence invalid.\n"
        << Style::Reset;
    return std::unexpected(CliError::PackageNotFound);
  }

  // Surgical manifest line injection sequence preserving project spacing layout
  std::ifstream inFile(tomlPath.string());
  std::stringstream buffer;
  buffer << inFile.rdbuf();
  std::string tomlContent = buffer.str();
  inFile.close();

  size_t depPos = tomlContent.find("dependencies = [");
  if (depPos != std::string::npos) {
    size_t closeBrace = tomlContent.find(']', depPos);
    if (closeBrace != std::string::npos) {
      std::string insertion = "\n    \"" + chosenPackage + "\",";
      tomlContent.insert(closeBrace, insertion);
    }
  } else {
    size_t projPos = tomlContent.find("[project]");
    if (projPos != std::string::npos) {
      size_t nextLine = tomlContent.find('\n', projPos);
      std::string insertion =
          "\ndependencies = [\n    \"" + chosenPackage + "\"\n]\n";
      if (nextLine != std::string::npos) {
        tomlContent.insert(nextLine + 1, insertion);
      } else {
        tomlContent += insertion;
      }
    } else {
      tomlContent +=
          "\n[project]\ndependencies = [\n    \"" + chosenPackage + "\"\n]\n";
    }
  }

  std::ofstream outFile(tomlPath.string());
  outFile << tomlContent;
  outFile.close();

  std::cout << "\n"
            << Style::Green << Style::Success
            << "Successfully added dependency to mokai.toml -> " << Style::Reset
            << Style::Bold << chosenPackage << Style::Reset << "\n";
  std::cout << Style::Dim
            << "Hint: Invoke 'mokai Build' to evaluate routing graphs and "
               "trigger smart clone/fetch execution passes.\n"
            << Style::Reset;
  return {};
}
static void processTemplatePlaceholders(const fs::path &file_path,
                                        const std::string &project_name,
                                        const std::string &cpp_version) {
  if (!fs::exists(file_path))
    return;

  std::ifstream in_file(file_path);
  if (!in_file.is_open())
    return;

  std::stringstream buffer;
  buffer << in_file.rdbuf();
  std::string content = buffer.str();
  in_file.close();

  auto replace_all = [](std::string &str, const std::string &from,
                        const std::string &to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
      str.replace(start_pos, from.length(), to);
      start_pos += to.length();
    }
  };

  replace_all(content, "{{PROJECT_NAME}}", project_name);
  replace_all(content, "{{CPP_VERSION}}", cpp_version);

  std::ofstream out_file(file_path);
  if (out_file.is_open()) {
    out_file << content;
  }
}

std::expected<std::monostate, CliError>
Cli::handleCreateProject(const std::vector<std::string> &args) {
  std::cout << "\n"
            << Style::Green << "✨ Mokai Initializer " << Style::Dim
            << "– Create a new environment\n"
            << Style::Reset;
  std::cout << Style::Dim
            << "──────────────────────────────────────────────────\n\n"
            << Style::Reset;

  // 1. Get Project Name
  std::string project_name = promptText("Project name", "my_mokai_project");
  fs::path target_dir = fs::current_path() / project_name;

  if (fs::exists(target_dir)) {
    std::cout << "\n"
              << Style::Red << Style::Error << "Aborted: Directory '"
              << project_name << "' already exists.\n\n"
              << Style::Reset;
    return std::unexpected(CliError::InvalidArguments);
  }

  // 2. Choose C++ Version
  std::vector<std::string> cpp_versions = {"c++11", "c++14", "c++17",
                                           "c++20", "c++23", "c++26"};
  size_t cpp_idx = promptChoice(
      "Select C++ Language Specification Target:", cpp_versions, 4);
  std::string chosen_cpp = cpp_versions[cpp_idx];

  // 3. Resolve Template Path using the CMake Macro
#ifdef MOKAI_TEMPLATE_DIR
  fs::path template_root = fs::path(MOKAI_TEMPLATE_DIR);
#else
  fs::path template_root = fs::current_path() / "src/templates";
#endif

  if (!fs::exists(template_root) || !fs::is_directory(template_root)) {
    std::cout
        << "\n"
        << Style::Red << Style::Error
        << "Template Engine Error: Unable to locate source configurations.\n";
    std::cout << "  Expected path mapping: " << template_root.string() << "\n\n"
              << Style::Reset;
    return std::unexpected(CliError::GeneralFailure);
  }

  std::vector<std::string> available_templates;
  for (const auto &entry : fs::directory_iterator(template_root)) {
    if (entry.is_directory()) {
      available_templates.push_back(entry.path().filename().string());
    }
  }

  if (available_templates.empty()) {
    std::cout << Style::Red << Style::Error
              << "No architectural blueprints found under assets path.\n"
              << Style::Reset;
    return std::unexpected(CliError::GeneralFailure);
  }

  std::sort(available_templates.begin(), available_templates.end());
  size_t template_idx = promptChoice(
      "Select Project Skeleton Blueprint:", available_templates, 0);
  std::string chosen_template = available_templates[template_idx];
  fs::path selected_template_path = template_root / chosen_template;

  std::vector<std::string> git_options = {"Yes", "No"};
  size_t git_idx = promptChoice(
      "Initialize empty local Git version control tree?", git_options, 0);
  bool init_git = (git_idx == 0);

  std::cout << "\n"
            << Style::Dim << "⠋ Spawning environment scaffolding..."
            << Style::Reset << "\r";
  std::flush(std::cout);

  try {
    fs::create_directories(target_dir);
    fs::copy(selected_template_path, target_dir,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);
    processTemplatePlaceholders(target_dir / "mokai.toml", project_name,
                                chosen_cpp);
  } catch (const fs::filesystem_error &e) {
    std::cout << "\n"
              << Style::Red << Style::Error << "I/O Failure writing files:\n  "
              << e.what() << "\n\n"
              << Style::Reset;
    return std::unexpected(CliError::ProjectCreationDenied);
  }

  // 6. Optional Git initialization
  if (init_git) {
    std::string git_cmd =
        "git init " + target_dir.string() + " > /dev/null 2>&1";
    std::system(git_cmd.c_str());
  }

  // Completed Banner
  std::cout << "\r" << Style::Green << Style::Success
            << "Project setup initialized perfectly!" << Style::Reset << "\n\n";
  std::cout << "  " << Style::Dim << "Location: " << Style::Reset
            << target_dir.string() << "\n\n";
  std::cout << "  Navigate and trigger production builds via:\n";
  std::cout << "  " << Style::Cyan << "cd " << project_name << Style::Reset
            << "\n";
  std::cout << "  " << Style::Cyan << "mokai Build" << Style::Reset << "\n\n";
  return {};
}

Cli::~Cli() {}

void Cli::logSupportedCommands() {
  for (const auto &command : m_supported_commands) {
    std::cout << "    " << Style::Green << "• " << Style::Reset << command.first
              << "\n";
  }
  std::cout << "\n";
}

} // namespace mokai
