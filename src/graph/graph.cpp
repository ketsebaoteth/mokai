#include "graph.hpp"
#include "cli/cli.hpp"
#include "graph/types.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace mokai {

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

static std::string calculateFileHash(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open())
    return "0";

  uint64_t hash = 14695981039346656037ULL;
  char buffer[4096];

  while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
    std::streamsize bytes_read = file.gcount();
    for (std::streamsize i = 0; i < bytes_read; ++i) {
      hash ^= static_cast<uint8_t>(buffer[i]);
      hash *= 1099511628211ULL;
    }
  }

  std::stringstream ss;
  ss << std::hex << hash;
  return ss.str();
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
  if (colon == std::string::npos) {
    return false;
  }
  pkg = raw.substr(0, colon);
  target = raw.substr(colon + 1);
  return true;
}

static bool SplitProfile(const std::string &raw, std::string &pkg,
                         std::string &profile) {
  auto slash = raw.find('/');
  if (slash == std::string::npos) {
    return false;
  }
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
  if (!resolved.empty()) {
    return resolved;
  }

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
        if (!states.count(dependency)) {
          continue;
        }

        if (states[dependency] == NodeState::Visiting) {
          std::stringstream error_trace;
          error_trace << "Dependency Cycle Detected! The execution sequence "
                         "cannot be resolved safely.\n"
                      << "    Cycle Path Trace:\n";

          bool track_active_loop = false;
          for (const auto &path_node : active_path) {
            if (path_node == dependency) {
              track_active_loop = true;
            }
            if (track_active_loop) {
              error_trace << "    " << path_node << " ->\n";
            }
          }
          error_trace << "    " << dependency << " (Loop closes here)";

          m_logger.Error(error_trace.str());
          return false;
        }

        if (states[dependency] == NodeState::Unvisited) {
          if (!self(self, dependency)) {
            return false;
          }
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
      if (!dfs(dfs, qt.qualifiedName)) {
        return {};
      }
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
    for (const auto &entry : fs::recursive_directory_iterator(search_root)) {
      if (fs::is_regular_file(entry)) {
        normalize_and_add(entry.path());
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

  // FIXED CORE RE-DEFINITION BUG: Initialized here, used globally down to
  // telemetry summary.
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

  int total_steps = static_cast<int>(build_order.size());
  int current_step = 0;
  std::unordered_map<std::string, std::string> built_library_map;
  std::vector<std::string> compile_commands_entries;
  std::mutex compilation_db_mutex;

  if (!m_allTargets.empty()) {
    executeHooks(m_allTargets.front().manifest, HookTrigger::PreBuild, "");
  }

  for (const auto &qualified_name : build_order) {
    current_step++;

    const QualifiedTarget *qt = FindByQualifiedName(qualified_name);
    if (!qt)
      return false;

    const Target &target = qt->target;

    if (!m_options.target_filter.empty() &&
        target.name != m_options.target_filter) {
      if (m_options.verbosity == Verbosity::Verbose) {
        m_logger.Debug("Filtering out target module execution tree step: " +
                       qualified_name);
      }
      continue;
    }

    std::vector<std::string> sources =
        resolveTargetSources(target, qt->manifest);
    if (sources.empty()) {
      if (m_options.verbosity != Verbosity::Quiet) {
        m_logger.Warn("Skipping build unit '" + qualified_name +
                      "': Active source list evaluates to empty via current "
                      "option configuration.");
      }
      continue;
    }

    total_source_files += sources.size();

    bool file_changed_detected = false;
    for (const auto &hook : qt->manifest->hooks) {
      if (hook.trigger != HookTrigger::FileChange || !hook.pattern.has_value())
        continue;

      std::vector<std::string> hook_matches;
      std::string base =
          qt->manifest->base_dir.empty() ? "." : qt->manifest->base_dir;

      if (hook.pattern.value().rfind("@", 0) == 0) {
        std::string group_target = hook.pattern.value().substr(1);
        for (const auto &fg : qt->manifest->file_groups) {
          if (fg.name == group_target) {
            for (const auto &pattern : fg.patterns)
              matchGlobPattern(pattern, base, hook_matches);
          }
        }
      } else {
        matchGlobPattern(hook.pattern.value(), base, hook_matches);
      }

      for (const auto &file : hook_matches) {
        if (!fs::exists(file))
          continue;
        std::string current_time = std::to_string(
            fs::last_write_time(file).time_since_epoch().count());

        if (m_options.force_rebuild || !state_cache.count(file) ||
            state_cache[file].first != current_time) {
          std::string current_hash = calculateFileHash(file);
          if (m_options.force_rebuild || !state_cache.count(file) ||
              state_cache[file].second != current_hash) {
            file_changed_detected = true;
            state_cache[file] = {current_time, current_hash};
          } else {
            state_cache[file].first = current_time;
          }
        }
      }

      if (file_changed_detected) {
        executeHooks(qt->manifest, HookTrigger::FileChange, target.name);
      }
    }

    executeHooks(qt->manifest, HookTrigger::PreTargetBuild, target.name);

    if (m_options.verbosity != Verbosity::Quiet) {
      m_logger.Step(current_step, total_steps,
                    "Compiling unit: " + qualified_name + " [" + target.name +
                        "]");
    }

    std::stringstream flags_stream;
    if (!toolchain.is_msvc) {
      flags_stream << "-fPIC ";
      if (m_options.profile == BuildProfile::Release) {
        flags_stream << "-O3 -DNDEBUG ";
      } else {
        flags_stream << "-g -O0 ";
      }
      if (m_options.verbosity == Verbosity::Verbose) {
        flags_stream << "-v ";
      }
    } else {
      if (m_options.profile == BuildProfile::Release) {
        flags_stream << "/O2 /DNDEBUG /EHsc ";
      } else {
        flags_stream << "/Zi /Od /EHsc ";
      }
    }

    auto eval_cb = [this, &target, &qt](const std::string &cond) {
      return this->evaluateConditionExpression(cond, target, qt->manifest);
    };

    std::vector<std::string> active_flags = target.getActiveFlags(eval_cb);
    for (const auto &flag : active_flags) {
      flags_stream << flag << " ";
    }

    std::string base =
        qt->manifest->base_dir.empty() ? "." : qt->manifest->base_dir;

    char inc_prefix = toolchain.is_msvc ? '/' : '-';
    for (const auto &inc : target.include_dirs) {
      fs::path inc_path(inc);
      if (inc_path.is_relative())
        inc_path = fs::path(base) / inc_path;
      flags_stream << inc_prefix << "I" << inc_path.lexically_normal().string()
                   << " ";
    }
    for (const auto &inc : qt->manifest->project.include_dirs) {
      fs::path inc_path(inc);
      if (inc_path.is_relative())
        inc_path = fs::path(base) / inc_path;
      flags_stream << inc_prefix << "I" << inc_path.lexically_normal().string()
                   << " ";
    }

    auto transitive_deps = getTransitiveDependencies(qualified_name);
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
            if (dep_inc_path.is_relative()) {
              dep_inc_path = fs::path(dep_base) / dep_inc_path;
            }
            std::string norm_inc = dep_inc_path.lexically_normal().string();

            if (!seen_includes.contains(norm_inc)) {
              seen_includes.insert(norm_inc);
              flags_stream << inc_prefix << "I" << norm_inc << " ";
            }
          }
        }
      }
    }

    std::vector<std::string> active_props = target.getActiveProperties(eval_cb);
    for (const auto &prop_ref : active_props) {
      if (prop_ref.rfind("@", 0) == 0) {
        std::string target_prop = prop_ref.substr(1);
        for (const auto &pg : qt->manifest->property_groups) {
          if (pg.name == target_prop) {
            bool group_allowed = true;
            if (pg.condition.has_value()) {
              group_allowed = this->evaluateConditionExpression(
                  pg.condition.value(), target, qt->manifest);
            }
            if (group_allowed) {
              for (const auto &def : pg.defines)
                flags_stream << (toolchain.is_msvc ? "/D" : "-D") << def << " ";
            }
          }
        }
      } else {
        flags_stream << (toolchain.is_msvc ? "/D" : "-D") << prop_ref << " ";
      }
    }

    std::string shared_user_flags = flags_stream.str();
    std::vector<std::string> object_files(sources.size());
    std::atomic<size_t> next_source_idx(0);
    std::atomic<bool> compilation_failed(false);
    std::atomic<bool> target_requires_linkage(false);

    unsigned int worker_count =
        (m_options.job_count > 0)
            ? static_cast<unsigned int>(m_options.job_count)
            : std::max(1u, std::thread::hardware_concurrency());

    std::vector<std::thread> workers;
    std::string working_directory = fs::current_path().string();
    std::string raw_manifest_std = qt->manifest->project.cpp_version;

    for (unsigned int i = 0; i < worker_count; ++i) {
      workers.emplace_back([&]() {
        while (true) {
          size_t idx = next_source_idx++;
          if (idx >= sources.size() || compilation_failed)
            break;

          const auto &src = sources[idx];
          fs::path src_path(src);
          std::string ext = src_path.extension().string();

          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          bool is_c_file = (ext == ".c");

          std::string chosen_compiler =
              is_c_file ? toolchain.c_compiler : toolchain.cpp_compiler;
          std::string final_std_flag;

          if (toolchain.is_msvc) {
            final_std_flag = is_c_file ? "/std:c11" : "/std:c++20";
          } else {
            if (is_c_file) {
              final_std_flag = (raw_manifest_std.empty() ||
                                raw_manifest_std.rfind("c++", 0) == 0)
                                   ? "-std=c11"
                                   : "-std=" + raw_manifest_std;
            } else {
              final_std_flag = (raw_manifest_std.empty() ||
                                raw_manifest_std.rfind("c++", 0) != 0)
                                   ? "-std=c++23"
                                   : "-std=" + raw_manifest_std;
            }
          }

          std::string object_filename = src_path.filename().string() +
                                        (toolchain.is_msvc ? ".obj" : ".o");
          fs::path out_obj_path =
              fs::path(object_cache_dir) / target.name / object_filename;
          fs::create_directories(out_obj_path.parent_path());

          std::string obj_file = out_obj_path.string();
          object_files[idx] = obj_file;

          std::string current_time = std::to_string(
              fs::last_write_time(src_path).time_since_epoch().count());
          bool need_compile = true;

          if (!m_options.force_rebuild && state_cache.count(src)) {
            if (state_cache[src].first == current_time &&
                fs::exists(obj_file)) {
              need_compile = false;
              total_cache_hits++;
            }
          }

          if (need_compile) {
            total_cache_misses++;
            target_requires_linkage = true;

            std::string command;
            if (toolchain.is_msvc) {
              command = chosen_compiler + " " + final_std_flag + " " +
                        shared_user_flags + " /c " + src + " /Fo" + obj_file;
            } else {
              command = chosen_compiler + " " + final_std_flag + " " +
                        shared_user_flags + " -c " + src + " -o " + obj_file;
            }

            {
              std::lock_guard<std::mutex> lock(compilation_db_mutex);
              std::stringstream json_entry;
              json_entry << "  {\n"
                         << "    \"directory\": \""
                         << escapeJsonString(working_directory) << "\",\n"
                         << "    \"command\": \"" << escapeJsonString(command)
                         << "\",\n"
                         << "    \"file\": \"" << escapeJsonString(src)
                         << "\",\n"
                         << "    \"output\": \"" << escapeJsonString(obj_file)
                         << "\"\n"
                         << "  }";
              compile_commands_entries.push_back(json_entry.str());
            }

            int res = std::system(command.c_str());
            if (res != 0) {
              compilation_failed = true;
            }
          }
        }
      });
    }

    for (auto &worker : workers) {
      if (worker.joinable())
        worker.join();
    }

    if (compilation_failed) {
      m_logger.Error("Compilation graph pass failed for block: " +
                     qualified_name);
      return false;
    }

    std::string target_output_file;
#if defined(_WIN32) || defined(_WIN64)
    if (target.type == TargetType::Executable) {
      target_output_file = target_build_dir + "/" + target.name + ".exe";
    } else if (target.type == TargetType::StaticLibrary) {
      target_output_file = target_build_dir + "/" + target.name + ".lib";
    } else if (target.type == TargetType::SharedLibrary) {
      target_output_file = target_build_dir + "/" + target.name + ".dll";
    }
#elif defined(__APPLE__)
    if (target.type == TargetType::Executable) {
      target_output_file = target_build_dir + "/" + target.name;
    } else if (target.type == TargetType::StaticLibrary) {
      target_output_file = target_build_dir + "/lib" + target.name + ".a";
    } else if (target.type == TargetType::SharedLibrary) {
      target_output_file = target_build_dir + "/lib" + target.name + ".dylib";
    }
#else
    if (target.type == TargetType::Executable) {
      target_output_file = target_build_dir + "/" + target.name;
    } else if (target.type == TargetType::StaticLibrary) {
      target_output_file = target_build_dir + "/lib" + target.name + ".a";
    } else if (target.type == TargetType::SharedLibrary) {
      target_output_file = target_build_dir + "/lib" + target.name + ".so";
    }
#endif

    if (target_requires_linkage || !fs::exists(target_output_file)) {
      std::string link_cmd;
      if (target.type == TargetType::StaticLibrary) {
        if (toolchain.is_msvc) {
          link_cmd = toolchain.archiver + " /OUT:" + target_output_file;
        } else {
          link_cmd = toolchain.archiver + " rcs " + target_output_file;
        }
        for (const auto &obj : object_files) {
          link_cmd += " " + obj;
        }
      } else {
        if (toolchain.is_msvc) {
          link_cmd = "link /OUT:" + target_output_file + " ";
          if (target.type == TargetType::SharedLibrary) {
            link_cmd += "/DLL ";
          }
        } else {
          link_cmd = toolchain.cpp_compiler + " ";
          if (target.type == TargetType::SharedLibrary) {
            link_cmd += "-shared ";
          }
          if (m_options.verbosity == Verbosity::Verbose) {
            link_cmd += "-v ";
          }
        }

        for (const auto &obj : object_files) {
          link_cmd += " " + obj;
        }

        if (toolchain.is_msvc) {
          for (const auto &dep_name : transitive_deps) {
            if (built_library_map.count(dep_name)) {
              fs::path lib_path = built_library_map[dep_name];
              link_cmd +=
                  " " + lib_path.replace_extension(".lib").string() + " ";
            }
          }
          for (const auto &sys_lib : target.system_libs) {
            link_cmd += " " + sys_lib + ".lib ";
          }
        } else {
          link_cmd += " -o " + target_output_file + " ";
          for (const auto &dep_name : transitive_deps) {
            if (built_library_map.count(dep_name)) {
              link_cmd += " " + built_library_map[dep_name] + " ";
            }
          }
          for (const auto &sys_lib : target.system_libs) {
            link_cmd += " -l" + sys_lib + " ";
          }
        }
      }

      int link_res = std::system(link_cmd.c_str());
      if (link_res != 0) {
        m_logger.Error("Linkage stage failed for block target: " +
                       target_output_file);
        return false;
      }
    }

    built_library_map[qualified_name] = target_output_file;

    for (const auto &src : sources) {
      if (fs::exists(src)) {
        std::string write_time =
            std::to_string(fs::last_write_time(src).time_since_epoch().count());
        state_cache[src] = {write_time, calculateFileHash(src)};
      }
    }

    executeHooks(qt->manifest, HookTrigger::PostTargetBuild, target.name);
  }

  std::ofstream out_cache(cache_path);
  if (out_cache.is_open()) {
    for (const auto &[f_path, data] : state_cache) {
      out_cache << f_path << " " << data.first << " " << data.second << "\n";
    }
    out_cache.close();
  }

  if (!compile_commands_entries.empty()) {
    std::ofstream db_file(target_build_dir + "/compile_commands.json");
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

  // FIXED: Purged pure std::cout structures. Telemetry output streams strictly
  // via log routing variants.
  if (m_options.verbosity != Verbosity::Quiet) {
    m_logger.Success("Build completed successfully in " +
                     std::to_string(total_duration) + "ms.");
    m_logger.Info("Cache Summary: " + std::to_string(total_cache_hits) +
                  " hits, " + std::to_string(total_cache_misses) + " misses.");
  }

  return true;
}

} // namespace mokai
