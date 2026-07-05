#pragma once

#include "icompiler.hpp"
#include <filesystem>
#include <format>
#include <utility>

namespace fs = std::filesystem;
namespace mokai {

class GccCompiler : public ICompiler {
public:
  GccCompiler(std::string c_path, std::string cpp_path, std::string ar_path)
      : m_c_path(std::move(c_path)), m_cpp_path(std::move(cpp_path)),
        m_ar_path(std::move(ar_path)) {}

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

  std::vector<std::string>
  dependencyFlags(const std::string &obj) const override {
    std::string depFile = fs::path(obj).replace_extension(".d").string();
    return {"-MMD", "-MF", depFile};
  }

  std::string formatOutput(std::string_view obj_path) const override {
    return std::format("-o \"{}\"", obj_path);
  }

  std::string standardFlag(std::string_view version, bool is_c) const override {
    return std::format("-std={}", version);
  }

  std::string getObjExtension() const override { return ".o"; }

  std::string buildFlag(BuildProfile build_type) const override {
    switch (build_type) {
    case BuildProfile::DEBUG_Lv1:
      return "-g1";
    case BuildProfile::DEBUG_Lv2:
      return "-g2";
    case BuildProfile::DEBUG_Lv3:
      return "-g3";
    default:
      return "-DNDEBUG";
    }
  }

  std::string optimizationFlag(OptimizationLevel lv) const override {
    switch (lv) {
    case OptimizationLevel::Perf_Slight:
      return "-O1";
    case OptimizationLevel::Perf_Moderate:
      return "-O3";
    case OptimizationLevel::Perf_Max:
      return "-Ofast";
    case OptimizationLevel::Space_Moderate:
      return "-Os";
    case OptimizationLevel::Space_Max:
      return "-Oz";
    default:
      return "";
    }
  }

  std::string compileOnlyFlag() const override { return "-c"; }

  std::string positionIndependentCodeFlag() const override { return "-fPIC"; }

  std::string formatArchiveCommand(std::string_view out_lib) const override {
    return std::format("rcs \"{}\"", out_lib);
  }

  std::string verboseFlag() const override { return "-v"; }

  CompilerType getType() const override { return CompilerType::GCC; }

private:
  std::string m_c_path;
  std::string m_cpp_path;
  std::string m_ar_path;
};

} // namespace mokai
