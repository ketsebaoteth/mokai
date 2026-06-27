#include "ToolChainFinder.hpp"
#include "ClangCompiler.hpp"
#include "GccCompiler.hpp"
#include "MsvcCompiler.hpp"
#include "log/log.h"
#include <expected>
#include <filesystem>
#include <memory>
#include <unistd.h>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mokai {
ToolchainFinder::ToolchainFinder(mokai::log::Logger &logger)
    : m_logger(logger) {}

std::string ToolchainFinder::findBinary(const std::string &name) const {
#ifdef _WIN32
  return findWindowsBinary(name);
#else
  return findUnixBinary(name);
#endif
}
namespace fs = std::filesystem;

std::string ToolchainFinder::findUnixBinary(const std::string &name) const {
#ifndef _WIN32
  if (name.find('/') != std::string::npos) {
    fs::path p(name);
    if (fs::exists(p) && !fs::is_directory(p)) {
      if (::access(p.c_str(), X_OK) == 0) {
        return fs::absolute(p).string();
      }
    }
    return "";
  }

  const char *path_env = std::getenv("PATH");
  if (!path_env) {
    return "";
  }

  std::string path_str(path_env);
  std::stringstream ss(path_str);
  std::string dir;
  std::vector<std::string> candidates;

  while (std::getline(ss, dir, ':')) {
    if (dir.empty())
      continue;

    fs::path dir_path(dir);
    if (!fs::exists(dir_path) || !fs::is_directory(dir_path))
      continue;

    fs::path direct_match = dir_path / name;
    if (fs::exists(direct_match) && !fs::is_directory(direct_match)) {
      if (::access(direct_match.c_str(), X_OK) == 0) {
        candidates.push_back(fs::absolute(direct_match).string());
      }
    }

    try {
      for (const auto &entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file())
          continue;

        std::string filename = entry.path().filename().string();
        if (filename.rfind(name + "-", 0) == 0) {
          if (::access(entry.path().c_str(), X_OK) == 0) {
            candidates.push_back(fs::absolute(entry.path()).string());
          }
        }
      }
    } catch (...) {
      continue;
    }
  }

  if (candidates.empty()) {
    return "";
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const std::string &a, const std::string &b) { return a > b; });

  return candidates.front();
#endif
}

std::string ToolchainFinder::findWindowsBinary(const std::string &name) const {
#ifdef _WIN32
  if (name.find('\\') != std::string::npos ||
      name.find('/') != std::string::npos) {
    fs::path p(name);
    if (fs::exists(p) && !fs::is_directory(p)) {
      if (::_access(p.string().c_str(), 0) == 0) {
        return fs::absolute(p).string();
      }
    }
    return "";
  }

  const char *path_env = std::getenv("PATH");
  if (path_env) {
    std::string path_str(path_env);
    std::stringstream ss(path_str);
    std::string dir;
    while (std::getline(ss, dir, ';')) {
      if (dir.empty())
        continue;
      fs::path candidate = fs::path(dir) / name;
      if (name.find('.') == std::string::npos && !fs::exists(candidate)) {
        candidate.replace_extension(".exe");
      }
      if (fs::exists(candidate) && !fs::is_directory(candidate)) {
        if (::_access(candidate.string().c_str(), 0) == 0) {
          return fs::absolute(candidate).string();
        }
      }
    }
  }

  if (name == "cl" || name == "cl.exe" || name == "lib" || name == "lib.exe") {
    const char *pf86 = std::getenv("ProgramFiles(x86)");
    if (pf86) {
      fs::path vswhere_path = fs::path(pf86) / "Microsoft Visual Studio" /
                              "Installer" / "vswhere.exe";
      if (fs::exists(vswhere_path)) {
        std::string pattern =
            (name == "cl" || name == "cl.exe")
                ? "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\cl.exe"
                : "VC\\Tools\\MSVC\\**\\bin\\Hostx64\\x64\\lib.exe";

        std::string cmd =
            "\"" + vswhere_path.string() +
            "\" -latest -products * -requires "
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find " +
            pattern;

#if defined(_MSC_VER)
        FILE *pipe =
            _wpopen(std::wstring(cmd.begin(), cmd.end()).c_str(), L"r");
#else
        FILE *pipe = popen(cmd.c_str(), "r");
#endif

        if (pipe) {
          std::array<char, 512> buffer;
          std::string result;
          while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
          }

#if defined(_MSC_VER)
          _pclose(pipe);
#else
          pclose(pipe);
#endif

          if (!result.empty()) {
            if (result.back() == '\n')
              result.pop_back();
            if (result.back() == '\r')
              result.pop_back();
            fs::path found_path(result);
            if (fs::exists(found_path)) {
              return fs::absolute(found_path).string();
            }
          }
        }
      }
    }
  }

  if (name.find("clang") != std::string::npos) {
    const char *pf = std::getenv("ProgramFiles");
    if (pf) {
      fs::path llvm_candidate = fs::path(pf) / "LLVM" / "bin" / name;
      if (name.find('.') == std::string::npos)
        llvm_candidate.replace_extension(".exe");
      if (fs::exists(llvm_candidate) && !fs::is_directory(llvm_candidate)) {
        return fs::absolute(llvm_candidate).string();
      }
    }
  }

  return "";
#endif
}

std::expected<std::unique_ptr<ICompiler>, std::string>
ToolchainFinder::discover(const std::string &user_pref) {
  if (!user_pref.empty()) {
    std::string valid_pref = findBinary(user_pref);
    if (valid_pref.empty()) {
      return std::unexpected("User-preferred compiler binary '" + user_pref +
                             "' was not found on system PATH.");
    }
    // clang
    if (user_pref.find("clang") != std::string::npos) {
      std::string c_bin = "clang";
      std::string cpp_bin = user_pref;
      // derive clang++ if clang is given and the other way around too
      if (user_pref.find("clang++") != std::string::npos) {
        std::string derived_c = user_pref;
        size_t pos = derived_c.find("clang++");
        derived_c.replace(pos, 7, "clang");
        if (!findBinary(derived_c).empty())
          c_bin = derived_c;
      } else {
        std::string derived_cpp = user_pref;
        size_t pos = derived_cpp.find("clang");
        derived_cpp.replace(pos, 5, "clang++");
        if (!findBinary(derived_cpp).empty())
          cpp_bin = derived_cpp;
      }

      std::string final_ar = findBinary("llvm-ar");
      if (final_ar.empty())
        final_ar = findBinary("ar");

      if (final_ar.empty()) {
        return std::unexpected("No archiver found (llvm-ar or ar). "
                               "Set [project] compiler_path or archiver_path.");
      }

      return std::make_unique<ClangCompiler>(
          std::move(c_bin), std::move(cpp_bin), std::move(final_ar));
    }

    if (user_pref.find("g++") != std::string::npos ||
        user_pref.find("gcc") != std::string::npos) {
      std::string c_bin = "gcc";
      std::string cpp_bin = user_pref;

      if (user_pref.find("g++") != std::string::npos) {
        std::string derived_c = user_pref;
        size_t pos = derived_c.find("g++");
        derived_c.replace(pos, 3, "gcc");
        if (!findBinary(derived_c).empty())
          c_bin = derived_c;
      } else {
        std::string derived_cpp = user_pref;
        size_t pos = derived_cpp.find("gcc");
        derived_cpp.replace(pos, 3, "g++");
        if (!findBinary(derived_cpp).empty())
          cpp_bin = derived_cpp;
      }
      std::string ar_bin = findBinary("ar");
      return std::make_unique<GccCompiler>(std::move(c_bin), std::move(cpp_bin),
                                           std::move(ar_bin));
    }

    if (user_pref == "cl") {
      std::string cl_c = findBinary("cl");
      std::string cl_cpp = user_pref;
      std::string lib_ar = findBinary("lib");

      if (cl_c.empty())
        cl_c = "cl";
      if (lib_ar.empty())
        lib_ar = "lib";

      return std::make_unique<MsvcCompiler>(std::move(cl_c), std::move(cl_cpp),
                                            std::move(lib_ar));
    }
  }
  std::string gcc_c = findBinary("gcc");
  std::string gcc_cpp = findBinary("g++");
  std::string clang_c = findBinary("clang");
  std::string clang_cpp = findBinary("clang++");
  std::string cl_bin = findBinary("cl");

  std::string ar_bin = findBinary("ar");
  std::string llvm_ar = findBinary("llvm-ar");
  std::string lib_ar = findBinary("lib");

  int available_toolchains = 0;
  if (!clang_cpp.empty())
    available_toolchains++;
  if (!gcc_cpp.empty())
    available_toolchains++;
  if (!cl_bin.empty())
    available_toolchains++;

  if (available_toolchains > 1) {
    std::string msg = "Multiple toolchains found on PATH (";
    if (!clang_cpp.empty())
      msg += "Clang, ";
    if (!gcc_cpp.empty())
      msg += "GCC, ";
    if (!cl_bin.empty())
      msg += "MSVC, ";
    if (msg.ends_with(", "))
      msg.resize(msg.size() - 2);

    // our default ranking strategy Clang > GCC > MSVC
    std::string choice = !clang_cpp.empty() ? "clang++" : "g++";
    msg += "). Defaulting to " + choice +
           ". Set [project] default_compiler to override.";
    m_logger.Warn(msg);
  }
  if (!clang_cpp.empty()) {
    std::string final_ar = llvm_ar.empty() ? ar_bin : llvm_ar;
    return std::make_unique<ClangCompiler>(
        std::move(clang_c), std::move(clang_cpp), std::move(final_ar));
  }

  if (!gcc_cpp.empty()) {
    return std::make_unique<GccCompiler>(std::move(gcc_c), std::move(gcc_cpp),
                                         std::move(ar_bin));
  }

  if (!cl_bin.empty()) {
    if (lib_ar.empty())
      lib_ar = "lib";
    return std::make_unique<MsvcCompiler>(cl_bin, cl_bin, std::move(lib_ar));
  }

  m_logger.Error("No valid C/C++ compiler discovered on system PATH.");
  throw std::runtime_error(
      "Mokai Build Core Error: Toolchain Discovery Exhausted.");
}
} // namespace mokai
