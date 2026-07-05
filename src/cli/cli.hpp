#pragma once

#include "config/config.hpp"
#include "core/os.hpp"
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

inline constexpr std::string_view MOKAI_VERSION = "v0.0.6a";

namespace mokai {

enum class Verbosity { Quiet, Default, Verbose };
enum class ColorMode { Auto, Always, Never };

struct GlobalOptions {
  std::filesystem::path root_dir = std::filesystem::current_path();
  Verbosity verbosity = Verbosity::Default;
  ColorMode color = ColorMode::Auto;

  BuildProfile profile = BuildProfile::DEBUG_Lv2;
  OptimizationLevel level = OptimizationLevel::None;
  int job_count = 0;
  std::string target_filter = "";
  bool force_rebuild = false;

  Platform target_platform = OS::GetCurrentPlatform();
  std::vector<std::string> user_compiler_flags;
  std::string default_compiler;
};

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
  Cli() = default;
  ~Cli() = default;

  std::expected<std::monostate, CliError> ParseCliArgs(int argc, char *argv[]);
  int Run(int argc, char *argv[]);

private:
  struct ConfigCacheEntry {
    std::unique_ptr<Config> config;
    std::filesystem::file_time_type last_modified;
  };

  std::unordered_map<std::string, ConfigCacheEntry> m_config_registry;

  Config *getConfig(const std::filesystem::path &tomlPath);

  GlobalOptions m_options;
  std::unordered_map<std::string, CommandInfo> m_supported_commands;

  void initCommands();
  void logSupportedCommands();

  std::expected<std::monostate, CliError>
  handleHelp(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handleBuild(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handleRun(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handlePackageAdd(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handleCreateProject(const std::vector<std::string> &args);

  std::expected<std::monostate, CliError>
  handleVersion(const std::vector<std::string> &args);
};

} // namespace mokai
