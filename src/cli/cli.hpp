#pragma once
#include "core/os.hpp"
#include "exp.hpp"
#include "log/log.h"
#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

inline constexpr std::string_view MOKAI_VERSION = "v0.0.1a";

namespace mokai {

enum class Verbosity { Quiet, Default, Verbose };
enum class ColorMode { Auto, Always, Never };

namespace Style {
inline constexpr std::string_view Reset = "\033[0m";
inline constexpr std::string_view Bold = "\033[1m";
inline constexpr std::string_view Dim = "\033[90m";
inline constexpr std::string_view Green = "\033[38;5;42m";
inline constexpr std::string_view Cyan = "\033[36m";
inline constexpr std::string_view Red = "\033[31m";
inline constexpr std::string_view Yellow = "\033[33m";
inline constexpr std::string_view Arrow = "❯ ";
inline constexpr std::string_view Success = "✔ ";
inline constexpr std::string_view Info = "ℹ ";
inline constexpr std::string_view Error = "✖ ";
} // namespace Style

enum class BuildProfile { Debug, Release };

struct GlobalOptions {
  std::filesystem::path root_dir = std::filesystem::current_path();
  Verbosity verbosity = Verbosity::Default;
  ColorMode color = ColorMode::Auto;

  // Core Engine Refactoring Flags
  BuildProfile profile = BuildProfile::Debug;
  int job_count = 0;
  std::string target_filter = "";
  bool force_rebuild = false;

  // Cross-Platform Mapping Adjustments
  Platform target_platform = OS::GetCurrentPlatform();

  // Custom compiler/toolchain flag overrides configured during CLI ingestion
  // loops
  std::vector<std::string> user_compiler_flags;
};

// based on POSIX standard
enum class ExitCode : int {
  Success = 0,
  GeneralFailure = 1,
  UsageError = 2,
  CommandNotFound = 127
};

struct CliError {
  enum class Code {
    UnknownCommand,
    InvalidArguments,
    BuildFailed,
    PackageNotFound,
    InvalidWorkspace,
    GeneralFailure,
    ProjectCreationDenied
  };

  Code code;
  std::string message;
};

struct CommandInfo {
  std::string_view usage;
  std::string_view explanation;
  std::function<std::expected<std::monostate, CliError>(
      const std::vector<std::string> &)>
      callback;
};

class Cli {
public:
  Cli();
  ~Cli() = default;
  std::expected<std::monostate, CliError> ParseCliArgs(int argc, char *argv[]);
  int Run(int argc, char *argv[]);

private:
  log::Logger m_logger;
  GlobalOptions m_options;
  std::unordered_map<std::string, CommandInfo> m_supported_commands;

  void initCommands();
  void logSupportedCommands();

  std::expected<std::monostate, CliError>
  handleHelp(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handleBuild(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handlePackageAdd(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handleCreateProject(const std::vector<std::string> &args);
};
} // namespace mokai
