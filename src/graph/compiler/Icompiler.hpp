#pragma once

#include "cli/cli.hpp"
#include <string>
namespace mokai {

enum class CompilerType { GCC, CLANG, MSVC };
enum class BuildType { DEBUG, RELEASE, MINSIZEREL };

class ICompiler {
public:
  virtual ~ICompiler() = default;

  // returns the known paths discovered beforehand
  virtual std::string getCompilerBinary(bool is_c) const = 0;
  virtual std::string getArchiverBinary() const = 0;

  virtual std::string formatInclude(std::string_view path) const = 0;
  virtual std::string formatDefine(std::string_view def) const = 0;
  virtual std::string formatOutput(std::string_view obj_path) const = 0;

  virtual std::string standardFlag(std::string_view version,
                                   bool is_c) const = 0;
  virtual std::string optimizationFlag(BuildProfile build_type) const = 0;

  virtual std::string compileOnlyFlag() const = 0;
  virtual std::string positionIndependentCodeFlag() const = 0;
  virtual std::string formatArchiveCommand(std::string_view out_lib) const = 0;
  virtual std::string verboseFlag() const = 0;

  virtual CompilerType getType() const = 0;
};
} // namespace mokai
