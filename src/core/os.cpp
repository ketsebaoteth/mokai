#include "os.hpp"
#include <array>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#if defined(__linux__)
#define MOKAI_PLATFORM_LINUX
#elif defined(__APPLE__) && defined(__MACH__)
#define MOKAI_PLATFORM_MACOS
#elif defined(_WIN32) || defined(_WIN64)
#define MOKAI_PLATFORM_WINDOWS
#endif

namespace mokai {

Platform OS::GetCurrentPlatform() {
#if defined(MOKAI_PLATFORM_LINUX)
  return Platform::Linux;
#elif defined(MOKAI_PLATFORM_MACOS)
  return Platform::MacOS;
#elif defined(MOKAI_PLATFORM_WINDOWS)
  return Platform::Windows;
#else
  return Platform::Unknown;
#endif
}

std::string OS::GetPlatformName() {
  switch (GetCurrentPlatform()) {
  case Platform::Linux:
    return "Linux";
  case Platform::MacOS:
    return "macOS";
  case Platform::Windows:
    return "Windows";
  default:
    return "Unknown";
  }
}

std::filesystem::path OS::GetTemporaryDirectory() {
  // std::filesystem automatically queries $TMPDIR, $TMP, $TEMP, or /tmp based
  // on host OS
  return std::filesystem::temp_directory_path();
}

std::string OS::GetSharedLibraryExtension() {
#if defined(MOKAI_PLATFORM_WINDOWS)
  return ".dll";
#elif defined(MOKAI_PLATFORM_MACOS)
  return ".dylib";
#else
  return ".so";
#endif
}

std::string OS::GetStaticLibraryExtension() {
#if defined(MOKAI_PLATFORM_WINDOWS)
  return ".lib";
#else
  return ".a";
#endif
}

std::string OS::GetExecutableExtension() {
#if defined(MOKAI_PLATFORM_WINDOWS)
  return ".exe";
#else
  return "";
#endif
}

std::filesystem::path OS::FindExecutable(const std::string &name) {
  std::string command;
#if defined(MOKAI_PLATFORM_WINDOWS)
  command = "where " + name + " > nul 2>&1";
#else
  command = "which " + name + " > /dev/null 2>&1";
#endif

  // If the tool is visible via standard shell rules
  if (std::system(command.c_str()) == 0) {
    return name; // Standard execution name can be invoked directly via shell
  }
  return "";
}

int OS::ExecuteCommand(
    const std::string &command,
    const std::unordered_map<std::string, std::string> &env) {
  std::string full_expression;

#if defined(MOKAI_PLATFORM_WINDOWS)
  // On Windows, we append set commands sequentially utilizing the command
  // linking operator (&&)
  if (!env.empty()) {
    for (const auto &[key, value] : env) {
      full_expression += "set " + key + "=" + value + " && ";
    }
  }
  full_expression += command;
#else
  // On POSIX (Linux/macOS), inline prepended scoping acts natively: KEY=VALUE
  // cmd
  if (!env.empty()) {
    for (const auto &[key, value] : env) {
      full_expression += key + "=" + value + " ";
    }
  }
  full_expression += command;
#endif

  return std::system(full_expression.c_str());
}

} // namespace mokai
