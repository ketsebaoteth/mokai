#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

namespace mokai {

enum class Platform { Linux, MacOS, Windows, Unknown };

class OS {
public:
  // Returns the active platform compile-time target
  static Platform GetCurrentPlatform();
  static std::string GetPlatformName();

  // Cross-platform temporary directory handling (replaces hardcoded /tmp/)
  static std::filesystem::path GetTemporaryDirectory();

  // Resolves platform-specific dynamic/shared library naming conventions
  static std::string GetSharedLibraryExtension();
  static std::string GetStaticLibraryExtension();
  static std::string GetExecutableExtension();

  // Locates a tool on the system PATH (replaces hardcoded "which")
  static std::filesystem::path FindExecutable(const std::string &name);

  /**
   * @brief Spawns a shell command with a custom isolated environment map.
   * Handles inline variables securely on Windows (cmd.exe) and Unix (sh/bash).
   * * @param command The execution string or path to binary.
   * @param env Key-value pairs representing environmental configurations.
   * @return The exit code of the executed process.
   */
  static int
  ExecuteCommand(const std::string &command,
                 const std::unordered_map<std::string, std::string> &env = {});
};

} // namespace mokai
