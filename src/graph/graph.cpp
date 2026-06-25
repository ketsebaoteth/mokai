#include "graph.hpp"
#include "cli/cli.hpp"
#include "graph/types.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

namespace mokai {

static int executeCommandFast(const std::vector<std::string> &args) {
  if (args.empty())
    return -1;

#if defined(_WIN32) || defined(_WIN64)
  size_t total_len = 0;
  for (const auto &a : args)
    total_len += a.size() + 1;

  std::string command;
  command.reserve(total_len);
  for (size_t i = 0; i < args.size(); ++i) {
    command += args[i];
    if (i + 1 < args.size())
      command += " ";
  }

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));

  std::vector<char> cmd_buffer(command.begin(), command.end());
  cmd_buffer.push_back('\0');

  if (CreateProcessA(NULL, cmd_buffer.data(), NULL, NULL, FALSE, 0, NULL, NULL,
                     &si, &pi)) {
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
  }
  return -1;
#else
  std::vector<char *> c_args;
  c_args.reserve(args.size() + 1);
  for (const auto &arg : args) {
    c_args.push_back(const_cast<char *>(arg.c_str()));
  }
  c_args.push_back(nullptr);

  pid_t pid;
  int status;
  if (posix_spawnp(&pid, c_args[0], NULL, NULL, c_args.data(), environ) == 0) {
    if (waitpid(pid, &status, 0) != -1) {
      if (WIFEXITED(status))
        return WEXITSTATUS(status);
    }
  }
  return -1;
#endif
}

struct Toolchain {
  std::string cpp_compiler;
  std::string c_compiler;
  std::string archiver;
  bool is_msvc = false;
};

static Toolchain discoverToolchain() {
  static Toolchain cached_toolchain;
  static bool discovered = false;

  if (!discovered) {
#if defined(_WIN32) || defined(_WIN64)
    if (std::system("where cl > NUL 2>&1") == 0) {
      cached_toolchain = {"cl", "cl", "lib", true};
    } else if (std::system("where clang++ > NUL 2>&1") == 0) {
      cached_toolchain = {"clang++", "clang", "llvm-ar", false};
    } else {
      cached_toolchain = {"g++", "gcc", "ar", false};
    }
#else
    if (std::system("which clang++ > /dev/null 2>&1") == 0) {
      cached_toolchain = {"clang++", "clang", "llvm-ar", false};
    } else {
      cached_toolchain = {"g++", "gcc", "ar", false};
    }
#endif
    discovered = true;
  }
  return cached_toolchain;
}

static std::string escapeJsonString(const std::string &input) {
  std::string output;
  for (char c : input) {
    if (c == '\\' || c == '"') {
      output += '\\';
    }
    output += c;
  }
  return output;
}

static std::string triggerToString(HookTrigger trigger) {
  switch (trigger) {
  case HookTrigger::PreBuild:
    return "pre_build";
  case HookTrigger::PostBuild:
    return "post_build";
  case HookTrigger::PreTargetBuild:
    return "pre_target_build";
  case HookTrigger::PostTargetBuild:
    return "post_target_build";
  case HookTrigger::FileChange:
    return "file_change";
  default:
    return "unknown";
  }
}

static std::string targetTypeToString(TargetType type) {
  switch (type) {
  case TargetType::Executable:
    return "executable";
  case TargetType::StaticLibrary:
    return "static_library";
  case TargetType::SharedLibrary:
    return "shared_library";
  default:
    return "unknown";
  }
}

const QualifiedTarget *
Graph::FindByQualifiedName(const std::string &qualified_name) const {
  for (const auto &qt : m_allTargets) {
    if (qt.qualifiedName == qualified_name) {
      return &qt;
    }
  }
  return nullptr;
}

void Graph::collectTransitive(const std::string &node,
                              std::unordered_set<std::string> &visited,
                              std::vector<std::string> &out_libs) {
  const QualifiedTarget *qt = FindByQualifiedName(node);
  if (!qt)
    return;

  for (const auto &raw_dep : qt->target.depends_on) {
    auto resolved = resolveDependsOnEntry(raw_dep, *qt);
    for (const auto &dep_name : resolved) {
      if (visited.find(dep_name) == visited.end()) {
        visited.insert(dep_name);
        collectTransitive(dep_name, visited, out_libs);
        out_libs.push_back(dep_name);
      }
    }
  }
}

std::vector<std::string>
Graph::getTransitiveDependencies(const std::string &qualified_name) {
  std::unordered_set<std::string> visited;
  std::vector<std::string> out_libs;
  collectTransitive(qualified_name, visited, out_libs);
  return out_libs;
}

std::vector<QualifiedTarget>
Graph::flattenManifestTree(std::shared_ptr<mokai::ProjectManifest> manifest,
                           const std::string &path_prefix) {
  std::vector<QualifiedTarget> result;

  for (auto &target : manifest->targets) {
    result.push_back({path_prefix + "::" + target.name, target, manifest});
  }

  for (auto &[dep_key, resolved] : manifest->resolved_dependencies) {
    if (resolved.manifest) {
      std::string child_prefix =
          path_prefix + "." + resolved.manifest->project.name;
      auto child_result = flattenManifestTree(resolved.manifest, child_prefix);
      result.insert(result.end(), child_result.begin(), child_result.end());
    }
  }
  return result;
}

static std::shared_ptr<ProjectManifest> FindResolvedDependency(
    const std::unordered_map<std::string, ResolvedDependency> &deps,
    const std::string &name_or_key) {

  auto to_lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  };

  std::string target_name = to_lower(name_or_key);

  for (const auto &[key, resolved] : deps) {
    size_t at_pos = key.find('@');
    std::string base_key =
        to_lower((at_pos == std::string::npos) ? key : key.substr(0, at_pos));

    bool key_match = (base_key == target_name);
    bool name_match = false;

    if (resolved.manifest) {
      name_match = (to_lower(resolved.manifest->project.name) == target_name);
    }

    if (key_match || name_match) {
      return resolved.manifest;
    }
  }
  return nullptr;
}

static const QualifiedTarget *
FindSiblingByName(const std::vector<QualifiedTarget> &all,
                  const QualifiedTarget &from, const std::string &name) {
  for (auto &qt : all) {
    if (qt.manifest == from.manifest && qt.target.name == name) {
      return &qt;
    }
  }
  return nullptr;
}

static bool SplitExplicitTarget(const std::string &raw, std::string &pkg,
                                std::string &target) {
  auto colon = raw.find(':');
  if (colon == std::string::npos)
    return false;
  pkg = raw.substr(0, colon);
  target = raw.substr(colon + 1);
  return true;
}

static bool SplitProfile(const std::string &raw, std::string &pkg,
                         std::string &profile) {
  auto slash = raw.find('/');
  if (slash == std::string::npos)
    return false;
  pkg = raw.substr(0, slash);
  profile = raw.substr(slash + 1);
  return true;
}

std::vector<std::string>
Graph::resolveDependsOnEntry(const std::string &raw_dep,
                             const QualifiedTarget &from_target) {
  std::vector<std::string> resolved;

  std::string pkg, explicit_target;
  if (SplitExplicitTarget(raw_dep, pkg, explicit_target)) {
    auto dep_manifest = FindResolvedDependency(
        from_target.manifest->resolved_dependencies, pkg);
    if (!dep_manifest) {
      m_logger.Error("Target '" + from_target.qualifiedName +
                     "' depends on unknown package '" + pkg + "'");
      return resolved;
    }

    bool found = false;
    for (auto &qt : m_allTargets) {
      if (qt.manifest == dep_manifest && qt.target.name == explicit_target) {
        resolved.push_back(qt.qualifiedName);
        found = true;
        break;
      }
    }
    if (!found) {
      m_logger.Error("Package '" + pkg + "' has no target named '" +
                     explicit_target + "' (required by '" +
                     from_target.qualifiedName + "')");
    }
    return resolved;
  }

  std::string profilePkg, profileName;
  if (SplitProfile(raw_dep, profilePkg, profileName)) {
    auto dep_manifest = FindResolvedDependency(
        from_target.manifest->resolved_dependencies, profilePkg);
    if (!dep_manifest) {
      m_logger.Error("Target '" + from_target.qualifiedName +
                     "' depends on unknown package '" + profilePkg + "'");
      return resolved;
    }

    if (!dep_manifest->exports ||
        !dep_manifest->exports->profiles.count(profileName)) {
      m_logger.Error("Package '" + profilePkg +
                     "' has no export profile named '" + profileName + "'");
      return resolved;
    }
    for (auto &target_name :
         dep_manifest->exports->profiles.at(profileName).targets) {
      for (auto &qt : m_allTargets) {
        if (qt.manifest == dep_manifest && qt.target.name == target_name) {
          resolved.push_back(qt.qualifiedName);
        }
      }
    }
    return resolved;
  }

  if (auto *sibling = FindSiblingByName(m_allTargets, from_target, raw_dep)) {
    resolved.push_back(sibling->qualifiedName);
    return resolved;
  }

  for (const auto &[dep_key, resolved_dep] :
       from_target.manifest->resolved_dependencies) {
    if (resolved_dep.manifest) {
      for (auto &qt : m_allTargets) {
        if (qt.manifest == resolved_dep.manifest && qt.target.name == raw_dep) {
          resolved.push_back(qt.qualifiedName);
        }
      }
    }
  }
  if (!resolved.empty())
    return resolved;

  auto dep_manifest = FindResolvedDependency(
      from_target.manifest->resolved_dependencies, raw_dep);
  if (!dep_manifest) {
    m_logger.Error(
        "Target '" + from_target.qualifiedName + "' depends on '" + raw_dep +
        "', which is neither a sibling target nor a declared dependency.");
    return resolved;
  }

  if (!dep_manifest->exports) {
    m_logger.Error("Dependency '" + raw_dep +
                   "' has no [exports] section, so it cannot be depended on by "
                   "name alone.");
    return resolved;
  }

  for (auto &target_name : dep_manifest->exports->default_targets) {
    for (auto &qt : m_allTargets) {
      if (qt.manifest == dep_manifest && qt.target.name == target_name) {
        resolved.push_back(qt.qualifiedName);
      }
    }
  }

  if (resolved.empty()) {
    m_logger.Error("Dependency '" + raw_dep +
                   "' declares exports.default, but none of those target names "
                   "matched a real target.");
  }

  return resolved;
}

std::vector<GraphEdge> Graph::buildEdges() {
  std::vector<GraphEdge> edges;
  for (auto &qt : m_allTargets) {
    for (auto &raw_dep : qt.target.depends_on) {
      auto resolved_targets = resolveDependsOnEntry(raw_dep, qt);
      for (auto &to_name : resolved_targets) {
        edges.push_back({qt.qualifiedName, to_name});
      }
    }
  }
  return edges;
}

std::vector<std::string>
Graph::computeBuildOrder(const std::vector<GraphEdge> &edges) {
  std::vector<std::string> build_order;
  std::unordered_map<std::string, std::vector<std::string>> adj;

  for (const auto &edge : edges) {
    adj[edge.from].push_back(edge.to);
  }

  std::unordered_map<std::string, NodeState> states;
  for (const auto &qt : m_allTargets) {
    states[qt.qualifiedName] = NodeState::Unvisited;
  }

  std::vector<std::string> active_path;
  auto dfs = [&](auto &self, const std::string &node) -> bool {
    states[node] = NodeState::Visiting;
    active_path.push_back(node);

    if (adj.count(node)) {
      for (const auto &dependency : adj[node]) {
        if (!states.count(dependency))
          continue;

        if (states[dependency] == NodeState::Visiting) {
          std::stringstream error_trace;
          error_trace << "Dependency Cycle Detected! The execution sequence "
                         "cannot be resolved safely.\n"
                      << "    Cycle Path Trace:\n";

          bool track_active_loop = false;
          for (const auto &path_node : active_path) {
            if (path_node == dependency)
              track_active_loop = true;
            if (track_active_loop) {
              error_trace << "    " << path_node << " ->\n";
            }
          }
          error_trace << "    " << dependency << " (Loop closes here)";

          m_logger.Error(error_trace.str());
          return false;
        }

        if (states[dependency] == NodeState::Unvisited) {
          if (!self(self, dependency))
            return false;
        }
      }
    }

    states[node] = NodeState::Done;
    active_path.pop_back();
    build_order.push_back(node);
    return true;
  };

  for (const auto &qt : m_allTargets) {
    if (states[qt.qualifiedName] == NodeState::Unvisited) {
      if (!dfs(dfs, qt.qualifiedName))
        return {};
    }
  }

  return build_order;
}

std::vector<std::string>
Graph::expandBraceNotation(const std::string &pattern) {
  std::vector<std::string> results;
  size_t start = pattern.find('{');
  size_t end = pattern.find('}', start);

  if (start == std::string::npos || end == std::string::npos) {
    results.push_back(pattern);
    return results;
  }

  std::string prefix = pattern.substr(0, start);
  std::string suffix = pattern.substr(end + 1);
  std::string options_raw = pattern.substr(start + 1, end - start - 1);

  std::stringstream ss(options_raw);
  std::string token;
  while (std::getline(ss, token, ',')) {
    std::string expanded = prefix + token + suffix;
    auto sub_expanded = expandBraceNotation(expanded);
    results.insert(results.end(), sub_expanded.begin(), sub_expanded.end());
  }

  return results;
}

void Graph::matchGlobPattern(const std::string &pattern,
                             const std::string &base_dir,
                             std::vector<std::string> &matches) {
  auto expanded_patterns = expandBraceNotation(pattern);
  if (expanded_patterns.size() > 1) {
    for (const auto &p : expanded_patterns) {
      matchGlobPattern(p, base_dir, matches);
    }
    return;
  }

  std::string target_pattern = pattern;
  if (target_pattern.rfind("./", 0) == 0) {
    target_pattern = target_pattern.substr(2);
  }

  std::replace(target_pattern.begin(), target_pattern.end(), '\\', '/');

  std::string regex_str = "^";
  bool is_recursive = (target_pattern.find("**") != std::string::npos);

  for (size_t i = 0; i < target_pattern.length(); ++i) {
    char c = target_pattern[i];
    if (c == '*' && i + 1 < target_pattern.length() &&
        target_pattern[i + 1] == '*') {
      regex_str += ".*";
      i++;
      if (i + 1 < target_pattern.length() && target_pattern[i + 1] == '/')
        i++;
    } else if (c == '*') {
      regex_str += "[^/]*";
    } else if (c == '?') {
      regex_str += "[^/]";
    } else if (c == '.' || c == '+' || c == '^' || c == '$' || c == '(' ||
               c == ')' || c == '[' || c == ']' || c == '|') {
      regex_str += '\\';
      regex_str += c;
    } else {
      regex_str += c;
    }
  }
  regex_str += "$";
  std::regex filter(regex_str);

  std::string search_root = base_dir.empty() ? "." : base_dir;

  auto normalize_and_add = [&](const fs::path &path) {
    std::string rel_path = fs::relative(path, search_root).string();
    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
    if (std::regex_match(rel_path, filter)) {
      matches.push_back(fs::path(path).lexically_normal().string());
    }
  };

  if (is_recursive) {
    // OPTIMIZATION: Early filtering of non-source heavy directories
    for (auto it = fs::recursive_directory_iterator(search_root);
         it != fs::recursive_directory_iterator(); ++it) {
      std::string dir_name = it->path().filename().string();
      if (dir_name == "build" || dir_name == ".git" || dir_name == ".mokai") {
        it.disable_recursion_pending();
        continue;
      }
      if (fs::is_regular_file(*it)) {
        normalize_and_add(it->path());
      }
    }
  } else {
    for (const auto &entry : fs::directory_iterator(search_root)) {
      if (fs::is_regular_file(entry)) {
        normalize_and_add(entry.path());
      }
    }
  }
}

bool Graph::evaluateConditionExpression(
    const std::string &condition, const Target &target,
    const std::shared_ptr<ProjectManifest> &manifest) {
  if (condition.empty())
    return true;

  std::regex target_type_regex(R"(target_type\s*==\s*([A-Za-z0-9_]+))");
  std::smatch tt_match;
  if (std::regex_search(condition, tt_match, target_type_regex)) {
    std::string expected_type = tt_match[1].str();
    std::string current_type_str = targetTypeToString(target.type);
    return (current_type_str == expected_type);
  }

  std::regex compiler_regex(R"(compiler\s*==\s*([A-Za-z0-9_]+))");
  std::smatch comp_match;
  if (std::regex_search(condition, comp_match, compiler_regex)) {
    std::string expected_compiler = comp_match[1].str();
    Toolchain tc = discoverToolchain();
    std::string active_compiler = "gcc";
    if (tc.is_msvc)
      active_compiler = "msvc";
    else if (tc.cpp_compiler == "clang++")
      active_compiler = "clang";

    return (active_compiler == expected_compiler);
  }

  std::regex option_regex(R"(options\.([A-Za-z0-9_]+)\s*==\s*(true|false))");
  std::smatch match;
  if (std::regex_search(condition, match, option_regex)) {
    std::string option_key = match[1].str();
    bool target_value = (match[2].str() == "true");

    if (manifest->options.count(option_key)) {
      return manifest->options.at(option_key) == target_value;
    }
    return !target_value;
  }

  return this->evaluateCond(condition);
}

std::vector<std::string>
Graph::resolveTargetSources(const Target &target,
                            const std::shared_ptr<ProjectManifest> &manifest) {
  std::vector<std::string> resolved;
  std::string base = manifest->base_dir.empty() ? "." : manifest->base_dir;

  auto eval_cb = [this, &target, &manifest](const std::string &cond) {
    return this->evaluateConditionExpression(cond, target, manifest);
  };

  std::vector<std::string> active_raw_sources =
      target.getActiveSources(eval_cb);

  for (const auto &raw_source : active_raw_sources) {
    if (raw_source.rfind("@", 0) == 0) {
      std::string group_target = raw_source.substr(1);
      bool group_found = false;

      for (const auto &fg : manifest->file_groups) {
        if (fg.name == group_target) {
          group_found = true;
          for (const auto &pattern : fg.patterns) {
            matchGlobPattern(pattern, base, resolved);
          }
          break;
        }
      }
      if (!group_found) {
        m_logger.Warn("Configuration Warning: Source reference target group '" +
                      raw_source +
                      "' could not be mapped within current manifest context.");
      }
    } else if (raw_source.find_first_of("*?{") != std::string::npos) {
      matchGlobPattern(raw_source, base, resolved);
    } else {
      std::string clean_path = raw_source;
      if (clean_path.rfind("./", 0) == 0)
        clean_path = clean_path.substr(2);

      fs::path full_path = fs::path(base) / clean_path;

      if (fs::exists(full_path) && fs::is_regular_file(full_path)) {
        resolved.push_back(full_path.lexically_normal().string());
      } else {
        m_logger.Error(
            "File System Error: Missing explicit source asset mapping.\n"
            "  Target declaration requested literal path: \"" +
            full_path.string() +
            "\"\n"
            "  Status: Verification failed (File does not exist or access "
            "denied).");
      }
    }
  }

  std::sort(resolved.begin(), resolved.end());
  resolved.erase(std::unique(resolved.begin(), resolved.end()), resolved.end());
  return resolved;
}

void Graph::executeHooks(const std::shared_ptr<ProjectManifest> &manifest,
                         HookTrigger trigger, const std::string &target_name) {
  for (const auto &hook : manifest->hooks) {
    if (hook.trigger != trigger)
      continue;
    if (hook.target.has_value() && hook.target.value() != target_name)
      continue;

    std::string trigger_str = triggerToString(trigger);
    m_logger.Info("Executing structural Hook [" + hook.name +
                  "] via context phase '" + trigger_str + "'");

    fs::path temp_dir = fs::temp_directory_path();
    fs::path context_path =
        temp_dir / ("mokai_hook_ctx_" + hook.name + ".json");

    std::ofstream ctx_file(context_path);
    ctx_file << "{\n"
             << "  \"trigger\": \"" << escapeJsonString(trigger_str) << "\",\n"
             << "  \"project\": \"" << escapeJsonString(manifest->project.name)
             << "\",\n"
             << "  \"target\": \"" << escapeJsonString(target_name) << "\"\n"
             << "}\n";
    ctx_file.close();

    std::string command;
#if defined(_WIN32) || defined(_WIN64)
    command =
        "set MOKAI_CONTEXT_FILE=" + context_path.string() + " && " + hook.run;
#else
    command = "MOKAI_CONTEXT_FILE=" + context_path.string() + " " + hook.run;
#endif

    int hook_result = std::system(command.c_str());
    if (hook_result != 0) {
      m_logger.Warn("Hook warning: lifecycle binary execution [" + hook.name +
                    "] dropped non-zero code: " + std::to_string(hook_result));
    }
    fs::remove(context_path);
  }
}

bool Graph::BuildAllTree(const std::vector<std::string> &build_order) {
  if (build_order.empty()) {
    m_logger.Warn("Build pipeline terminated: No build sequence targets "
                  "scheduled for execution.");
    return true;
  }

  std::unordered_map<std::string, std::string> source_filename_to_target;
  // OPTIMIZATION: Cache resolved vectors to prevent repeating expensive
  // crawling operations later
  std::unordered_map<std::string, std::vector<std::string>>
      cached_resolved_sources;

  for (const auto &qualified_name : build_order) {
    const QualifiedTarget *qt = FindByQualifiedName(qualified_name);
    if (!qt)
      continue;

    if (!m_options.target_filter.empty() &&
        qt->target.name != m_options.target_filter) {
      continue;
    }

    std::vector<std::string> target_srcs =
        resolveTargetSources(qt->target, qt->manifest);
    cached_resolved_sources[qualified_name] = target_srcs;

    for (const auto &src : target_srcs) {
      std::string filename = fs::path(src).filename().string();
      if (source_filename_to_target.count(filename)) {
        m_logger.Error("Build Collision Error: Duplicate source file name '" +
                       filename + "' found in both '" +
                       source_filename_to_target[filename] + "' and '" +
                       qualified_name +
                       "'. Source names must be globally unique.");
        return false;
      }
      source_filename_to_target[filename] = qualified_name;
    }
  }

  auto build_start_time = std::chrono::high_resolution_clock::now();
  Toolchain toolchain = discoverToolchain();
  std::string output_root = "./build";

  std::string sub_profile =
      (m_options.profile == BuildProfile::Release) ? "release" : "debug";
  std::string target_build_dir = output_root + "/" + sub_profile;
  std::string object_cache_dir = target_build_dir + "/obj";

  fs::create_directories(target_build_dir);
  fs::create_directories(object_cache_dir);

  std::string cache_dir = "./.mokai";
  fs::create_directories(cache_dir);
  std::string cache_path = cache_dir + "/mokai.cache";

  size_t total_source_files = 0;
  std::atomic<size_t> total_cache_hits{0};
  std::atomic<size_t> total_cache_misses{0};

  std::unordered_map<std::string, std::pair<std::string, std::string>>
      state_cache;
  std::mutex cache_mutex;

  if (fs::exists(cache_path) && !m_options.force_rebuild) {
    std::ifstream cache_file(cache_path);
    std::string line, f_path, f_time, f_hash;
    while (std::getline(cache_file, line)) {
      std::stringstream ss(line);
      if (ss >> f_path >> f_time >> f_hash) {
        state_cache[f_path] = {f_time, f_hash};
      }
    }
  }

  std::unordered_map<std::string, std::string> built_library_map;
  std::vector<std::string> compile_commands_entries;
  std::mutex compilation_db_mutex;

  if (!m_allTargets.empty()) {
    executeHooks(m_allTargets.front().manifest, HookTrigger::PreBuild, "");
  }

  std::unordered_map<std::string, std::vector<std::string>> dependents_graph;
  std::unordered_map<std::string, int> in_degree;
  std::unordered_set<std::string> pipeline_targets(build_order.begin(),
                                                   build_order.end());

  for (const auto &qualified_name : build_order) {
    in_degree[qualified_name] = 0;
  }

  for (const auto &qualified_name : build_order) {
    const QualifiedTarget *qt = FindByQualifiedName(qualified_name);
    if (!qt)
      continue;

    for (const auto &raw_dep : qt->target.depends_on) {
      auto resolved = resolveDependsOnEntry(raw_dep, *qt);
      for (const auto &dep_name : resolved) {
        if (pipeline_targets.contains(dep_name)) {
          dependents_graph[dep_name].push_back(qualified_name);
          in_degree[qualified_name]++;
        }
      }
    }
  }

  std::queue<std::string> ready_targets;
  int completed_targets_count = 0;
  int total_pipeline_targets = 0;
  std::atomic<bool> global_build_failed{false};

  std::atomic<int> completed_file_tasks{0};
  int processed_file_tasks = 0;

  for (const auto &qualified_name : build_order) {
    const QualifiedTarget *qt = FindByQualifiedName(qualified_name);
    if (!qt)
      continue;
    if (!m_options.target_filter.empty() &&
        qt->target.name != m_options.target_filter) {
      continue;
    }
    total_pipeline_targets++;
    if (in_degree[qualified_name] == 0) {
      ready_targets.push(qualified_name);
    }
  }

  std::condition_variable scheduler_cv;
  std::mutex scheduler_mutex;

  unsigned int worker_count =
      (m_options.job_count > 0)
          ? static_cast<unsigned int>(m_options.job_count)
          : std::max(1u, std::thread::hardware_concurrency());

  struct FileTask {
    std::string src;
    std::string obj_file;
    std::string chosen_compiler;
    std::string final_std_flag;
    std::shared_ptr<const std::vector<std::string>> shared_base_args;
    std::string working_directory;
    bool is_msvc;
  };

  struct CacheRecord {
    std::string src;
    std::string write_time;
    std::string hash;
  };

  struct ActiveTargetContext {
    const QualifiedTarget *qt;
    std::vector<std::string> sources;
    std::vector<std::string> object_files;
    std::atomic<size_t> remaining_files{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> requires_linkage{false};
    int step_index;

    std::vector<CacheRecord> computed_cache_records;
    std::mutex cache_records_mutex;
  };

  std::queue<std::pair<std::shared_ptr<ActiveTargetContext>, FileTask>>
      global_task_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::atomic<bool> workers_should_terminate{false};

  std::atomic<int> current_global_step{0};

  std::vector<std::thread> workers;
  for (unsigned int i = 0; i < worker_count; ++i) {
    workers.emplace_back([&]() {
      while (true) {
        std::pair<std::shared_ptr<ActiveTargetContext>, FileTask> work_item;
        {
          std::unique_lock<std::mutex> lock(queue_mutex);
          queue_cv.wait(lock, [&]() {
            return !global_task_queue.empty() || workers_should_terminate ||
                   global_build_failed;
          });

          if (workers_should_terminate || global_build_failed)
            break;

          work_item = std::move(global_task_queue.front());
          global_task_queue.pop();
        }

        auto &ctx = work_item.first;
        const auto &task = work_item.second;

        if (global_build_failed || ctx->failed) {
          ctx->remaining_files.fetch_sub(1);
          completed_file_tasks.fetch_add(1);
          std::unique_lock<std::mutex> sched_lock(scheduler_mutex);
          scheduler_cv.notify_one();
          continue;
        }

        bool need_compile = true;
        std::string current_time;
        try {
          fs::path src_path(task.src);
          if (fs::exists(src_path)) {
            current_time = std::to_string(
                fs::last_write_time(src_path).time_since_epoch().count());

            std::lock_guard<std::mutex> lock(cache_mutex);
            if (!m_options.force_rebuild && state_cache.count(task.src) &&
                state_cache[task.src].first == current_time &&
                fs::exists(task.obj_file)) {
              need_compile = false;
              total_cache_hits++;
            }
          }
        } catch (...) {
        }

        if (need_compile) {
          total_cache_misses++;
          ctx->requires_linkage = true;

          std::vector<std::string> file_args;
          file_args.reserve(task.shared_base_args->size() + 5);
          file_args.push_back(task.chosen_compiler);
          file_args.push_back(task.final_std_flag);
          for (const auto &a : *task.shared_base_args)
            file_args.push_back(a);

          if (task.is_msvc) {
            file_args.push_back("/c");
            file_args.push_back(task.src);
            file_args.push_back("/Fo" + task.obj_file);
          } else {
            file_args.push_back("-c");
            file_args.push_back(task.src);
            file_args.push_back("-o");
            file_args.push_back(task.obj_file);
          }

          std::string full_command_str;
          for (size_t a = 0; a < file_args.size(); ++a) {
            full_command_str += file_args[a];
            if (a + 1 < file_args.size())
              full_command_str += " ";
          }

          std::stringstream json_entry;
          json_entry << "  {\n"
                     << "    \"directory\": \""
                     << escapeJsonString(task.working_directory) << "\",\n"
                     << "    \"command\": \""
                     << escapeJsonString(full_command_str) << "\",\n"
                     << "    \"file\": \"" << escapeJsonString(task.src)
                     << "\",\n"
                     << "    \"output\": \"" << escapeJsonString(task.obj_file)
                     << "\"\n"
                     << "  }";
          std::string json_str = json_entry.str();

          {
            std::lock_guard<std::mutex> db_lock(compilation_db_mutex);
            compile_commands_entries.push_back(std::move(json_str));
          }

          int res = executeCommandFast(file_args);

          if (res != 0) {
            ctx->failed = true;
            global_build_failed = true;
          } else {
            if (!current_time.empty()) {
              std::lock_guard<std::mutex> rec_lock(ctx->cache_records_mutex);
              ctx->computed_cache_records.push_back(
                  {task.src, current_time, "-"});
            }
          }
        }

        ctx->remaining_files.fetch_sub(1);
        completed_file_tasks.fetch_add(1);
        {
          std::unique_lock<std::mutex> sched_lock(scheduler_mutex);
          scheduler_cv.notify_one();
        }
      }
    });
  }

  std::vector<std::shared_ptr<ActiveTargetContext>> active_targets_running;
  std::string working_directory = fs::current_path().string();

  std::unique_lock<std::mutex> sched_lock(scheduler_mutex);
  while ((completed_targets_count < total_pipeline_targets) &&
         !global_build_failed) {

    while (!ready_targets.empty()) {
      std::string current_ready = ready_targets.front();
      ready_targets.pop();

      const QualifiedTarget *qt = FindByQualifiedName(current_ready);
      const Target &target = qt->target;

      // OPTIMIZATION: Pulled directly from cache, avoiding secondary disk crawl
      // entirely
      std::vector<std::string> sources = cached_resolved_sources[current_ready];
      if (sources.empty()) {
        if (m_options.verbosity != Verbosity::Quiet) {
          m_logger.Warn("Skipping build unit '" + current_ready +
                        "': Source list evaluated to empty.");
        }
        completed_targets_count++;
        for (const auto &dep : dependents_graph[current_ready]) {
          in_degree[dep]--;
          if (in_degree[dep] == 0 && pipeline_targets.contains(dep)) {
            ready_targets.push(dep);
          }
        }
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(cache_mutex);
        total_source_files += sources.size();
      }

      executeHooks(qt->manifest, HookTrigger::PreTargetBuild, target.name);

      int step_idx = ++current_global_step;
      if (m_options.verbosity != Verbosity::Quiet) {
        m_logger.Step(step_idx, total_pipeline_targets,
                      "Compiling unit: " + current_ready + " [" + target.name +
                          "]");
      }

      auto base_args = std::make_shared<std::vector<std::string>>();
      if (!toolchain.is_msvc) {
        base_args->push_back("-fPIC");
        if (m_options.profile == BuildProfile::Release) {
          base_args->push_back("-O3");
          base_args->push_back("-DNDEBUG");
        } else {
          base_args->push_back("-g");
          base_args->push_back("-O0");
        }
      } else {
        if (m_options.profile == BuildProfile::Release) {
          base_args->push_back("/O2");
          base_args->push_back("/DNDEBUG");
          base_args->push_back("/EHsc");
        } else {
          base_args->push_back("/Zi");
          base_args->push_back("/Od");
          base_args->push_back("/EHsc");
        }
      }

      auto eval_cb = [this, &target, &qt](const std::string &cond) {
        return this->evaluateConditionExpression(cond, target, qt->manifest);
      };

      std::vector<std::string> active_flags = target.getActiveFlags(eval_cb);
      for (const auto &flag : active_flags)
        base_args->push_back(flag);

      std::string base =
          qt->manifest->base_dir.empty() ? "." : qt->manifest->base_dir;
      std::string inc_prefix = toolchain.is_msvc ? "/I" : "-I";

      for (const auto &inc : target.include_dirs) {
        fs::path inc_path(inc);
        if (inc_path.is_relative())
          inc_path = fs::path(base) / inc_path;
        base_args->push_back(inc_prefix + inc_path.lexically_normal().string());
      }
      for (const auto &inc : qt->manifest->project.include_dirs) {
        fs::path inc_path(inc);
        if (inc_path.is_relative())
          inc_path = fs::path(base) / inc_path;
        base_args->push_back(inc_prefix + inc_path.lexically_normal().string());
      }

      auto transitive_deps = getTransitiveDependencies(current_ready);
      std::reverse(transitive_deps.begin(), transitive_deps.end());

      std::unordered_set<std::string> seen_includes;
      for (const auto &dep_name : transitive_deps) {
        if (const QualifiedTarget *dep_qt = FindByQualifiedName(dep_name)) {
          if (dep_qt->manifest && dep_qt->manifest->exports) {
            std::string dep_base = dep_qt->manifest->base_dir.empty()
                                       ? "."
                                       : dep_qt->manifest->base_dir;
            for (const auto &export_inc :
                 dep_qt->manifest->exports->include_dirs) {
              fs::path dep_inc_path(export_inc);
              if (dep_inc_path.is_relative())
                dep_inc_path = fs::path(dep_base) / dep_inc_path;
              std::string norm_inc = dep_inc_path.lexically_normal().string();
              if (!seen_includes.contains(norm_inc)) {
                seen_includes.insert(norm_inc);
                base_args->push_back(inc_prefix + norm_inc);
              }
            }
          }
        }
      }

      std::vector<std::string> active_props =
          target.getActiveProperties(eval_cb);
      std::string def_prefix = toolchain.is_msvc ? "/D" : "-D";
      for (const auto &prop_ref : active_props) {
        if (prop_ref.rfind("@", 0) == 0) {
          std::string target_prop = prop_ref.substr(1);
          for (const auto &pg : qt->manifest->property_groups) {
            if (pg.name == target_prop) {
              if (!pg.condition.has_value() ||
                  this->evaluateConditionExpression(pg.condition.value(),
                                                    target, qt->manifest)) {
                for (const auto &def : pg.defines)
                  base_args->push_back(def_prefix + def);
              }
            }
          }
        } else {
          base_args->push_back(def_prefix + prop_ref);
        }
      }

      auto ctx = std::make_shared<ActiveTargetContext>(
          qt, sources, std::vector<std::string>(sources.size()), 0, false,
          false, step_idx);

      fs::path target_obj_dir = fs::path(object_cache_dir) / target.name;
      fs::create_directories(target_obj_dir);

      std::vector<FileTask> localized_tasks;
      for (size_t idx = 0; idx < sources.size(); ++idx) {
        const auto &src = sources[idx];
        fs::path src_path(src);
        std::string ext = src_path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool is_c_file = (ext == ".c");

        std::string chosen_compiler =
            is_c_file ? toolchain.c_compiler : toolchain.cpp_compiler;
        std::string final_std_flag =
            toolchain.is_msvc ? (is_c_file ? "/std:c11" : "/std:c++20")
                              : (is_c_file ? "-std=c11" : "-std=c++23");

        std::string object_filename =
            src_path.filename().string() + (toolchain.is_msvc ? ".obj" : ".o");

        fs::path out_obj_path = target_obj_dir / object_filename;
        std::string obj_file = out_obj_path.string();
        ctx->object_files[idx] = obj_file;

        localized_tasks.push_back({src, obj_file, chosen_compiler,
                                   final_std_flag, base_args, working_directory,
                                   toolchain.is_msvc});
      }

      if (localized_tasks.empty()) {
        ctx->remaining_files = 0;
      } else {
        ctx->remaining_files = localized_tasks.size();
        {
          std::lock_guard<std::mutex> lock(queue_mutex);
          for (auto &task : localized_tasks) {
            global_task_queue.push({ctx, std::move(task)});
          }
        }
        queue_cv.notify_all();
      }
      active_targets_running.push_back(ctx);
    }

    scheduler_cv.wait(sched_lock, [&]() {
      return global_build_failed ||
             (completed_file_tasks.load() > processed_file_tasks);
    });

    if (global_build_failed)
      break;

    processed_file_tasks = completed_file_tasks.load();

    for (auto it = active_targets_running.begin();
         it != active_targets_running.end();) {
      auto ctx = *it;
      if (ctx->remaining_files.load() == 0) {
        if (ctx->failed) {
          global_build_failed = true;
          break;
        }

        const Target &target = ctx->qt->target;
        std::string qualified_name = ctx->qt->qualifiedName;

        std::string target_output_file;
#if defined(_WIN32) || defined(_WIN64)
        if (target.type == TargetType::Executable)
          target_output_file = target_build_dir + "/" + target.name + ".exe";
        else if (target.type == TargetType::StaticLibrary)
          target_output_file = target_build_dir + "/" + target.name + ".lib";
        else if (target.type == TargetType::SharedLibrary)
          target_output_file = target_build_dir + "/" + target.name + ".dll";
#elif defined(__APPLE__)
        if (target.type == TargetType::Executable)
          target_output_file = target_build_dir + "/" + target.name;
        else if (target.type == TargetType::StaticLibrary)
          target_output_file = target_build_dir + "/lib" + target.name + ".a";
        else if (target.type == TargetType::SharedLibrary)
          target_output_file =
              target_build_dir + "/lib" + target.name + ".dylib";
#else
        if (target.type == TargetType::Executable)
          target_output_file = target_build_dir + "/" + target.name;
        else if (target.type == TargetType::StaticLibrary)
          target_output_file = target_build_dir + "/lib" + target.name + ".a";
        else if (target.type == TargetType::SharedLibrary)
          target_output_file = target_build_dir + "/lib" + target.name + ".so";
#endif

        if (ctx->requires_linkage || !fs::exists(target_output_file)) {
          std::string link_cmd;
          auto transitive_deps = getTransitiveDependencies(qualified_name);
          std::reverse(transitive_deps.begin(), transitive_deps.end());

          if (target.type == TargetType::StaticLibrary) {
            link_cmd =
                toolchain.is_msvc
                    ? (toolchain.archiver + " /OUT:" + target_output_file)
                    : (toolchain.archiver + " rcs " + target_output_file);
            for (const auto &obj : ctx->object_files)
              link_cmd += " " + obj;
          } else {
            if (toolchain.is_msvc) {
              link_cmd = "link /OUT:" + target_output_file + " ";
              if (target.type == TargetType::SharedLibrary)
                link_cmd += "/DLL ";
            } else {
              link_cmd =
                  toolchain.cpp_compiler + " " +
                  (target.type == TargetType::SharedLibrary ? "-shared " : "");
              if (m_options.verbosity == Verbosity::Verbose)
                link_cmd += "-v ";
            }

            for (const auto &obj : ctx->object_files)
              link_cmd += " " + obj;

            if (toolchain.is_msvc) {
              for (const auto &dep_name : transitive_deps) {
                if (built_library_map.count(dep_name)) {
                  fs::path lib_path = built_library_map[dep_name];
                  link_cmd +=
                      " " + lib_path.replace_extension(".lib").string() + " ";
                }
              }
              for (const auto &sys_lib : target.system_libs)
                link_cmd += " " + sys_lib + ".lib ";
            } else {
              link_cmd += " -o " + target_output_file + " ";
              for (const auto &dep_name : transitive_deps) {
                if (built_library_map.count(dep_name))
                  link_cmd += " " + built_library_map[dep_name] + " ";
              }
              for (const auto &sys_lib : target.system_libs)
                link_cmd += " -l" + sys_lib + " ";
            }
          }

          int link_res = std::system(link_cmd.c_str());
          if (link_res != 0) {
            m_logger.Error("Linkage stage failed for block target: " +
                           target_output_file);
            global_build_failed = true;
            break;
          }
        }

        built_library_map[qualified_name] = target_output_file;

        {
          std::lock_guard<std::mutex> lock(cache_mutex);
          for (const auto &rec : ctx->computed_cache_records) {
            state_cache[rec.src] = {rec.write_time, rec.hash};
          }
        }

        executeHooks(ctx->qt->manifest, HookTrigger::PostTargetBuild,
                     target.name);

        completed_targets_count++;
        for (const auto &dep : dependents_graph[qualified_name]) {
          in_degree[dep]--;
          if (in_degree[dep] == 0 && pipeline_targets.contains(dep)) {
            ready_targets.push(dep);
          }
        }

        it = active_targets_running.erase(it);
      } else {
        ++it;
      }
    }
  }
  sched_lock.unlock();

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    workers_should_terminate = true;
  }
  queue_cv.notify_all();

  for (auto &worker : workers) {
    if (worker.joinable())
      worker.join();
  }

  if (global_build_failed) {
    m_logger.Error("Concurrency Pipeline Terminated: A step dropped "
                   "compilation or link error.");
    return false;
  }

  std::ofstream out_cache(cache_path);
  if (out_cache.is_open()) {
    for (const auto &[f_path, data] : state_cache) {
      out_cache << f_path << " " << data.first << " " << data.second << "\n";
    }
    out_cache.close();
  }

  if (!compile_commands_entries.empty()) {
    std::ofstream db_file(output_root + "/compile_commands.json");
    if (db_file.is_open()) {
      db_file << "[\n";
      for (size_t i = 0; i < compile_commands_entries.size(); ++i) {
        db_file << compile_commands_entries[i];
        if (i + 1 < compile_commands_entries.size())
          db_file << ",\n";
      }
      db_file << "\n]";
      db_file.close();
    }
  }

  if (!m_allTargets.empty()) {
    executeHooks(m_allTargets.front().manifest, HookTrigger::PostBuild, "");
  }

  auto build_end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            build_end_time - build_start_time)
                            .count();

  if (m_options.verbosity != Verbosity::Quiet) {
    m_logger.Success("Build completed successfully in " +
                     std::to_string(total_duration) + "ms.");
    m_logger.Info("Cache Summary: " + std::to_string(total_cache_hits) +
                  " hits, " + std::to_string(total_cache_misses) + " misses.");
  }

  return true;
}

} // namespace mokai
