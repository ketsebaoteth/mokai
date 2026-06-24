#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mokai {

enum class OptimizationLevel {
  None,     // -O0 or /Od
  Optimize, // -O3 or /O2
};

struct CompilerOptions {
  OptimizationLevel opt_level = OptimizationLevel::None;
  bool generate_debug_symbols = true;
  bool position_independent =
      true; // Necessary for Linux shared libs, ignored by MSVC
  std::vector<std::string> custom_flags;
  std::vector<std::filesystem::path> include_dirs;
};

class IToolchainBackend {
public:
  virtual ~IToolchainBackend() = default;

  // Returns the system executable name/path for this toolchain (e.g. "clang++",
  // "g++", "cl.exe")
  virtual std::string GetCompilerExecutable() const = 0;
  virtual std::string GetArchiverExecutable() const = 0;
  virtual std::string GetLinkerExecutable() const = 0;

  /**
   * @brief Formats the complete command string to compile a single source file
   * into an object file.
   */
  virtual std::string
  BuildCompileCommand(const std::filesystem::path &src,
                      const std::filesystem::path &obj,
                      const CompilerOptions &options) const = 0;

  /**
   * @brief Formats the command string to bundle object files into a static
   * library archive.
   */
  virtual std::string BuildStaticArchiveCommand(
      const std::filesystem::path &output_lib,
      const std::vector<std::filesystem::path> &obj_files) const = 0;

  /**
   * @brief Formats the command string to link object files and libraries into
   * an executable or shared library.
   */
  virtual std::string
  BuildLinkCommand(const std::filesystem::path &output_binary,
                   const std::vector<std::filesystem::path> &obj_files,
                   const std::vector<std::filesystem::path> &lib_dependencies,
                   bool is_shared_library) const = 0;

  // Factory method to instantly provision the correct backend for the host OS
  static std::unique_ptr<IToolchainBackend> CreateHostNativeBackend();
};

} // namespace mokai
