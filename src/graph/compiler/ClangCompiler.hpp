#pragma once
#include "Icompiler.hpp"
#include "cli/cli.hpp"
#include <format>
#include <string_view>

namespace mokai {
class ClangCompiler : public ICompiler {
public:
  ClangCompiler(std::string c_path, std::string cpp_path, std::string ar_path)
      : m_c_path(std::move(c_path)), m_ar_path(std::move(ar_path)),
        m_cpp_path(std::move(cpp_path)) {};

  std::string getCompilerBinary(bool is_c) const override {
    return is_c ? m_c_path : m_cpp_path;
  }

  std::string getArchiverBinary() const override { return m_ar_path; }

  std::string formatInclude(std::string_view path) const override {
    return std::format("-I{}", path);
  }

  std::string formatDefine(std::string_view def) const override {
    return std::format("-D{}", def);
  }

  std::string formatOutput(std::string_view obj_path) const override {
    return std::format("-o \"{}\"", obj_path);
  }

  // -std=c11 or -std=c++23
  std::string standardFlag(std::string_view version, bool is_c) const override {
    return std::format("-std={}{}", is_c ? "c" : "c++", version);
  }

  std::string optimizationFlag(BuildProfile build_type) const override {
    switch (build_type) {
    case BuildProfile::DEBUG:
      return "-g";
    case BuildProfile::RELEASE:
      return "-O3";
    case BuildProfile::MINSIZEREL:
      return "-Os";
    }
    return "-O2"; // fallback
  }

  std::string compileOnlyFlag() const override { return "-c"; }

  std::string positionIndependentCodeFlag() const override { return "-fPIC"; }

  std::string formatArchiveCommand(std::string_view out_lib) const override {
    return std::format("rcs \"{}\"", out_lib);
  }

  std::string verboseFlag() const override { return "-v"; }

  CompilerType getType() const override { return CompilerType::CLANG; }

private:
  std::string m_c_path;
  std::string m_cpp_path;
  std::string m_ar_path;
};
} // namespace mokai
