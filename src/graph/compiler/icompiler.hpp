#pragma once

#include <memory>
#include <string>
#include <vector>

namespace mokai {

enum class CompilerType { GCC, CLANG, MSVC };

struct Task {
  std::string source;
  std::string output;
  bool is_c;
  std::string std_flag;
  std::string working_dir;
  std::shared_ptr<const std::vector<std::string>> build_args;
  std::vector<std::string> defines;
};

enum class BuildProfile { DEBUG_Lv1, DEBUG_Lv2, DEBUG_Lv3, RELEASE };

enum class OptimizationLevel {
  None,
  Perf_Slight,    // -O1
  Perf_Moderate,  // -O3    or /O2
  Perf_Max,       // -Ofast or /fp:fast
  Space_Moderate, // -Os    or /Os
  Space_Max,      // -Oz    or /O1
};

class ICompiler {
public:
  virtual ~ICompiler() = default;

  virtual std::string getCompilerBinary(bool is_c) const = 0;
  virtual std::string getArchiverBinary() const = 0;

  virtual std::string formatInclude(std::string_view path) const = 0;
  virtual std::string formatDefine(std::string_view def) const = 0;
  virtual std::string formatOutput(std::string_view obj_path) const = 0;

  virtual std::string standardFlag(std::string_view version,
                                   bool is_c) const = 0;
  virtual std::string buildFlag(BuildProfile build_type) const = 0;
  virtual std::string optimizationFlag(OptimizationLevel lv) const = 0;

  virtual std::string compileOnlyFlag() const = 0;
  virtual std::string positionIndependentCodeFlag() const = 0;
  virtual std::string getObjExtension() const = 0;
  virtual std::vector<std::string>
  dependencyFlags(const std::string &obj) const = 0;
  virtual std::string formatArchiveCommand(std::string_view out_lib) const = 0;
  virtual std::string verboseFlag() const = 0;

  virtual CompilerType getType() const = 0;

  std::vector<std::string> OrchestrateCompileArgs(Task &task) {
    std::vector<std::string> args = {getCompilerBinary(task.is_c)};
    args.push_back(compileOnlyFlag());
    args.push_back(task.source);

    // TODO: Uncomment this line after implementing extraction of field
    // 'optimize' in the toml
    //
    // args.push_back(optimizationFlag());

    std::string formatted = formatOutput(task.output);
    if (formatted.starts_with("-o \"") && formatted.ends_with("\"")) {
      args.push_back("-o");
      args.push_back(task.output);
    } else {
      args.push_back(formatted);
    }
    for (const auto &f : dependencyFlags(task.output)) {
      args.push_back(f);
    }

    args.push_back(task.std_flag);
    for (const auto &arg : *task.build_args) {
      args.push_back(arg);
    }

    return args;
  };
};

} // namespace mokai
