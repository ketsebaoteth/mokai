#pragma once

#include "icompiler.hpp"
#include <format>
#include <utility>

namespace mokai {

class MsvcCompiler : public ICompiler {
public:
  MsvcCompiler(std::string c_path, std::string cpp_path, std::string ar_path)
      : m_c_path(std::move(c_path)), m_cpp_path(std::move(cpp_path)),
        m_ar_path(std::move(ar_path)) {}

  std::string getCompilerBinary(bool is_c) const override {
    return is_c ? m_c_path : m_cpp_path;
  }

  std::string getArchiverBinary() const override { return m_ar_path; }

  std::string formatInclude(std::string_view path) const override {
    return std::format("/I\"{}\"", path);
  }

  std::string formatDefine(std::string_view def) const override {
    return std::format("/D{}", def);
  }

  std::string formatOutput(std::string_view obj_path) const override {
    return std::format("/Fo\"{}\"", obj_path);
  }

  std::vector<std::string>
  dependencyFlags(const std::string &obj) const override {
    return {"/showIncludes"};
  }

  std::string standardFlag(std::string_view version, bool is_c) const override {
    return std::format("/std:{}", version);
  }

  std::string getObjExtension() const override { return ".obj"; }

  std::string buildFlag(BuildProfile build_type) const override {
    switch (build_type) {
    case BuildProfile::DEBUG_Lv1:
      return "/Zd";
    case BuildProfile::DEBUG_Lv2:
    case BuildProfile::DEBUG_Lv3:
      return "/Z7";
    default:
      return "/DNDEBUG";
    }
  }
  std::string optimizationFlag(OptimizationLevel lv) const override {
    switch (lv) {
    case OptimizationLevel::Perf_Moderate:
      return "/O2";
    case OptimizationLevel::Perf_Max:
      return "/fp:fast";
    case OptimizationLevel::Space_Moderate:
      return "/Os";
    case OptimizationLevel::Space_Max:
      return "/O1";
    default:
      return "";
    }
  }

  std::string compileOnlyFlag() const override { return "/c"; }

  std::string positionIndependentCodeFlag() const override {
    return ""; // Enforced natively inside MSVC's runtime pipeline architecture
  }

  std::string formatArchiveCommand(std::string_view out_lib) const override {
    return std::format("/OUT:\"{}\"", out_lib);
  }

  std::string verboseFlag() const override {
    return "/Bt"; // Diagnostic timings for compiler translation passes
  }

  CompilerType getType() const override { return CompilerType::MSVC; }

private:
  std::string m_c_path;
  std::string m_cpp_path;
  std::string m_ar_path;
};

} // namespace mokai
