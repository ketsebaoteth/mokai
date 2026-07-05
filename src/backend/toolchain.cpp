#include "toolchain.hpp"
#include "../core/os.hpp"
#include <sstream>

namespace mokai {

class PosixToolchainBackend : public IToolchainBackend {
public:
  std::string GetCompilerExecutable() const override { return "clang++"; }

  std::string GetArchiverExecutable() const override { return "ar"; }

  std::string GetLinkerExecutable() const override { return "clang++"; }

  std::string
  BuildCompileCommand(const std::filesystem::path &src,
                      const std::filesystem::path &obj,
                      const CompilerOptions &options) const override {
    std::ostringstream cmd;
    cmd << GetCompilerExecutable() << " -c " << src.string() << " -o "
        << obj.string();

    // INFO: -flto for Link Time Optimization
    if (options.opt_level == OptimizationLevel::Perf_Slight) {
      cmd << " -O1 -DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Perf_Moderate) {
      cmd << " -O3 -DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Perf_Max) {
      cmd << " -Ofast -flto -DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Space_Moderate) {
      cmd << " -Os -DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Space_Max) {
      cmd << " -Oz -flto -DNDEBUG";
    } else {
      cmd << " -O0";
    }

    if (options.generate_debug_symbols) {
      cmd << " -g";
    }

    if (options.position_independent) {
      cmd << " -fPIC";
    }

    for (const auto &dir : options.include_dirs) {
      cmd << " -I" << dir.string();
    }

    for (const auto &dir : options.sys_include_dirs) {
      cmd << " -i" << dir.string();
    }

    for (const auto &flag : options.custom_flags) {
      cmd << " " << flag;
    }

    return cmd.str();
  }

  std::string BuildStaticArchiveCommand(
      const std::filesystem::path &output_lib,
      const std::vector<std::filesystem::path> &obj_files) const override {
    std::ostringstream cmd;
    cmd << GetArchiverExecutable() << " rcs " << output_lib.string();
    for (const auto &obj : obj_files) {
      cmd << " " << obj.string();
    }
    return cmd.str();
  }

  std::string
  BuildLinkCommand(const std::filesystem::path &output_binary,
                   const std::vector<std::filesystem::path> &obj_files,
                   const std::vector<std::filesystem::path> &lib_dependencies,
                   bool is_shared_library) const override {
    std::ostringstream cmd;
    cmd << GetLinkerExecutable();

    for (const auto &obj : obj_files) {
      cmd << " " << obj.string();
    }

    cmd << " -o " << output_binary.string();

    if (is_shared_library) {
      cmd << " -shared";
    }

    // Link dependencies
    for (const auto &lib : lib_dependencies) {
      cmd << " " << lib.string();
    }

    return cmd.str();
  }
};

// ============================================================================
// Windows (MSVC) Backend Implementation
// ============================================================================
class MsvcToolchainBackend : public IToolchainBackend {
public:
  std::string GetCompilerExecutable() const override { return "cl.exe"; }
  std::string GetArchiverExecutable() const override { return "lib.exe"; }
  std::string GetLinkerExecutable() const override { return "link.exe"; }

  std::string
  BuildCompileCommand(const std::filesystem::path &src,
                      const std::filesystem::path &obj,
                      const CompilerOptions &options) const override {
    std::ostringstream cmd;
    // /nologo suppresses banner info, /c compiles without linking, /Fo sets
    // object path
    cmd << "cl.exe /nologo /c " << src.string() << " /Fo" << obj.string();

    if (options.opt_level == OptimizationLevel::Perf_Moderate) {
      cmd << " /O2 /DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Perf_Max) {
      cmd << " /fp:fast /DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Space_Moderate) {
      cmd << " /Os /DNDEBUG";
    } else if (options.opt_level == OptimizationLevel::Space_Max) {
      cmd << " /O1 /DNDEBUG";
    } else {
      cmd << " /Od";
    }

    if (options.generate_debug_symbols) {
      cmd << " /Zi";
    }

    for (const auto &dir : options.include_dirs) {
      cmd << " /I" << dir.string();
    }

    for (const auto &dir : options.sys_include_dirs) {
      cmd << " /i" << dir.string();
    }

    for (const auto &flag : options.custom_flags) {
      cmd << " " << flag;
    }

    return cmd.str();
  }

  std::string BuildStaticArchiveCommand(
      const std::filesystem::path &output_lib,
      const std::vector<std::filesystem::path> &obj_files) const override {
    std::ostringstream cmd;
    cmd << "lib.exe /nologo /OUT:" << output_lib.string();
    for (const auto &obj : obj_files) {
      cmd << " " << obj.string();
    }
    return cmd.str();
  }

  std::string
  BuildLinkCommand(const std::filesystem::path &output_binary,
                   const std::vector<std::filesystem::path> &obj_files,
                   const std::vector<std::filesystem::path> &lib_dependencies,
                   bool is_shared_library) const override {
    std::ostringstream cmd;
    cmd << "link.exe /nologo";

    for (const auto &obj : obj_files) {
      cmd << " " << obj.string();
    }

    cmd << " /OUT:" << output_binary.string();

    if (is_shared_library) {
      cmd << " /DLL";
    }

    for (const auto &lib : lib_dependencies) {
      cmd << " " << lib.string();
    }

    return cmd.str();
  }
};

// ============================================================================
// Factory Implementation
// ============================================================================
std::unique_ptr<IToolchainBackend>
IToolchainBackend::CreateHostNativeBackend() {
  if (OS::GetCurrentPlatform() == Platform::Windows) {
    return std::make_unique<MsvcToolchainBackend>();
  }
  // Linux and macOS share POSIX flags format for standard GCC/Clang toolchains
  return std::make_unique<PosixToolchainBackend>();
}

} // namespace mokai
