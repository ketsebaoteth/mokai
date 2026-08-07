#include "cli.hpp"
#include "config/config.hpp"
#include "core/os.hpp"
#include "graph/graph.hpp"
#include "graph/types.hpp"
#include "log/log.h"
#include "templates/template.hpp"
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <conio.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace mokai {

namespace Color {
constexpr std::string_view Reset = "\033[0m";
constexpr std::string_view Dim = "\033[90m";
constexpr std::string_view Green = "\033[32m";
constexpr std::string_view Cyan = "\033[36m";
constexpr std::string_view Red = "\033[31m";
constexpr std::string_view Yellow = "\033[33m";
constexpr std::string_view Violet = "\033[35m";
} // namespace Color

namespace dx {

inline char toLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// Custom min for 3 values to replace <algorithm> std::min initializer_list
// overload
inline size_t min3(size_t a, size_t b, size_t c) {
  size_t m = a;
  if (b < m)
    m = b;
  if (c < m)
    m = c;
  return m;
}

// Lightweight Insertion Sort replacing <algorithm> std::sort template bloat
static void sortStrings(std::vector<std::string> &vec) {
  for (size_t i = 1; i < vec.size(); ++i) {
    std::string key = std::move(vec[i]);
    size_t j = i;
    while (j > 0 && vec[j - 1] > key) {
      vec[j] = std::move(vec[j - 1]);
      --j;
    }
    vec[j] = std::move(key);
  }
}

static std::string readFileToString(const fs::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open())
    return "";
  std::string content;
  auto size = file.tellg();
  if (size > 0) {
    content.resize(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    file.read(&content[0], size);
  }
  return content;
}

static size_t calculateDistance(std::string_view s1, std::string_view s2) {
  const size_t len1 = s1.size(), len2 = s2.size();
  std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));
  for (size_t i = 0; i <= len1; ++i)
    d[i][0] = i;
  for (size_t j = 0; j <= len2; ++j)
    d[0][j] = j;

  for (size_t i = 1; i <= len1; ++i) {
    for (size_t j = 1; j <= len2; ++j) {
      size_t cost =
          (toLowerAscii(s1[i - 1]) == toLowerAscii(s2[j - 1])) ? 0 : 1;
      d[i][j] = min3(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost);
    }
  }
  return d[len1][len2];
}

static std::string formatHint(const std::string &hint_text) {
  return "\n\n  Hint: " + hint_text;
}

static std::string
findClosestMatch(std::string_view input,
                 const std::vector<std::string_view> &options,
                 size_t max_dist = 3) {
  size_t min_dist = max_dist;
  std::string closest_match = "";
  for (const auto &opt : options) {
    size_t dist = calculateDistance(opt, input);
    if (dist < min_dist) {
      min_dist = dist;
      closest_match = opt;
    }
  }
  return closest_match;
}

static std::string findClosestManifestMatch(const fs::path &dir) {
  if (!fs::exists(dir) || !fs::is_directory(dir))
    return "";
  std::vector<std::string> files;
  try {
    for (const auto &entry : fs::directory_iterator(dir)) {
      if (entry.is_regular_file())
        files.push_back(entry.path().filename().string());
    }
  } catch (...) {
    return "";
  }

  std::vector<std::string_view> sv_options;
  sv_options.reserve(files.size());
  for (const auto &f : files)
    sv_options.push_back(f);
  return findClosestMatch("mokai.toml", sv_options, 4);
}

static std::string toLower(std::string str) {
  for (char &c : str)
    c = toLowerAscii(c);
  return str;
}

static void findAndReplaceOrInsert(std::string &content,
                                   const std::string &target,
                                   const std::string &fallback_section,
                                   const std::string &insertion) {
  size_t pos = content.find(target);
  if (pos != std::string::npos) {
    size_t end_line = content.find('\n', pos);
    if (end_line != std::string::npos) {
      content.replace(pos, end_line - pos, insertion);
      return;
    }
  }
  size_t sec_pos = content.find(fallback_section);
  if (sec_pos != std::string::npos) {
    content.insert(sec_pos + fallback_section.length(), "\n" + insertion);
  } else {
    content += "\n" + fallback_section + "\n" + insertion + "\n";
  }
}

} // namespace dx

void Cli::initCommands() {
  m_supported_commands["build"] = {
      "build [path]", "Compiles all dependencies and targets in parallel.",
      [this](const std::vector<std::string> &args) {
        return handleBuild(args);
      }};
  m_supported_commands["run"] = {
      "run [target_name] [args...]", "Builds and executes a target binary.",
      [this](const std::vector<std::string> &args) { return handleRun(args); }};
  m_supported_commands["create"] = {
      "create [project_name]", "Scaffolds a clean C++ workspace directory.",
      [this](const std::vector<std::string> &args) {
        return handleCreateProject(args);
      }};
  m_supported_commands["add"] = {
      "add [package]",
      "Adds a registry or local path dependency to mokai.toml.",
      [this](const std::vector<std::string> &args) {
        return handlePackageAdd(args);
      }};
  m_supported_commands["help"] = {
      "help [command]", "Displays usage and details for build commands.",
      [this](const std::vector<std::string> &args) {
        return handleHelp(args);
      }};
  m_supported_commands["version"] = {
      "version", "Displays the current release version of the build engine.",
      [this](const std::vector<std::string> &args) {
        return handleVersion(args);
      }};
}

int Cli::Run(int argc, char *argv[]) {
  initCommands();
  if (argc == 1) {
    std::println("{}mokai v{}{}", Color::Cyan, MOKAI_VERSION, Color::Reset);
    std::println("");
    logSupportedCommands();
    return static_cast<int>(ExitCode::UsageError);
  }

  std::string first_arg = argv[1];
  if (first_arg == "--help" || first_arg == "-h" || first_arg == "help") {
    if (argc > 2) {
      handleHelp({argv[2]});
    } else {
      std::println("{}mokai v{}{}", Color::Cyan, MOKAI_VERSION, Color::Reset);
      std::println("");
      logSupportedCommands();
      std::println("");
      std::println("{}Options:{}", Color::Cyan, Color::Reset);
      std::println(
          "  -h, --help          Show help details and optional parameters");
      std::println(
          "  -v, --verbose       Enable verbose engine debugging outputs");
      std::println("  -q, --quiet         Suppress non-error notifications");
      std::println("  --release           Compile targets inside production "
                   "release profiles");
      std::println(
          "  --clean, --no-cache Force complete target recompilation cycles");
      std::println(
          "  -j, --jobs <count>  Set parallel worker thread limits manually");
    }
    return static_cast<int>(ExitCode::Success);
  }

  auto result = ParseCliArgs(argc, argv);
  if (!result.has_value()) {
    Log::Error(result.error().message);
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
  PerfTimer timer;
  std::vector<std::string> rawArgs;
  rawArgs.reserve(argc - 1);
  for (int i = 1; i < argc; ++i)
    rawArgs.push_back(argv[i]);

  std::string command = "";
  std::vector<std::string> subCommandArgs;
  bool help_requested = false;

  for (size_t i = 0; i < rawArgs.size(); ++i) {
    const auto &arg = rawArgs[i];
    if (arg == "-v" || arg == "--verbose")
      m_options.verbosity = Verbosity::Verbose;
    else if (arg == "-q" || arg == "--quiet")
      m_options.verbosity = Verbosity::Quiet;
    else if (arg == "--release")
      m_options.profile = BuildProfile::RELEASE;
    else if (arg == "--debug")
      m_options.profile = BuildProfile::DEBUG;
    else if (arg == "--minsizerel")
      m_options.profile = BuildProfile::MINSIZEREL;
    else if (arg == "--no-cache" || arg == "--clean")
      m_options.force_rebuild = true;
    else if (arg == "-j" || arg == "--jobs") {
      if (i + 1 >= rawArgs.size())
        return std::unexpected(CliError{
            CliError::Code::InvalidArguments,
            "Option flag '" + arg + "' requires a numeric job count value"});
      try {
        m_options.job_count = std::stoi(rawArgs[++i]);
      } catch (...) {
        return std::unexpected(
            CliError{CliError::Code::InvalidArguments,
                     "Invalid numeric value provided for allocated jobs: '" +
                         rawArgs[i] + "'"});
      }
    } else if (arg == "--target") {
      if (i + 1 >= rawArgs.size())
        return std::unexpected(CliError{
            CliError::Code::InvalidArguments,
            "Option flag '--target' requires a valid target filter name"});
      m_options.target_filter = rawArgs[++i];
    } else if (arg == "--version" || arg == "-V")
      command = "version";
    else if (arg == "-h" || arg == "--help")
      help_requested = true;
    else if (arg.starts_with("-"))
      return std::unexpected(
          CliError{CliError::Code::UnknownCommand,
                   "Unrecognized command option: '" + arg + "'"});
    else {
      if (command.empty())
        command = arg;
      else
        subCommandArgs.push_back(arg);
    }
  }
  timer.Mark("CLI: Parsing Phase");

  if (help_requested)
    return command.empty() ? (logSupportedCommands(),
                              std::expected<std::monostate, CliError>{})
                           : handleHelp({command});
  if (command.empty())
    return std::unexpected(CliError{
        CliError::Code::UnknownCommand,
        "No operational command specified. Define an action to execute."});

  if (auto cmdToDispatch = m_supported_commands.find(command);
      cmdToDispatch != m_supported_commands.end()) {
    auto result = cmdToDispatch->second.callback(subCommandArgs);
    timer.Mark("CLI: Dispatch Execution");
    return result;
  }

  std::vector<std::string_view> cmd_options;
  for (const auto &[name, _] : m_supported_commands)
    cmd_options.push_back(name);
  std::string closest_cmd = dx::findClosestMatch(command, cmd_options, 3);

  std::string err = "Unknown command: '" + command + "'";
  if (!closest_cmd.empty())
    err += dx::formatHint("Did you mean 'mokai " + closest_cmd + "'?");
  timer.Mark("CLI: Typo Resolution");

  return std::unexpected(CliError{CliError::Code::UnknownCommand, err});
}

Config *Cli::getConfig(const fs::path &tomlPath) {
  std::string pathStr = fs::absolute(tomlPath).lexically_normal().string();
  bool exists = fs::exists(tomlPath);
  fs::file_time_type current_time;
  if (exists) {
    try {
      current_time = fs::last_write_time(tomlPath);
    } catch (...) {
      exists = false;
    }
  }

  auto it = m_config_registry.find(pathStr);
  if (it != m_config_registry.end()) {
    if (exists && it->second.last_modified == current_time)
      return it->second.config.get();
    if (exists) {
      it->second.last_modified = current_time;
      it->second.config =
          std::make_unique<Config>(tomlPath.parent_path().string(), m_options);
    } else {
      m_config_registry.erase(it);
      return nullptr;
    }
    return it->second.config.get();
  }

  if (exists) {
    ConfigCacheEntry entry;
    entry.last_modified = current_time;
    entry.config =
        std::make_unique<Config>(tomlPath.parent_path().string(), m_options);
    auto [insertedIt, success] =
        m_config_registry.emplace(pathStr, std::move(entry));
    return insertedIt->second.config.get();
  }
  return nullptr;
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
      if (break_pt == std::string::npos || break_pt < start)
        break_pt = start + current_limit;
      if (!first_line)
        std::print("{}", indent);
      std::println("{}", text.substr(start, break_pt - start));
      start = break_pt + 1;
      first_line = false;
    } else {
      if (!first_line)
        std::print("{}", indent);
      std::println("{}", text.substr(start));
      break;
    }
  }
}

std::expected<std::monostate, CliError>
Cli::handleVersion(const std::vector<std::string> &args) {
  std::println("{}", MOKAI_VERSION);
  return {};
}

std::expected<std::monostate, CliError>
Cli::handleHelp(const std::vector<std::string> &args) {
  if (args.empty()) {
    logSupportedCommands();
    return std::unexpected(CliError{CliError::Code::InvalidArguments,
                                    "No command specified for help lookup."});
  }

  static const std::unordered_map<std::string_view, std::string_view>
      detailed_descriptions = {
          {"build",
           "Compiles all project targets and dependencies matching graph "
           "configurations. Automatically parses manifests, handles "
           "glob-patterns, re-evaluates header updates, and compiles object "
           "targets in parallel via system worker pools."},
          {"run",
           "Builds and launches a compiled executable binary target. If no "
           "target argument is specified, the default target configured in "
           "mokai.toml is triggered automatically. Trailing arguments are "
           "forwarded cleanly to the executing sub-process."},
          {"create",
           "Launches the interactive initialization wizard to scaffold a clean "
           "C++ workspace layout. Generates mokai.toml manifests, file "
           "directories, main entrypoints, and initiates version control."},
          {"add", "Resolves and links a dependency package. Checks matching "
                  "versions inside local and global package indexes, updates "
                  "dependencies blocks inside local mokai.toml, and registers "
                  "libraries with build contexts."},
          {"help",
           "Displays explicit parameter limits, operational details, option "
           "flag usage, and description overviews for each build action."},
          {"version",
           "Logs the current release flag version of the build tool."}};

  std::string_view cmd =
      (args[0] == "--version") ? "version" : std::string_view(args[0]);

  if (auto help = m_supported_commands.find(std::string(cmd));
      help != m_supported_commands.end()) {
    std::println("\n Command: {}", help->first);
    std::println(" Usage:   {}", help->second.usage);
    std::print(" Details: ");

    std::string_view explanation = help->second.explanation;
    if (auto it = detailed_descriptions.find(cmd);
        it != detailed_descriptions.end())
      explanation = it->second;

    printWrapped(explanation, 80, "          ");
    return {};
  } else {
    std::vector<std::string_view> cmd_options;
    for (const auto &[name, _] : m_supported_commands)
      cmd_options.push_back(name);
    std::string closest_cmd = dx::findClosestMatch(args[0], cmd_options, 3);

    std::string err = "Command not found: '" + args[0] + "'";
    if (!closest_cmd.empty())
      err += dx::formatHint("Did you mean to run 'help " + closest_cmd + "'?");
    return std::unexpected(CliError{CliError::Code::UnknownCommand, err});
  }
}

std::expected<std::monostate, CliError>
Cli::handleBuild(const std::vector<std::string> &args) {
  fs::path workingDir = args.empty() ? fs::current_path() : fs::path(args[0]);
  if (!fs::exists(workingDir) || !fs::is_directory(workingDir)) {
    return std::unexpected(
        CliError{CliError::Code::InvalidWorkspace,
                 "Specified path is not a directory: " + workingDir.string()});
  }

  workingDir = fs::absolute(workingDir).lexically_normal();
  Config *currentConfig = getConfig(workingDir / "mokai.toml");

  if (!currentConfig) {
    std::string err =
        "Could not find a valid 'mokai.toml' file in: " + workingDir.string();
    std::string closest = dx::findClosestManifestMatch(workingDir);
    err +=
        !closest.empty()
            ? dx::formatHint("A file named '" + closest +
                             "' was found. Correct the typo if necessary.")
            : dx::formatHint("Run 'mokai create' to generate a new project.");
    return std::unexpected(CliError{CliError::Code::InvalidWorkspace, err});
  }

  if (m_options.verbosity != Verbosity::Quiet) {
    Log::Info(std::format("Initializing build target pipeline for {}...",
                          OS::GetPlatformName()));
  }

  auto graph_result = Graph::Create(
      std::make_shared<ProjectManifest>(currentConfig->getManifest()),
      m_options);
  if (!graph_result) {
    Log::Error("Failed to initialize build dependency graph: " +
               graph_result.error());
    return {};
  }

  Graph graph = std::move(graph_result.value());
  auto buildOrder = graph.computeBuildOrder(graph.getEdges());
  if (buildOrder.empty())
    return std::unexpected(
        CliError{CliError::Code::BuildFailed,
                 "Cyclic or invalid target dependencies detected."});
  if (!graph.BuildAllTree(buildOrder))
    return std::unexpected(
        CliError{CliError::Code::BuildFailed,
                 "Compilation sequence aborted due to errors."});

  if (m_options.verbosity != Verbosity::Quiet)
    Log::Success("Build completed successfully.");
  return {};
}

std::expected<std::monostate, CliError>
Cli::handleRun(const std::vector<std::string> &args) {
  fs::path workingDir = fs::current_path();
  Config *currentConfig = getConfig(workingDir / "mokai.toml");
  if (!currentConfig) {
    std::string err = "Workspace manifest context not found.";
    std::string closest = dx::findClosestManifestMatch(workingDir);
    if (!closest.empty())
      err += dx::formatHint("A file named '" + closest +
                            "' was found. Correct the typo if necessary.");
    return std::unexpected(CliError{CliError::Code::InvalidWorkspace, err});
  }

  auto manifest = currentConfig->getManifest();
  const Target *chosenTarget = nullptr;
  std::vector<std::string> forwardArgs;

  if (!args.empty() && !args[0].starts_with("-")) {
    std::string explicitTargetName = args[0];
    for (const auto &target : manifest.targets) {
      if (target.name == explicitTargetName) {
        chosenTarget = &target;
        break;
      }
    }

    if (!chosenTarget) {
      std::vector<std::string_view> target_options;
      for (const auto &t : manifest.targets)
        target_options.push_back(t.name);
      std::string closest_target =
          dx::findClosestMatch(explicitTargetName, target_options, 4);

      std::string err =
          "No target named '" + explicitTargetName + "' inside mokai.toml.";
      if (!closest_target.empty())
        err += dx::formatHint("Did you mean target '" + closest_target + "'?");
      return std::unexpected(CliError{CliError::Code::InvalidArguments, err});
    }
    for (size_t i = 1; i < args.size(); ++i)
      forwardArgs.push_back(args[i]);
  } else {
    for (const auto &target : manifest.targets) {
      if (target.is_default && target.type == TargetType::Executable) {
        chosenTarget = &target;
        break;
      }
    }
    if (!chosenTarget) {
      for (const auto &target : manifest.targets) {
        if (target.type == TargetType::Executable) {
          chosenTarget = &target;
          break;
        }
      }
    }
    forwardArgs = args;
  }

  if (!chosenTarget)
    return std::unexpected(
        CliError{CliError::Code::InvalidArguments,
                 "No executable target maps found inside the manifest."});
  if (chosenTarget->type != TargetType::Executable) {
    return std::unexpected(CliError{
        CliError::Code::InvalidArguments,
        "Target '" + chosenTarget->name + "' is not an executable type." +
            dx::formatHint("Libraries cannot be executed directly.")});
  }

  auto buildRes = handleBuild({});
  if (!buildRes.has_value())
    return buildRes;

  std::string profileFolder =
      (m_options.profile == BuildProfile::RELEASE) ? "release" : "debug";
  fs::path binaryPath =
      fs::path("./build") / profileFolder / chosenTarget->name;

#if defined(_WIN32) || defined(_WIN64)
  binaryPath.replace_extension(".exe");
#endif

  if (!fs::exists(binaryPath)) {
    return std::unexpected(CliError{
        CliError::Code::GeneralFailure,
        "Compiled target binary not found at path: " + binaryPath.string()});
  }

  if (m_options.verbosity != Verbosity::Quiet)
    Log::Info(std::format("Executing target binary: {}", binaryPath.string()));

  std::string execCommand = binaryPath.string();
  for (const auto &fArg : forwardArgs)
    execCommand += " " + fArg;

  std::unordered_map<std::string, std::string> emptyEnv;
  int runtimeExitCode = OS::ExecuteCommand(execCommand, emptyEnv);
  if (runtimeExitCode != 0)
    Log::Error(std::format("Process terminated with non-zero exit code: {}",
                           runtimeExitCode));

  return {};
}

struct TerminalRawGuard {
#if !defined(_WIN32) && !defined(_WIN64)
  struct termios orig;
  TerminalRawGuard() {
    tcgetattr(STDIN_FILENO, &orig);
    struct termios raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  }
  ~TerminalRawGuard() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig); }
#else
  TerminalRawGuard() {}
  ~TerminalRawGuard() {}
#endif
};

static size_t promptChoice(const std::string &title,
                           const std::vector<std::string> &options,
                           size_t default_idx = 0) {
  if (options.empty())
    return 0;
  size_t current_idx = default_idx;
  const size_t max_visible = 5;
  bool selecting = true;
  bool first_render = true;

  std::print("\033[?25l");
  {
    TerminalRawGuard raw_mode_guard;
    while (selecting) {
      size_t start_idx =
          (current_idx >= max_visible) ? (current_idx - max_visible + 1) : 0;
      if (!first_render) {
        size_t lines_to_clear = std::min(options.size(), max_visible) + 2;
        for (size_t i = 0; i < lines_to_clear; ++i)
          std::print("\033[A\033[2K");
      }
      first_render = false;

      std::println("{}{} (Select using arrow keys or j/k){}", Color::Violet,
                   title, Color::Reset);
      for (size_t i = start_idx;
           i < std::min(options.size(), start_idx + max_visible); ++i) {
        std::println("{}", (i == current_idx)
                               ? std::format("  > {}", options[i])
                               : std::format("    {}", options[i]));
      }

      std::print("  (Use up/down to navigate, Enter to select");
      if (options.size() > max_visible)
        std::print(" | item {}/{}", (current_idx + 1), options.size());
      std::println(")");
      std::fflush(stdout);

#if defined(_WIN32) || defined(_WIN64)
      int ch = _getch();
      if (ch == 0 || ch == 0xE0) {
        ch = _getch();
        if (ch == 72 && current_idx > 0)
          current_idx--;
        else if (ch == 80 && current_idx < options.size() - 1)
          current_idx++;
      } else if (ch == '\r' || ch == '\n')
        selecting = false;
      else if (ch == 'k' && current_idx > 0)
        current_idx--;
      else if (ch == 'j' && current_idx < options.size() - 1)
        current_idx++;
      else if (ch == 3) {
        std::print("\033[?25h");
        std::println("Interrupted.");
        std::exit(130);
      }
#else
      char ch;
      if (read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == '\n' || ch == '\r')
          selecting = false;
        else if (ch == 'k' && current_idx > 0)
          current_idx--;
        else if (ch == 'j' && current_idx < options.size() - 1)
          current_idx++;
        else if (ch == '\033') {
          char seq[2];
          if (read(STDIN_FILENO, &seq[0], 1) == 1 &&
              read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[0] == '[' && seq[1] == 'A' && current_idx > 0)
              current_idx--;
            else if (seq[0] == '[' && seq[1] == 'B' &&
                     current_idx < options.size() - 1)
              current_idx++;
          }
        } else if (ch == 3) {
          std::print("\033[?25h");
          std::println("Interrupted.");
          std::exit(130);
        }
      }
#endif
    }
  }

  std::print("\033[?25h");
  size_t lines_to_clear = std::min(options.size(), max_visible) + 2;
  for (size_t i = 0; i < lines_to_clear; ++i)
    std::print("\033[A\033[2K");
  std::println("{}> {}: {}{}", Color::Violet, title, options[current_idx],
               Color::Reset);

  return current_idx;
}

static std::string promptText(const std::string &prompt,
                              const std::string &default_val = "") {
  std::print("{}> {}{}", Color::Violet, prompt, Color::Reset);
  if (!default_val.empty())
    std::print(" ({})", default_val);
  std::print(": ");
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
    std::string err =
        "Manifest context not found inside current root directories.";
    std::string closest = dx::findClosestManifestMatch(fs::current_path());
    if (!closest.empty())
      err += dx::formatHint("Found similar file '" + closest +
                            "'. Check file name for typos.");
    return std::unexpected(CliError{CliError::Code::InvalidWorkspace, err});
  }

  std::string chosenPackage = "";
  if (!args.empty()) {
    chosenPackage = args[0];
  } else {
    fs::path registryDir =
        OS::GetTemporaryDirectory().parent_path() / ".mokai" / "registry";
    if (!fs::exists(registryDir) || fs::is_empty(registryDir)) {
      Log::Warn("Package index uninitialized. Build target to sync files.");
      chosenPackage =
          promptText("Enter package mapping manually (e.g. sfml@3.0.0)");
    } else {
      std::vector<std::string> availablePackages;
      for (const auto &entry : fs::directory_iterator(registryDir)) {
        if (!entry.is_directory() && entry.path().extension() == ".toml")
          availablePackages.push_back(entry.path().stem().string());
      }
      dx::sortStrings(availablePackages);

      if (availablePackages.empty()) {
        chosenPackage = promptText(
            "No packages in registry index. Enter package name manually");
      } else {
        size_t pkgIdx = promptChoice(
            "Select target workspace dependency package:", availablePackages,
            0);
        std::string pkgName = availablePackages[pkgIdx];

        std::string rContent =
            dx::readFileToString(registryDir / (pkgName + ".toml"));

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
              bool duplicate = false;
              for (const auto &opt : constraintOptions) {
                if (opt == vRange) {
                  duplicate = true;
                  break;
                }
              }
              if (!duplicate)
                constraintOptions.push_back(vRange);
            }
          }
          pos += 13;
        }

        if (constraintOptions.empty()) {
          std::string customVer =
              promptText("No semantic version ranges matched. Enter explicit "
                         "build target version");
          chosenPackage =
              customVer.empty() ? pkgName : pkgName + "@" + customVer;
        } else {
          constraintOptions.push_back("Enter manual spec constraint...");
          size_t verIdx =
              promptChoice("Select matching semantic version limit constraint:",
                           constraintOptions, 0);

          if (constraintOptions[verIdx] == "Enter manual spec constraint...") {
            std::string customVer =
                promptText("Specify target version constraint (e.g. >=1.2.0)");
            chosenPackage =
                customVer.empty() ? pkgName : pkgName + "@" + customVer;
          } else {
            std::string chosenRange = constraintOptions[verIdx];
            std::string cleanVer =
                promptText("Specify exact target version compatible with (" +
                               chosenRange + ")",
                           "1.0.0");
            chosenPackage = pkgName + "@" + cleanVer;
          }
        }
      }
    }
  }

  if (chosenPackage.empty())
    return std::unexpected(
        CliError{CliError::Code::PackageNotFound,
                 "Aborted package resolution: Empty package specifier."});

  std::string tomlContent = dx::readFileToString(tomlPath);
  size_t depPos = tomlContent.find("dependencies = [");
  if (depPos != std::string::npos) {
    size_t closeBrace = tomlContent.find(']', depPos);
    if (closeBrace != std::string::npos)
      tomlContent.insert(closeBrace, "\n    \"" + chosenPackage + "\",");
  } else {
    dx::findAndReplaceOrInsert(tomlContent, "dependencies = [", "[project]",
                               "dependencies = [\n    \"" + chosenPackage +
                                   "\"\n]");
  }

  std::ofstream outFile(tomlPath.string(), std::ios::binary);
  if (outFile.is_open()) {
    outFile << tomlContent;
    outFile.close();
  }
  Log::Success(std::format("Successfully injected target dependency: {}",
                           chosenPackage));
  return {};
}

std::expected<std::monostate, CliError>
Cli::handleCreateProject(const std::vector<std::string> &args) {
  std::string project_name;
  std::println("\n{}Mokai Project Scaffolding{}", Color::Violet, Color::Reset);
  std::println("{}====================================={}", Color::Dim,
               Color::Reset);

  if (!args.empty() && !args[0].empty()) {
    project_name = args[0];
    std::println("{}> Project name: {} [Passed via arguments]{}", Color::Violet,
                 project_name, Color::Reset);
  } else {
    project_name = promptText("Project name", "my_mokai_project");
  }

  fs::path target_dir = fs::current_path() / project_name;
  if (fs::exists(target_dir)) {
    return std::unexpected(
        CliError{CliError::Code::InvalidArguments,
                 "Workspace configuration error: Path already exists: '" +
                     project_name + "'"});
  }

  std::vector<std::string> lang_families = {"C++", "C"};
  size_t lang_idx =
      promptChoice("Select programming language family", lang_families, 0);
  std::string chosen_lang = lang_families[lang_idx];

  std::string chosen_std;
  if (chosen_lang == "C++") {
    std::vector<std::string> cpp_standards = {"C++11", "C++14", "C++17",
                                              "C++20", "C++23", "C++26"};
    chosen_std = cpp_standards[promptChoice("Select C++ standard specification",
                                            cpp_standards, 4)];
  } else {
    std::vector<std::string> c_standards = {"C89", "C99", "C11", "C17", "C23"};
    chosen_std = c_standards[promptChoice("Select C standard specification",
                                          c_standards, 2)];
  }

  TemplateGen template_engine;
  auto raw_available = template_engine.getAvailableTemplates();
  std::string chosen_template = "console"; // Default fallback template
  if (!raw_available.empty()) {
    std::vector<std::string> available_templates;
    for (const auto &[name, desc] : raw_available)
      available_templates.push_back(name + " (" + desc + ")");
    dx::sortStrings(available_templates);

    // Call promptChoice ONCE to get selected index
    size_t selected_idx =
        promptChoice("Select matching embedded layout blueprint template",
                     available_templates, 0);

    // Remap selected choice back to raw_available template key
    for (const auto &[name, desc] : raw_available) {
      if (available_templates[selected_idx].starts_with(name)) {
        chosen_template = name;
        break;
      }
    }
  }
  std::vector<std::string> git_options = {"Yes", "No"};
  bool init_git =
      (promptChoice("Initialize default git repository control index?",
                    git_options, 0) == 0);

  std::print("\nSpawning project workspace layout...\r");
  std::fflush(stdout);

  std::string std_for_creation =
      dx::toLower((chosen_lang == "C++") ? chosen_std : "c++23");

  if (!template_engine.create(chosen_template, target_dir, project_name,
                              std_for_creation)) {
    return std::unexpected(CliError{
        CliError::Code::ProjectCreationDenied,
        "Failed writing blueprint layout template files to target path."});
  }

  if (init_git) {
    std::unordered_map<std::string, std::string> clean_env;
    OS::ExecuteCommand("git init --initial-branch=main " + target_dir.string(),
                       clean_env);
    std::ofstream gitignore(target_dir / ".gitignore");
    if (gitignore.is_open()) {
      gitignore << "build/\nobj/\n.mokai/"
                   "\ncompile_commands.json\n*.o\n*.obj\n*.a\n*.lib\n*.so\n*."
                   "dll\n*.dylib\n*.exe\n*.d\n.vscode/\n.vs/\nout/\n";
      gitignore.close();
    }
  }

  fs::path toml_path = target_dir / "mokai.toml";
  std::string content = dx::readFileToString(toml_path);
  if (!content.empty()) {
    std::string std_val = dx::toLower(chosen_std);
    if (chosen_lang == "C++") {
      dx::findAndReplaceOrInsert(content, "cpp_version =", "[project]",
                                 std::format("cpp_version = \"{}\"", std_val));
    } else {
      dx::findAndReplaceOrInsert(content, "c_version = ", "[project]",
                                 std::format("c_version = \"{}\"", std_val));
    }
    std::ofstream toml_out(toml_path, std::ios::binary);
    if (toml_out.is_open()) {
      toml_out << content;
      toml_out.close();
    }
  }

  Log::Success("Project workspace generated successfully.");
  std::println("\n  {}Location:        {}{}\n  {}Language Family: {}{}\n  "
               "{}Specification:   {}{}\n  {}Template:        {}{}\n",
               Color::Violet, Color::Reset, target_dir.string(), Color::Violet,
               Color::Reset, chosen_lang, Color::Violet, Color::Reset,
               chosen_std, Color::Violet, Color::Reset, chosen_template);
  std::println("  Navigate and trigger a compilation build via:\n  {}cd {}{}\n "
               " {}mokai build{}",
               Color::Cyan, project_name, Color::Reset, Color::Cyan,
               Color::Reset);

  return {};
}

void Cli::logSupportedCommands() {
  std::println("Commands:");
  auto print_cmd = [](std::string_view name, std::string_view desc) {
    std::println("  {}{:<10}{} - {}", Color::Violet, name, Color::Reset, desc);
  };
  print_cmd("add", "Adds a registry or local path dependency to mokai.toml.");
  print_cmd("build", "Compiles all dependencies and targets in parallel.");
  print_cmd("create", "Scaffolds a clean C++ workspace directory.");
  print_cmd("help", "Displays usage and details for build commands.");
  print_cmd("run", "Builds and executes a target binary.");
  print_cmd("version", "Displays the current release version.");
}

} // namespace mokai
