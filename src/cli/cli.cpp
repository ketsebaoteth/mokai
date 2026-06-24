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
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace mokai {

Cli::Cli() {
  log::Logger log;
  m_logger = log;
  m_logger.SetPrefix("mokai");
}

int Cli::Run(int argc, char *argv[]) {
  initCommands();
  if (argc == 1) {
    std::println(std::cerr, " {}{} {}{}", Style::Green, "◆ mokai", Style::Dim,
                 MOKAI_VERSION);
    std::println(std::cerr,
                 "  {}Run {}mokai --help [command]{} to learn more.\n",
                 Style::Dim, Style::Reset, Style::Dim);
    std::println(std::cerr, "{}Available commands:{}", Style::Bold,
                 Style::Reset);
    logSupportedCommands();
    return static_cast<int>(ExitCode::UsageError);
  }

  auto result = ParseCliArgs(argc, argv);
  if (!result.has_value()) {
    m_logger.Error(result.error().message);

    switch (result.error().code) {
    case CliError::Code::UnknownCommand:
      logSupportedCommands();
      return static_cast<int>(ExitCode::CommandNotFound);
    case CliError::Code::InvalidArguments:
    case CliError::Code::InvalidWorkspace:
      return static_cast<int>(ExitCode::UsageError);
    case CliError::Code::BuildFailed:
    case CliError::Code::PackageNotFound:
    case CliError::Code::GeneralFailure:
    case CliError::Code::ProjectCreationDenied:
      return static_cast<int>(ExitCode::GeneralFailure);
    }
  }
  return static_cast<int>(ExitCode::Success);
}

std::expected<std::monostate, CliError> Cli::ParseCliArgs(int argc,
                                                          char *argv[]) {
  std::vector<std::string> rawArgs;
  rawArgs.reserve(argc - 1);
  for (int i = 1; i < argc; ++i) {
    rawArgs.push_back(argv[i]);
  }

  std::string command = "";
  std::vector<std::string> subCommandArgs;

  // Modern deterministic switch evaluation
  for (size_t i = 0; i < rawArgs.size(); ++i) {
    const auto &arg = rawArgs[i];

    if (arg == "-v" || arg == "--verbose") {
      m_options.verbosity = Verbosity::Verbose;
    } else if (arg == "-q" || arg == "--quiet") {
      m_options.verbosity = Verbosity::Quiet;
    } else if (arg == "--release") {
      m_options.profile = BuildProfile::Release;
    } else if (arg == "--debug") {
      m_options.profile = BuildProfile::Debug;
    } else if (arg == "--no-cache" || arg == "--clean") {
      m_options.force_rebuild = true;
    } else if (arg == "-j" || arg == "--jobs") {
      if (i + 1 >= rawArgs.size()) {
        return std::unexpected(
            CliError{CliError::Code::InvalidArguments,
                     "Option flag '" + arg +
                         "' requires a numeric execution count payload value"});
      }
      try {
        m_options.job_count = std::stoi(rawArgs[++i]);
      } catch (...) {
        return std::unexpected(CliError{CliError::Code::InvalidArguments,
                                        "Invalid numeric payload token value "
                                        "passed into job allocations: '" +
                                            rawArgs[i] + "'"});
      }
    } else if (arg == "--target") {
      if (i + 1 >= rawArgs.size()) {
        return std::unexpected(CliError{CliError::Code::InvalidArguments,
                                        "Option flag '--target' requires a "
                                        "tracking definition parameter value"});
      }
      m_options.target_filter = rawArgs[++i];
    } else if (arg.starts_with("-")) {
      return std::unexpected(
          CliError{CliError::Code::UnknownCommand,
                   "Unrecognized system infrastructure option flag switch: '" +
                       arg + "'"});
    } else {
      // First unflagged literal token represents our operational execution
      // command routing target
      if (command.empty()) {
        command = arg;
      } else {
        subCommandArgs.push_back(arg);
      }
    }
  }

  // Update logging level constraints based on parsed environment state
  // adjustments
  if (m_options.verbosity == Verbosity::Quiet) {
    m_logger.SetLevel(log::Level::Error);
  } else if (m_options.verbosity == Verbosity::Verbose) {
    m_logger.SetLevel(log::Level::Debug);
  }

  if (command.empty()) {
    return std::unexpected(CliError{CliError::Code::UnknownCommand,
                                    "Missing operational instruction execution "
                                    "action target path definition context"});
  }

  if (auto cmdToDispatch = m_supported_commands.find(command);
      cmdToDispatch != m_supported_commands.end()) {
    return cmdToDispatch->second.callback(subCommandArgs);
  }

  return std::unexpected(CliError{CliError::Code::UnknownCommand,
                                  "Unknown command issued: '" + command + "'"});
}

static void
printWrapped(const std::string_view &text, size_t max_line_length = 80,
             const std::string_view &indent = "                  ") {
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
      if (!first_line) {
        std::print("{}", indent);
      }
      std::println("{}", text.substr(start, break_pt - start));

      start = break_pt + 1;
      first_line = false;
    } else {
      if (!first_line) {
        std::print("{}", indent);
      }
      std::println("{}", text.substr(start));
      break;
    }
  }
}

std::expected<std::monostate, CliError>
Cli::handleHelp(const std::vector<std::string> &args) {
  if (args.empty()) {
    logSupportedCommands();
    return std::unexpected(
        CliError{CliError::Code::InvalidArguments,
                 "Help invoked with missing or empty command argument"});
  }

  if (auto help = m_supported_commands.find(args[0]);
      help != m_supported_commands.end()) {
    std::println("\n {}Command:{} {}", Style::Green, Style::Reset, help->first);
    std::println(" {}Usage:{}    {}", Style::Cyan, Style::Reset,
                 help->second.usage);
    std::print(" {}Details:{} ", Style::Dim, Style::Reset);
    printWrapped(help->second.explanation, 80, "          ");
    return {};
  } else {
    return std::unexpected(
        CliError{CliError::Code::UnknownCommand,
                 "Help subsystem unable to locate command: '" + args[0] + "'"});
  }
}

std::expected<std::monostate, CliError>
Cli::handleBuild(const std::vector<std::string> &args) {
  fs::path workingDir = args.empty() ? fs::current_path() : fs::path(args[0]);
  if (!fs::exists(workingDir)) {
    return std::unexpected(
        CliError{CliError::Code::InvalidWorkspace,
                 "Path does not exist: " + workingDir.string()});
  }
  if (!fs::is_directory(workingDir)) {
    return std::unexpected(
        CliError{CliError::Code::InvalidWorkspace,
                 "Path is not a directory: " + workingDir.string()});
  }

  workingDir = fs::absolute(workingDir).lexically_normal();

  if (m_options.verbosity != Verbosity::Quiet) {
    std::println("{}{}Initializing build target pipeline...{}", Style::Cyan,
                 Style::Arrow, Style::Reset);
  }

  Config config(workingDir.string());
  Graph graph(config.getManifest(), m_options);

  auto buildOrder = graph.computeBuildOrder(graph.getEdges());
  if (buildOrder.empty()) {
    return std::unexpected(CliError{
        CliError::Code::BuildFailed,
        "Build pipeline aborted: cyclic or invalid dependencies detected"});
  }

  if (!graph.BuildAllTree(buildOrder)) {
    return std::unexpected(CliError{
        CliError::Code::BuildFailed,
        "Compilation sequence dropped out with errors (non-zero exit code)"});
  }

  if (m_options.verbosity != Verbosity::Quiet) {
    std::println("\n{}{}Build completed successfully.{}", Style::Green,
                 Style::Success, Style::Reset);
  }
  return {};
}

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

  std::print("\033[?25l");

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
        std::print("\033[A\033[2K");
      }
    }
    first_render = false;

    std::println("{}● {} {}", Style::Cyan, Style::Reset, title);

    for (size_t i = start_idx;
         i < std::min(options.size(), start_idx + max_visible); ++i) {
      if (i == current_idx) {
        std::println("  {}{}⦿ {}{}", Style::Green, "❯ ", options[i],
                     Style::Reset);
      } else {
        std::println("    {}{} {}", Style::Dim, "○", options[i], Style::Reset);
      }
    }

    std::print("  {}(Use ↑/↓ or j/k to navigate. Enter to select", Style::Dim);
    if (options.size() > max_visible) {
      std::print(" | item {}/{}", (current_idx + 1), options.size());
    }
    std::println("){}", Style::Reset);
    std::fflush(stdout);

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
        std::print("\033[?25h");
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        std::println("{}\nAborted via interrupt.{}", Style::Red, Style::Reset);
        std::exit(130);
      }
    }
  }

  std::print("\033[?25h");
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);

  size_t lines_to_clear = std::min(options.size(), max_visible) + 2;
  for (size_t i = 0; i < lines_to_clear; ++i) {
    std::print("\033[A\033[2K");
  }
  std::println("{}{}{} {}› {}{}", Style::Green, Style::Success, title,
               Style::Dim, Style::Reset, options[current_idx]);

  return current_idx;
}

static std::string promptText(const std::string &prompt,
                              const std::string &default_val = "") {
  std::print("{}{}{}{}", Style::Green, Style::Arrow, Style::Reset, prompt);
  if (!default_val.empty()) {
    std::print("{} ({}){}", Style::Dim, default_val, Style::Reset);
  }
  std::print(" ");
  std::fflush(stdout);

  std::string input;
  std::getline(std::cin, input);

  input.erase(0, input.find_first_not_of(" \t\r\n"));
  input.erase(input.find_last_not_of(" \t\r\n") + 1);

  return input.empty() ? default_val : input;
}

std::expected<std::monostate, CliError>
Cli::handlePackageAdd(const std::vector<std::string> &args) {
  fs::path tomlPath = fs::current_path() / "mokai.toml";
  if (!fs::exists(tomlPath)) {
    return std::unexpected(CliError{CliError::Code::InvalidWorkspace,
                                    "Active workspace error: No 'mokai.toml' "
                                    "file found in current context root"});
  }

  std::string chosenPackage = "";

  if (!args.empty()) {
    chosenPackage = args[0];
  } else {
    std::println(
        "\n{}{}📦 Mokai Package Add {}{}– Inject a workspace dependency{}",
        Style::Green, Style::Reset, Style::Dim, Style::Reset, Style::Reset);
    std::println("{}──────────────────────────────────────────────────\n{}",
                 Style::Dim, Style::Reset);

    const char *homeEnv = std::getenv("HOME");
    fs::path homePath = homeEnv ? fs::path(homeEnv) : fs::current_path();
    fs::path registryDir = homePath / ".mokai" / "registry";

    if (!fs::exists(registryDir) || fs::is_empty(registryDir)) {
      std::println("{}{}Central configuration map is uninitialized. Run a "
                   "target build sequence to sync index assets.{}",
                   Style::Yellow, Style::Info, Style::Reset);
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
    return std::unexpected(CliError{CliError::Code::PackageNotFound,
                                    "Aborted package insertion: Selection "
                                    "expression sequence invalid or empty"});
  }

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

  std::println("\n{}{}Successfully added dependency to mokai.toml -> {}{}{}",
               Style::Green, Style::Success, Style::Reset, Style::Bold,
               chosenPackage, Style::Reset);
  std::println("{}Hint: Invoke 'mokai Build' to evaluate routing graphs and "
               "trigger smart clone/fetch execution passes.{}",
               Style::Dim, Style::Reset);
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
  std::println("\n{}{}✨ Mokai Initializer {}{}– Create a new environment{}",
               Style::Green, Style::Reset, Style::Dim, Style::Reset,
               Style::Reset);
  std::println("{}──────────────────────────────────────────────────\n{}",
               Style::Dim, Style::Reset);

  std::string project_name = promptText("Project name", "my_mokai_project");
  fs::path target_dir = fs::current_path() / project_name;

  if (fs::exists(target_dir)) {
    return std::unexpected(CliError{CliError::Code::InvalidArguments,
                                    "Project creation aborted: Directory '" +
                                        project_name + "' already exists"});
  }

  std::vector<std::string> cpp_versions = {"c++11", "c++14", "c++17",
                                           "c++20", "c++23", "c++26"};
  size_t cpp_idx = promptChoice(
      "Select C++ Language Specification Target:", cpp_versions, 4);
  std::string chosen_cpp = cpp_versions[cpp_idx];

#ifdef MOKAI_TEMPLATE_DIR
  fs::path template_root = fs::path(MOKAI_TEMPLATE_DIR);
#else
  fs::path template_root = fs::current_path() / "src/templates";
#endif

  if (!fs::exists(template_root) || !fs::is_directory(template_root)) {
    return std::unexpected(CliError{
        CliError::Code::GeneralFailure,
        "Template Engine Error: blueprint resource directory not found at " +
            template_root.string()});
  }

  std::vector<std::string> available_templates;
  for (const auto &entry : fs::directory_iterator(template_root)) {
    if (entry.is_directory()) {
      available_templates.push_back(entry.path().filename().string());
    }
  }

  if (available_templates.empty()) {
    return std::unexpected(CliError{CliError::Code::GeneralFailure,
                                    "Template Engine Error: blueprint resource "
                                    "directory is completely empty"});
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

  std::print("\n{}⠋ Spawning environment scaffolding...{}\r", Style::Dim,
             Style::Reset);
  std::fflush(stdout);

  try {
    fs::create_directories(target_dir);
    fs::copy(selected_template_path, target_dir,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);
    processTemplatePlaceholders(target_dir / "mokai.toml", project_name,
                                chosen_cpp);
  } catch (const fs::filesystem_error &e) {
    return std::unexpected(
        CliError{CliError::Code::ProjectCreationDenied,
                 std::string("Filesystem structural operation I/O failure: ") +
                     e.what()});
  }

  if (init_git) {
    std::string git_cmd =
        "git init " + target_dir.string() + " > /dev/null 2>&1";
    std::system(git_cmd.c_str());
  }

  std::println("\r{}{}Project setup initialized perfectly!{}", Style::Green,
               Style::Success, Style::Reset);
  std::println("");
  std::println("  {}Location: {} {}", Style::Dim, Style::Reset,
               target_dir.string());
  std::println("  Navigate and trigger production builds via:");
  std::println("  {}cd {}{}", Style::Cyan, project_name, Style::Reset);
  std::println("  {}mokai Build{}", Style::Cyan, Style::Reset);
  return {};
}

void Cli::logSupportedCommands() {
  for (const auto &command : m_supported_commands) {
    std::println("    {}{}• {}{}", Style::Green, Style::Reset, command.first,
                 Style::Reset);
  }
  std::println("");
}

} // namespace mokai
