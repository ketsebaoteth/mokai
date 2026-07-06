#pragma once

#include "graph/compiler/icompiler.hpp"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mokai {

enum class TargetType {
  Executable,
  StaticLibrary,
  SharedLibrary,
  InterfaceLibrary,
  Unknown,
};

struct VersionFromSpec {
  std::string file;
  std::string pattern;
};

struct ConditionalSources {
  std::string condition;
  std::vector<std::string> patterns;
};

struct ConditionalFlags {
  std::string condition;
  std::vector<std::string> flags;
};

struct ConditionalProperties {
  std::string condition;
  std::vector<std::string> defines;
};

// Added to support conditional platform-specific system linking (e.g., X11 only
// on Linux)
struct ConditionalSystemLibs {
  std::string condition;
  std::vector<std::string> libs;
};

struct Target {
  std::string name;
  TargetType type = TargetType::Unknown;

  std::vector<std::string> sources; // literal paths, globs, or "@group" refs
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines;
  std::vector<std::string> properties; // literal defines, or "@group" refs
  std::vector<std::string> flags;      // raw passthrough compiler flags

  // FIXED CORE GAP: Array to hold system dependencies required during the
  // linker stage (-l flags)
  std::vector<std::string> system_libs;

  std::vector<std::string>
      depends_on; // target names, "subproject:target", or "package:target"

  std::vector<ConditionalSources> sources_if;
  std::vector<ConditionalFlags> flags_if;
  std::vector<ConditionalProperties> properties_if;
  std::vector<ConditionalSystemLibs>
      system_libs_if; // Added for conditional system libraries
  bool is_default = false;

  std::vector<std::string> getActiveSources(
      const std::function<bool(const std::string &)> &eval_fn) const;
  std::vector<std::string>
  getActiveFlags(const std::function<bool(const std::string &)> &eval_fn) const;
  std::vector<std::string> getActiveProperties(
      const std::function<bool(const std::string &)> &eval_fn) const;

  // Added to mirror active property/flag extraction logic for system library
  // link evaluation
  std::vector<std::string> getActiveSystemLibs(
      const std::function<bool(const std::string &)> &eval_fn) const;
};

struct ProjectManifest;

struct GraphEdge {
  std::string from;
  std::string to;
};

struct QualifiedTarget {
  std::string qualifiedName;
  Target target;
  std::shared_ptr<ProjectManifest> manifest;
};

struct FileGroup {
  std::string name;
  std::vector<std::string> patterns;
};

struct PropertyGroup {
  std::string name;
  std::vector<std::string> defines;
  std::optional<std::string> condition; // e.g. "os == linux && arch == x64"
};

enum class HookTrigger {
  PreBuild,
  PostBuild,
  PreTargetBuild,
  PostTargetBuild,
  FileChange,
  Unknown,
};

struct Hook {
  std::string name;
  HookTrigger trigger = HookTrigger::Unknown;
  std::string run; // path to an executable script

  // Only one of these is meaningful, depending on `trigger`:
  std::optional<std::string> target;  // for Pre/PostTargetBuild
  std::optional<std::string> pattern; // for FileChange — a glob or "@group" ref
};

struct CompilerCompatibility {
  std::vector<std::string> supported;
  std::vector<std::string> unsupported;
};

struct Compatibility {
  std::string min_cpp_version;
  std::string preferred_cpp_version;
  std::vector<std::string> unsupported_cpp_versions;
  CompilerCompatibility compilers;
};

// -----------------------------------------------------------------------
// Exports — present only if this project is consumable as a dependency
// -----------------------------------------------------------------------
struct ExportProfile {
  std::vector<std::string> targets;
};

struct Exports {
  std::vector<std::string> default_targets;
  std::unordered_map<std::string, ExportProfile> profiles;
  std::vector<std::string> include_dirs;
  std::vector<std::string> defines_required;
  std::vector<std::string> defines_optional;
};

// -----------------------------------------------------------------------
// Output — where build artifacts land
// -----------------------------------------------------------------------
struct OutputProfile {
  bool enabled = true;
  std::string subdir;
};

struct OutputConfig {
  std::string directory = "./build";
  std::unordered_map<std::string, OutputProfile> configs;
};

// -----------------------------------------------------------------------
// Project — top-level [project] table
// -----------------------------------------------------------------------
struct ProjectMetadata {
  std::string name;
  std::string version;
  std::optional<VersionFromSpec> version_from;
  std::string description;
  std::vector<std::string> authors;
  std::string license;
  std::string homepage;

  std::string cpp_version;
  std::string c_version;

  // FIXED CORE GAP: Allows a package to state which target to fall back to when
  // depended on generally
  std::string default_target;

  std::vector<std::string> include_dirs;
  std::vector<std::string> dependencies;
};

// -----------------------------------------------------------------------
// ProjectManifest — the full, parsed result of one mokai.toml
// -----------------------------------------------------------------------
struct DependencySpec {
  std::string name;
  std::string version;
};

struct ResolvedDependency {
  DependencySpec requested;
  std::shared_ptr<ProjectManifest> manifest;
};

struct ProjectManifest {
  ProjectMetadata project;
  std::string base_dir = ".";
  std::optional<Compatibility> compatibility;
  std::optional<Exports> exports;
  OutputConfig output;
  std::map<std::string, bool> options;

  std::vector<FileGroup> file_groups;
  std::vector<PropertyGroup> property_groups;
  std::vector<Target> targets;
  std::vector<Hook> hooks;
  OptimizationLevel opt_lv;
  BuildProfile build_profile;

  std::unordered_map<std::string, ResolvedDependency> resolved_dependencies;
};

} // namespace mokai
