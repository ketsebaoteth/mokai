#include "config.hpp"
#include "config/toml.hpp"
#include "graph/types.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(_WIN64)
#include <sys/wait.h> // POSIX/Linux process macro status decoding
#endif

namespace fs = std::filesystem;

namespace mokai {

// ============================================================================
// Intelligent DX Engine (Levenshtein Distance for Typo Detection)
// ============================================================================
static size_t calculateDistance(std::string_view s1, std::string_view s2) {
  const size_t len1 = s1.size(), len2 = s2.size();
  std::vector<std::vector<size_t>> d(len1 + 1, std::vector<size_t>(len2 + 1));
  for (size_t i = 0; i <= len1; ++i)
    d[i][0] = i;
  for (size_t j = 0; j <= len2; ++j)
    d[0][j] = j;

  for (size_t i = 1; i <= len1; ++i) {
    for (size_t j = 1; j <= len2; ++j) {
      size_t cost =
          (std::tolower(s1[i - 1]) == std::tolower(s2[j - 1])) ? 0 : 1;
      d[i][j] =
          std::min({d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + cost});
    }
  }
  return d[len1][len2];
}

// Inline Lightweight Semantic Versioning Engine for Robust Range Resolving
struct SemVer {
  int major = 0;
  int minor = 0;
  int patch = 0;

  static SemVer parse(std::string s) {
    SemVer v;
    if (s.empty())
      return v;
    if (s[0] == 'v' || s[0] == 'V')
      s = s.substr(1);
    std::replace(s.begin(), s.end(), '.', ' ');
    std::stringstream ss(s);
    ss >> v.major >> v.minor >> v.patch;
    return v;
  }

  auto operator<=>(const SemVer &) const = default;
};

// Evaluates if a given version satisfies a rule constraint like ">=3.0.0"
static bool satisfiesConstraint(const SemVer &version,
                                const std::string &constraint) {
  std::string clean = constraint;
  clean.erase(std::remove_if(clean.begin(), clean.end(), ::isspace),
              clean.end());
  if (clean.empty())
    return true;

  size_t numIdx = clean.find_first_of("0123456789vV");
  if (numIdx == std::string::npos)
    return false;

  std::string op = clean.substr(0, numIdx);
  SemVer target = SemVer::parse(clean.substr(numIdx));

  if (op == ">=")
    return version >= target;
  if (op == "<")
    return version < target;
  if (op == "<=")
    return version <= target;
  if (op == ">")
    return version > target;
  if (op == "==" || op.empty())
    return version == target;
  return false;
}

static bool matchesRange(const std::string &versionStr,
                         const std::string &rangeStr) {
  if (rangeStr.empty() || rangeStr == "*")
    return true;
  SemVer v = SemVer::parse(versionStr);

  std::stringstream ss(rangeStr);
  std::string token;
  // Handle comma-separated range bounds safely (e.g. ">=3.0.0, <4.0.0")
  while (std::getline(ss, token, ',')) {
    if (!satisfiesConstraint(v, token))
      return false;
  }
  return true;
}

Config::Config(std::string workingDir) {
  if (!checkIsFolderAndExists(workingDir)) {
    std::string hint = "Ensure you are targeting the correct project root.";
    auto fuzzyDir = fuzzyFindCloseFolder(workingDir);
    if (fuzzyDir.found) {
      hint = "Found a similar directory '\033[1m" +
             fs::path(fuzzyDir.best_match).filename().string() +
             "\033[0m'. Did you make a typo?";
    }
    m_logger.Error("Configuration Error: The specified directory does not "
                   "exist or is inaccessible:\n"
                   "  Path: \"" +
                   workingDir +
                   "\"\n"
                   "  \033[36m💡 Hint:\033[0m " +
                   hint);
    return;
  }

  m_manifest.base_dir = workingDir;

  fs::path config_path = fs::path(workingDir) / "mokai.toml";
  std::string config_path_str = config_path.string();

  if (!checkIsFileAndExists(config_path_str)) {
    std::string hint =
        "Run '\033[1mmokai create\033[0m' to scaffold a new project workspace.";
    auto fuzzyFile = fuzzyFindCloseFile(config_path_str);
    if (fuzzyFile.found) {
      hint = "Found a close match named '\033[1m" +
             fs::path(fuzzyFile.best_match).filename().string() +
             "\033[0m'. Did you make a typo when naming the file?";
    }
    m_logger.Error("Configuration Error: Missing project manifest file.\n"
                   "  Expected Location: \"" +
                   config_path.string() +
                   "\"\n"
                   "  \033[36m💡 Hint:\033[0m " +
                   hint);
    return;
  }

  if (auto res = loadConfig(config_path_str); !res) {
    m_logger.Error(res.error());
    return;
  }

  if (auto res = parseConfig(); !res) {
    if (!res.error().empty()) {
      m_logger.Error(res.error());
    }
    return;
  }
  if (auto res = extractProjectData(); !res) {
    m_logger.Error("Manifest Validation Error: Failed to build project model "
                   "from 'mokai.toml'.");
    m_logger.Error(res.error());
    return; // Fast escape on internal tree generation errors
  }
}

std::expected<void, std::string> Config::loadConfig(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::unexpected(
        "File I/O Error: Unable to open configuration file for reading.\n"
        "  Path: \"" +
        path +
        "\"\n"
        "  Hint: Check system read permissions for this file.");
  }

  std::stringstream stream;
  stream << file.rdbuf();
  m_file_content = stream.str();

  if (m_file_content.empty()) {
    return std::unexpected("Manifest Error: The file \"" + path +
                           "\" was read successfully but contains zero bytes.");
  }
  return {};
}

std::expected<void, std::string> Config::parseConfig() {
  try {
    m_config_toml = toml::parse(m_file_content);
  } catch (const toml::parse_error &e) {
    auto source = e.source();
    size_t target_line = source.begin.line;
    size_t target_col = source.begin.column;

    std::string source_line;
    std::stringstream ss(m_file_content);
    size_t current_line = 1;
    while (std::getline(ss, source_line)) {
      if (current_line == target_line) {
        break;
      }
      current_line++;
    }

    int caret_len = 1;
    if (source.end.line == target_line && source.end.column > target_col) {
      caret_len = static_cast<int>(source.end.column - target_col);
    }

    m_logger.Error(
        "TOML Syntax Error: Failed to parse configuration manifest.");
    m_logger.ErrorInline(source_line, std::string(e.description()),
                         static_cast<int>(target_line),
                         static_cast<int>(target_col - 1), caret_len);

    return std::unexpected("");
  }
  return {};
}

std::expected<void, std::string> Config::extractProjectData() {
  auto extract_string_array = [](auto &&node_view,
                                 std::vector<std::string> &dest) {
    if (auto *arr = node_view.as_array()) {
      for (auto &&el : *arr) {
        if (auto val = el.template value<std::string>()) {
          dest.push_back(*val);
        }
      }
    }
  };

  ProjectMetadata metadata;
  toml::node_view<toml::node> projectTable = m_config_toml["project"];

  if (!projectTable.is_table()) {
    return std::unexpected(
        "Validation Error: Missing global [project] block header.\n"
        "  Hint: Every 'mokai.toml' file must begin with a defined [project] "
        "table context.");
  }

  metadata.name = projectTable["name"].value_or("");
  if (metadata.name.empty()) {
    return std::unexpected("Validation Error: Missing required key 'name' "
                           "inside the [project] block.");
  }

  metadata.version = projectTable["version"].value_or("");
  metadata.license = projectTable["license"].value_or("");
  metadata.description = projectTable["description"].value_or("");
  metadata.homepage = projectTable["homepage"].value_or("");
  metadata.default_target = projectTable["default_target"].value_or("");

  if (auto *vf_table = projectTable["version_from"].as_table()) {
    VersionFromSpec vf;
    vf.file = (*vf_table)["file"].value_or("");
    vf.pattern = (*vf_table)["pattern"].value_or("");
    metadata.version_from = std::move(vf);
  }

  auto cppversion = projectTable["cpp_version"].value<std::string>();
  static const std::vector<std::string> known_versions{
      "c99", "c++11", "c++14", "c++17", "c++20", "c++23", "c++26"};
  metadata.cpp_version = cppversion.value_or("c++23");

  if (!std::ranges::contains(known_versions, metadata.cpp_version)) {
    return std::unexpected("Validation Error: Unrecognized C++ standard: \"" +
                           metadata.cpp_version + "\"");
  }

  extract_string_array(projectTable["authors"], metadata.authors);
  extract_string_array(projectTable["include_dirs"], metadata.include_dirs);
  auto depTable = projectTable["dependencies"];

  if (auto *arr = depTable.as_array()) {
    for (auto &&el : *arr) {
      if (auto val = el.value<std::string>()) {
        std::string depStr = *val;
        metadata.dependencies.push_back(depStr);

        if (isLocDep(depStr)) {
          fs::path dep_path = fs::path(m_manifest.base_dir) / depStr;
          m_logger.Info("Package Router: Resolving local link mapping -> " +
                        depStr);

          if (!fs::exists(dep_path)) {
            return std::unexpected("Package Router Error: Local dependency "
                                   "directory does not exist: " +
                                   dep_path.string());
          }

          fs::path canonical_path = fs::canonical(dep_path);
          Config depconfig(canonical_path.string());
          DependencySpec spec{depStr, ""};
          m_manifest.resolved_dependencies[depStr] =
              ResolvedDependency{spec, depconfig.getManifest()};

        } else if (isGitDep(depStr)) {
          std::string gitUrl = depStr.substr(4);
          std::string repoName = gitUrl;
          size_t lastSlash = repoName.find_last_of('/');
          if (lastSlash != std::string::npos)
            repoName = repoName.substr(lastSlash + 1);
          if (repoName.ends_with(".git"))
            repoName = repoName.substr(0, repoName.length() - 4);

          std::string targetExtDir = "./build/external/" + repoName;
          fs::create_directories("./build/external");

          if (!fs::exists(targetExtDir)) {
            m_logger.Info("Package Router: Syncing remote Git link -> " +
                          gitUrl);
            std::string cloneCmd =
                "git clone --progress " + gitUrl + " " + targetExtDir;
            if (std::system(cloneCmd.c_str()) != 0) {
              return std::unexpected("Package Router Error: Link cloning "
                                     "transaction interrupted for asset -> " +
                                     gitUrl);
            }
          } else {
            m_logger.Info(
                "Package Router: Fetching standard tree updates for -> " +
                repoName);
            std::string pullCmd = "git -C " + targetExtDir + " pull --progress";
            if (std::system(pullCmd.c_str()) != 0) {
              return std::unexpected("Package Router Error: Local pipeline "
                                     "pull transaction dropped out for -> " +
                                     repoName);
            }
          }

          std::string depTomlPath = targetExtDir + "/mokai.toml";
          if (fs::exists(depTomlPath)) {
            Config depconfig(targetExtDir);
            DependencySpec spec{depStr, ""};
            m_manifest.resolved_dependencies[depStr] =
                ResolvedDependency{spec, depconfig.getManifest()};
          } else {
            return std::unexpected(
                "Package Router Error: Cloned asset is missing a 'mokai.toml' "
                "target configuration: " +
                targetExtDir);
          }

        } else {
          std::string pkgName = depStr;
          std::string pkgVersionSpec = "";
          size_t versionDelim = depStr.find('@');
          if (versionDelim != std::string::npos) {
            pkgName = depStr.substr(0, versionDelim);
            pkgVersionSpec = depStr.substr(versionDelim + 1);
          }

          const char *homeEnv = std::getenv("HOME");
          fs::path homePath = homeEnv ? fs::path(homeEnv) : fs::current_path();
          fs::path registryDir = homePath / ".mokai" / "registry";

          if (!fs::exists(registryDir)) {
            m_logger.Info(
                "Package Router: Syncing central registry context mappings...");
            fs::create_directories(registryDir.parent_path());
            std::string regCloneCmd =
                "git clone --progress "
                "https://github.com/L1TerminalFault/mokai_confs " +
                registryDir.string();
            if (std::system(regCloneCmd.c_str()) != 0) {
              return std::unexpected(
                  "Package Router Error: Critical initialization failure "
                  "syncing tracking maps.");
            }
          } else {
            m_logger.Info(
                "Package Router: Syncing configuration tracking indexes...");
            std::string regPullCmd =
                "git -C " + registryDir.string() + " pull --progress";
            if (std::system(regPullCmd.c_str()) != 0) {
              m_logger.Error("Package Router Error: Local tracker framework "
                             "could not index downstream lines.");
            }
          }

          fs::path manifestRecipeFile = registryDir / (pkgName + ".toml");
          if (fs::exists(manifestRecipeFile)) {
            std::ifstream rFile(manifestRecipeFile.string());
            std::stringstream rStream;
            rStream << rFile.rdbuf();

            try {
              auto rootRegistryNode = toml::parse(rStream.str());
              std::string gitHubHandle =
                  rootRegistryNode["package"]["github"].value_or("");
              std::string matchedRecipePath = "";
              std::string targetGitRef = "";

              if (auto *recipeArray =
                      rootRegistryNode["package"]["recipe"].as_array()) {
                for (auto &&recipeNode : *recipeArray) {
                  if (auto *tbl = recipeNode.as_table()) {
                    std::string vRange = (*tbl)["version_range"].value_or("");
                    std::string cPath = (*tbl)["mokaiconf_path"].value_or("");
                    std::string gitRef =
                        (*tbl)["tag"].value_or((*tbl)["branch"].value_or(
                            (*tbl)["commit"].value_or("")));

                    if (matchedRecipePath.empty()) {
                      matchedRecipePath = cPath;
                      targetGitRef = gitRef;
                    }
                    if (!pkgVersionSpec.empty() &&
                        matchesRange(pkgVersionSpec, vRange)) {
                      matchedRecipePath = cPath;
                      targetGitRef = gitRef;
                    }
                  }
                }
              }

              if (!gitHubHandle.empty() && !matchedRecipePath.empty()) {
                std::string packageGitUrl =
                    "https://github.com/" + gitHubHandle;
                std::string targetPkgBuildDir = "./build/external/" + pkgName;
                fs::create_directories("./build/external");

                if (!fs::exists(targetPkgBuildDir)) {
                  m_logger.Info(
                      "Package Router: Downloading source files for [" +
                      pkgName + "] -> " + packageGitUrl);
                  std::string pkgCloneCmd = "git clone --progress " +
                                            packageGitUrl + " " +
                                            targetPkgBuildDir;
                  if (std::system(pkgCloneCmd.c_str()) != 0) {
                    return std::unexpected(
                        "Package Router Error: Failed to completely clone "
                        "remote package source layout: " +
                        pkgName);
                  }
                } else {
                  m_logger.Info(
                      "Package Router: Syncing remote tracking trees for [" +
                      pkgName + "]");
                  std::string pkgFetchCmd = "git -C " + targetPkgBuildDir +
                                            " fetch --all --tags --progress";
                  if (std::system(pkgFetchCmd.c_str()) != 0) {
                    return std::unexpected(
                        "Package Router Error: Tracker failed tracking live "
                        "heads for package: " +
                        pkgName);
                  }
                }

                if (!targetGitRef.empty()) {
                  m_logger.Info("Package Router: Pointing target branch layout "
                                "tracking ref to -> " +
                                targetGitRef);
                  std::string checkoutCmd = "git -C " + targetPkgBuildDir +
                                            " checkout " + targetGitRef;
                  if (std::system(checkoutCmd.c_str()) != 0) {
                    return std::unexpected(
                        "Package Router Error: Reference context tracking map "
                        "rejected reference payload alignment: " +
                        targetGitRef);
                  }
                }

                fs::path blueprintConfSource = registryDir / matchedRecipePath;
                fs::path destinationConfTarget =
                    fs::path(targetPkgBuildDir) / "mokai.toml";

                if (fs::exists(blueprintConfSource)) {
                  fs::copy_file(blueprintConfSource, destinationConfTarget,
                                fs::copy_options::overwrite_existing);

                  Config depconfig(targetPkgBuildDir);
                  DependencySpec spec{depStr, pkgVersionSpec};
                  m_manifest.resolved_dependencies[pkgName] =
                      ResolvedDependency{spec, depconfig.getManifest()};
                } else {
                  return std::unexpected(
                      "Registry Routing Error: Structural config blueprints "
                      "could not be mapped: " +
                      blueprintConfSource.string());
                }
              }
            } catch (...) {
              return std::unexpected(
                  "Registry Parsing Failure: Content formatting tracking logic "
                  "corrupted for: " +
                  pkgName);
            }
          } else {
            return std::unexpected(
                "Registry Resolution Failure: The library package tracking map "
                "'" +
                pkgName + "' does not exist in central configurations.");
          }
        }
      }
    }
  }

  m_manifest.project = std::move(metadata);

  if (auto *options_table = m_config_toml["options"].as_table()) {
    for (auto &&[key, value] : *options_table) {
      if (auto val = value.value<bool>()) {
        m_manifest.options[std::string(key.str())] = *val;
      }
    }
  }

  if (auto *comp_table = m_config_toml["compatibility"].as_table()) {
    Compatibility comp;
    comp.min_cpp_version = (*comp_table)["min_cpp_version"].value_or("");
    comp.preferred_cpp_version =
        (*comp_table)["preferred_cpp_version"].value_or("");
    extract_string_array((*comp_table)["unsupported_cpp_versions"],
                         comp.unsupported_cpp_versions);
    extract_string_array((*comp_table)["compilers"]["supported"],
                         comp.compilers.supported);
    extract_string_array((*comp_table)["compilers"]["unsupported"],
                         comp.compilers.unsupported);
    m_manifest.compatibility = std::move(comp);
  }

  if (auto *fg_table = m_config_toml["file_group"].as_table()) {
    for (auto &&[key, value] : *fg_table) {
      if (auto *inner_table = value.as_table()) {
        FileGroup group;
        group.name = std::string(key.str());
        extract_string_array((*inner_table)["patterns"], group.patterns);
        m_manifest.file_groups.push_back(std::move(group));
      }
    }
  }

  if (auto *pg_table = m_config_toml["property_group"].as_table()) {
    for (auto &&[key, value] : *pg_table) {
      if (auto *inner_table = value.as_table()) {
        PropertyGroup group;
        group.name = std::string(key.str());
        extract_string_array((*inner_table)["defines"], group.defines);
        if (auto cond = (*inner_table)["condition"].value<std::string>())
          group.condition = std::move(*cond);
        m_manifest.property_groups.push_back(std::move(group));
      }
    }
  }

  if (auto *target_table = m_config_toml["target"].as_table()) {
    for (auto &&[key, value] : *target_table) {
      if (auto *inner_table = value.as_table()) {
        Target target;
        target.name = std::string(key.str());

        std::string type_str = (*inner_table)["type"].value_or("");
        if (type_str == "executable")
          target.type = TargetType::Executable;
        else if (type_str == "static_library")
          target.type = TargetType::StaticLibrary;
        else if (type_str == "shared_library")
          target.type = TargetType::SharedLibrary;
        else {
          return std::unexpected("Target Validation Error: Invalid type "
                                 "context flag inside build targets: " +
                                 target.name);
        }

        extract_string_array((*inner_table)["sources"], target.sources);
        extract_string_array((*inner_table)["include_dirs"],
                             target.include_dirs);
        extract_string_array((*inner_table)["properties"], target.properties);
        extract_string_array((*inner_table)["flags"], target.flags);
        extract_string_array((*inner_table)["system_libs"], target.system_libs);
        extract_string_array((*inner_table)["depends_on"], target.depends_on);

        if (auto *s_if_arr = (*inner_table)["sources_if"].as_array()) {
          for (auto &&el : *s_if_arr) {
            if (auto *tbl = el.as_table()) {
              ConditionalSources cs;
              cs.condition = (*tbl)["condition"].value_or("");
              extract_string_array((*tbl)["patterns"], cs.patterns);
              target.sources_if.push_back(std::move(cs));
            }
          }
        }

        if (auto *f_if_arr = (*inner_table)["flags_if"].as_array()) {
          for (auto &&el : *f_if_arr) {
            if (auto *tbl = el.as_table()) {
              ConditionalFlags cf;
              cf.condition = (*tbl)["condition"].value_or("");
              extract_string_array((*tbl)["flags"], cf.flags);
              target.flags_if.push_back(std::move(cf));
            }
          }
        }

        if (auto *p_if_arr = (*inner_table)["properties_if"].as_array()) {
          for (auto &&el : *p_if_arr) {
            if (auto *tbl = el.as_table()) {
              ConditionalProperties cp;
              cp.condition = (*tbl)["condition"].value_or("");
              extract_string_array((*tbl)["defines"], cp.defines);
              target.properties_if.push_back(std::move(cp));
            }
          }
        }

        if (auto *sys_if_arr = (*inner_table)["system_libs_if"].as_array()) {
          for (auto &&el : *sys_if_arr) {
            if (auto *tbl = el.as_table()) {
              ConditionalSystemLibs csl;
              csl.condition = (*tbl)["condition"].value_or("");
              extract_string_array((*tbl)["libs"], csl.libs);
              target.system_libs_if.push_back(std::move(csl));
            }
          }
        }

        m_manifest.targets.push_back(std::move(target));
      }
    }
  }

  if (auto *hook_table = m_config_toml["hook"].as_table()) {
    for (auto &&[key, value] : *hook_table) {
      if (auto *inner_table = value.as_table()) {
        Hook hook;
        hook.name = std::string(key.str());

        std::string trigger_str = (*inner_table)["on"].value_or("");
        if (trigger_str == "pre_build")
          hook.trigger = HookTrigger::PreBuild;
        else if (trigger_str == "post_build")
          hook.trigger = HookTrigger::PostBuild;
        else if (trigger_str == "pre_target_build")
          hook.trigger = HookTrigger::PreTargetBuild;
        else if (trigger_str == "post_target_build")
          hook.trigger = HookTrigger::PostTargetBuild;
        else if (trigger_str == "file_change")
          hook.trigger = HookTrigger::FileChange;
        else
          return std::unexpected("Hook Validation Error: Step trigger "
                                 "unrecognized parsing workspace hooks.");

        hook.run = (*inner_table)["run"].value_or("");
        if (hook.run.empty())
          return std::unexpected("Hook Validation Error: Interceptor "
                                 "configuration script parameter missing.");

        if (auto tgt = (*inner_table)["target"].value<std::string>())
          hook.target = std::move(*tgt);
        if (auto pat = (*inner_table)["pattern"].value<std::string>())
          hook.pattern = std::move(*pat);

        m_manifest.hooks.push_back(std::move(hook));
      }
    }
  }

  if (auto *exports_table = m_config_toml["exports"].as_table()) {
    Exports exp;
    extract_string_array((*exports_table)["default_targets"],
                         exp.default_targets);
    if (exp.default_targets.empty())
      return std::unexpected("Validation Error: Declared [exports] block "
                             "structural layout requires missing keys.");

    extract_string_array((*exports_table)["include_dirs"], exp.include_dirs);
    extract_string_array((*exports_table)["defines_required"],
                         exp.defines_required);
    extract_string_array((*exports_table)["defines_optional"],
                         exp.defines_optional);

    if (auto *profile_table = (*exports_table)["profile"].as_table()) {
      for (auto &&[key, value] : *profile_table) {
        if (auto *inner_table = value.as_table()) {
          ExportProfile profile;
          extract_string_array((*inner_table)["targets"], profile.targets);
          exp.profiles[std::string(key.str())] = std::move(profile);
        }
      }
    }
    m_manifest.exports = std::move(exp);
  }

  toml::node_view output_view = m_config_toml["output"];
  m_manifest.output.directory = output_view["directory"].value_or("./build");

  if (auto *configs_table = m_config_toml["output"]["configs"].as_table()) {
    for (auto &&[key, value] : *configs_table) {
      if (auto *cfg_table = value.as_table()) {
        OutputProfile profile;
        profile.enabled = (*cfg_table)["enabled"].value_or(true);
        profile.subdir = (*cfg_table)["subdir"].value_or("");
        m_manifest.output.configs[std::string(key.str())] = std::move(profile);
      }
    }
  }

  m_logger.Success("Extracted Config For: " + m_manifest.project.name);
  return {};
}

bool Config::createProject(const std::string &projectName) {
  std::string chosenName = projectName;
  if (chosenName.empty()) {
    std::cout << "\033[1;34m⚙\033[0m Enter project name signature "
                 "[untitled_project]: ";
    std::getline(std::cin, chosenName);
    if (chosenName.empty())
      chosenName = "untitled_project";
  }

  fs::path projectRoot = fs::current_path() / chosenName;
  if (fs::exists(projectRoot)) {
    std::cout
        << "\033[1;31m✘ Configuration Error:\033[0m Workspace target path \""
        << chosenName << "\" already explicitly exists.\n";
    return false;
  }

  try {
    fs::create_directories(projectRoot / "src");

    std::ofstream tomlFile(projectRoot / "mokai.toml");
    tomlFile << "[project]\n"
             << "name = \"" << chosenName << "\"\n"
             << "version = \"0.1.0\"\n"
             << "cpp_version = \"c++23\"\n\n"
             << "[target." << chosenName << "]\n"
             << "type = \"executable\"\n"
             << "sources = [\"src/main.cpp\"]\n";

    std::ofstream mainFile(projectRoot / "src" / "main.cpp");
    mainFile << "#include <iostream>\n\n"
             << "int main() {\n"
             << "    std::cout << \"Hello from " << chosenName << "!\\n\";\n"
             << "    return 0;\n"
             << "}\n";

    std::cout << "\033[1;32m✓ Pipeline Initialization Success:\033[0m "
                 "Workspace generated cleanly inside: "
              << projectRoot.string() << "\n";
    return true;
  } catch (const std::exception &e) {
    std::cout << "\033[1;31m✘ System I/O Error:\033[0m Failed to spawn "
                 "workspace files: "
              << e.what() << "\n";
    return false;
  }
}

bool Config::runTarget(const std::string &targetName) {
  const Target *matchedTarget = nullptr;
  for (const auto &t : m_manifest.targets) {
    if (t.name == targetName) {
      matchedTarget = &t;
      break;
    }
  }

  if (!matchedTarget) {
    std::string hint =
        "Check your 'mokai.toml' file for the exact target name.";
    std::string best_match;
    size_t min_dist = 4;
    for (const auto &t : m_manifest.targets) {
      size_t dist = calculateDistance(t.name, targetName);
      if (dist < min_dist) {
        min_dist = dist;
        best_match = t.name;
      }
    }
    if (!best_match.empty()) {
      hint = "Did you mean to run '\033[1m" + best_match + "\033[0m'?";
    }

    m_logger.Error("Runtime Execution Error: Target build execution node '" +
                   targetName +
                   "' was not found in manifest mappings.\n"
                   "  \033[36m💡 Hint:\033[0m " +
                   hint);
    return false;
  }

  if (matchedTarget->type != TargetType::Executable) {
    m_logger.Error(
        "Runtime Execution Error: Target entity '" + targetName +
        "' matches a library compilation pattern instead of an executable.");
    return false;
  }

  fs::path absoluteOutputBinPath =
      fs::path(m_manifest.base_dir) / m_manifest.output.directory;

  std::string profileSubdir = "";
  for (const auto &[key, profile] : m_manifest.output.configs) {
    if (profile.enabled) {
      profileSubdir = profile.subdir;
      break;
    }
  }
  if (!profileSubdir.empty())
    absoluteOutputBinPath /= profileSubdir;
  absoluteOutputBinPath /= targetName;

  if (!fs::exists(absoluteOutputBinPath)) {
    m_logger.Error("Compilation Mismatch: Executable target binary not found "
                   "at pathway location:\n"
                   "  Path: \"" +
                   absoluteOutputBinPath.string() +
                   "\"\n"
                   "  Hint: Build the pipeline layout target tracking "
                   "dependencies before launching execution units.");
    return false;
  }

  m_logger.Info("Launching process pipeline unit: " + targetName + " -> " +
                absoluteOutputBinPath.string());

  int exitStatus = std::system(absoluteOutputBinPath.string().c_str());

  if (exitStatus == -1) {
    m_logger.Error("Runtime Execution Error: Failed to spawn child process "
                   "sub-shell context.");
    return false;
  }

#if defined(_WIN32) || defined(_WIN64)
  // Windows returns raw exit codes directly from std::system
  return exitStatus == 0;
#else
  // POSIX macro decoding for accurate target exit states on Linux/macOS
  return WIFEXITED(exitStatus) && WEXITSTATUS(exitStatus) == 0;
#endif
}

bool Config::isGitDep(std::string &str) { return str.starts_with("git:"); }
bool Config::isLocDep(std::string &str) {
  return str.starts_with("./") || str.starts_with("../");
}

bool Config::checkIsFileAndExists(const std::string &path) {
  return fs::exists(path) && fs::is_regular_file(path);
}

bool Config::checkIsFolderAndExists(const std::string &path) {
  return fs::exists(path) && fs::is_directory(path);
}

FuzzyFindResult Config::fuzzyFindCloseFile(std::string &path) {
  fs::path p(path);
  fs::path parent = p.has_parent_path() ? p.parent_path() : fs::current_path();
  std::string filename = p.filename().string();

  if (!fs::exists(parent))
    return {false, ""};

  std::string best_match;
  size_t min_dist = 4;

  try {
    for (const auto &entry : fs::directory_iterator(parent)) {
      if (entry.is_regular_file()) {
        std::string entryName = entry.path().filename().string();
        size_t dist = calculateDistance(entryName, filename);
        if (dist < min_dist && dist > 0) { // dist > 0 prevents matching exact
                                           // file if somehow requested
          min_dist = dist;
          best_match = entry.path().string();
        }
      }
    }
  } catch (...) {
  }

  if (!best_match.empty())
    return {true, best_match};
  return {false, ""};
}

FuzzyFindResult Config::fuzzyFindCloseFolder(std::string &path) {
  fs::path p(path);
  fs::path parent = p.has_parent_path() ? p.parent_path() : fs::current_path();
  std::string folderName = p.filename().string();

  if (!fs::exists(parent))
    return {false, ""};

  std::string best_match;
  size_t min_dist = 4;

  try {
    for (const auto &entry : fs::directory_iterator(parent)) {
      if (entry.is_directory()) {
        std::string entryName = entry.path().filename().string();
        size_t dist = calculateDistance(entryName, folderName);
        if (dist < min_dist && dist > 0) {
          min_dist = dist;
          best_match = entry.path().string();
        }
      }
    }
  } catch (...) {
  }

  if (!best_match.empty())
    return {true, best_match};
  return {false, ""};
}

} // namespace mokai
