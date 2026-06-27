#include "graph.hpp"
#include "cli/cli.hpp"
#include "graph/compiler/ToolChainFinder.hpp"
#include "graph/types.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
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

// TODO: codereview , find better place
int Graph::executeCommandFast(const std::vector<std::string> &args) {
  if (args.empty())
    return -1;
#ifdef _WIN32
  // flatten the arguments into a single raw command-line string layout
  std::string cmdLine = "";
  for (size_t i = 0; i < args.size(); ++i) {
    cmdLine += (i > 0 ? " \"" : "\"") + args[i] + "\"";
  }

  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  SecureZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  SecureZeroMemory(&pi, sizeof(pi));

  if (!CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, TRUE, 0, nullptr,
                      nullptr, &si, &pi)) {
    return -1;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return static_cast<int>(exitCode);

#else
  // POSIX Linux, macOS, BSD
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &arg : args) {
    argv.push_back(const_cast<char *>(arg.c_str()));
  }
  argv.push_back(
      nullptr); // POSIX requires a trailing null pointer to terminate the array

  pid_t pid = fork();

  if (pid < 0) {
    // Forking failed entirely at the OS level
    return -1;
  } else if (pid == 0) {
    // Execute the binary directly. This replaces the child process memory space
    // with the compiler (gcc/clang)
    execvp(argv[0], argv.data());

    // If execvp returns, it means the binary wasn't found or isn't executable
    _exit(127);
  } else {
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
      return -1;
    }
    // Explicit crash and signal handling verification
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      // Child was abnormally terminated or crashed by a signal
      return -1;
    }

    return -1;
  }
#endif
}

std::string Graph::escapeJsonString(const std::string &input) {
  std::string output;
  for (char c : input) {
    if (c == '\\' || c == '"')
      output += '\\';
    output += c;
  }
  return output;
}

// creator
std::expected<Graph, std::string>
Graph::Create(std::shared_ptr<ProjectManifest> rootManifest,
              const GlobalOptions &options) {
  mokai::log::Logger init_logger;
  ToolchainFinder finder(init_logger);
  auto result = finder.discover(options.default_compiler);

  if (!result) {
    return std::unexpected(result.error());
  }

  return Graph(std::move(rootManifest), options, std::move(result.value()));
}

// constructor
Graph::Graph(std::shared_ptr<ProjectManifest> rootManifest,
             const GlobalOptions &options, std::unique_ptr<ICompiler> compiler)
    : m_root_manifest(std::move(rootManifest)), m_options(options), m_logger(),
      m_conditionEngine(std::make_unique<ConditionEngine>()),
      m_compiler(std::move(compiler)) {
  populateRegistry(m_root_manifest, m_rootPrefix);
  m_edges = buildEdges();
}
std::string Graph::getCachePath() const {
  return (fs::path(".mokai") / "graph.bin").string();
}

void Graph::populateRegistry(std::shared_ptr<ProjectManifest> manifest,
                             const std::string_view path_prefix) {
  if (!manifest || m_processedManifests.contains(manifest.get()))
    return;
  m_processedManifests.insert(manifest.get());

  fs::path tomlPath = fs::path(manifest->base_dir) / "mokai.toml";
  if (fs::exists(tomlPath)) {
    auto ftime = fs::last_write_time(tomlPath);
    auto duration = ftime.time_since_epoch();
    auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    m_manifestTimestamps[tomlPath.string()] = std::to_string(millis);
  }

  for (auto &target : manifest->targets) {
    std::string qn = generateQualifiedName(path_prefix, target.name);

    if (m_targetRegistry.contains(qn)) {
      m_logger.Error("Duplicate qualified target name detected: '" + qn +
                     "' in manifest location: " + manifest->base_dir);
      throw std::runtime_error(
          "Mokai compilation halted: Target name collision on " + qn);
    }

    m_targetRegistry[qn] = {qn, target, manifest};

    if (!m_resolvedSourcesCache.count(qn)) {
      try {
        m_resolvedSourcesCache[qn] = resolveTargetSources(target, manifest);
      } catch (const std::exception &e) {
        m_logger.Error("Failed to resolve target file components for '" + qn +
                       "': " + e.what());
        continue;
      }
    }
  }

  for (auto &[dep_key, resolved] : manifest->resolved_dependencies) {
    if (resolved.manifest) {
      populateRegistry(resolved.manifest, std::string(path_prefix) +
                                              std::string(m_packageSeparator) +
                                              resolved.manifest->project.name);
    }
  }
}

std::string Graph::generateQualifiedName(const std::string_view prefix,
                                         const std::string_view name) const {
  if (prefix.empty()) {
    return std::string(name);
  }

  std::string result;
  result.reserve(prefix.size() + m_packageSeparator.size() + name.size());

  result.append(prefix);
  result.append(m_packageSeparator);
  result.append(name);

  return result;
}

const QualifiedTarget *
Graph::FindByQualifiedName(const std::string &qualified_name) const {
  auto it = m_targetRegistry.find(qualified_name);
  if (it == m_targetRegistry.end()) {
    m_logger.Warn("Queried non-existent build target token: '" +
                  qualified_name + "'");
    return nullptr;
  }
  return &it->second;
}

std::vector<GraphEdge> Graph::buildEdges() {
  std::vector<GraphEdge> edges;
  edges.reserve(m_targetRegistry.size() * 4);
  for (auto const &[qn, qt] : m_targetRegistry) {
    std::unordered_set<std::string_view> seen_dependencies;
    for (const auto &raw_dep : qt.target.depends_on) {
      for (const auto &to_name : resolveDependsOnEntry(raw_dep, qt)) {
        if (seen_dependencies.contains(to_name)) {
          continue;
        }
        if (!m_targetRegistry.contains(to_name)) {
          m_logger.Error("Target '" + qn +
                         "' references a missing or unresolved dependency: '" +
                         to_name + "'");
          continue;
        }

        seen_dependencies.insert(to_name);
        edges.push_back({qn, to_name});
      }
    }
  }
  return edges;
}

std::vector<std::string>
Graph::resolveDependsOnEntry(const std::string &raw_dep,
                             const QualifiedTarget &from_target) {
  std::vector<std::string> resolved;

  auto get_manifest_prefix =
      [&](const ProjectManifest *manifest) -> std::string_view {
    if (manifest == m_root_manifest.get()) {
      return m_rootPrefix; // Fixed: Match what populateRegistry used
    }

    // Fallback structural scan if it's a sub-dependency manifest
    for (const auto &[qn, qt] : m_targetRegistry) {
      if (qt.manifest.get() == manifest) {
        size_t dot = qn.find('.');
        if (dot != std::string::npos) {
          return std::string_view(qn.data(), dot);
        }
      }
    }
    return "";
  };

  auto find_in_manifest_fast =
      [&](const std::shared_ptr<ProjectManifest> &manifest,
          const std::string_view target_name) -> std::string {
    std::string_view prefix = get_manifest_prefix(manifest.get());
    std::string qn = generateQualifiedName(prefix, target_name);
    return m_targetRegistry.contains(qn) ? qn : "";
  };

  std::string_view dep_view(raw_dep);
  size_t colon = dep_view.find(':');

  if (colon != std::string::npos) {
    std::string_view pkg = dep_view.substr(0, colon);
    std::string_view tgt = dep_view.substr(colon + 1);

    for (const auto &[key, rd] : from_target.manifest->resolved_dependencies) {
      if ((key == pkg || (rd.manifest && rd.manifest->project.name == pkg)) &&
          rd.manifest) {
        std::string found = find_in_manifest_fast(rd.manifest, tgt);
        if (!found.empty()) {
          resolved.push_back(found);
        }
      }
    }

    if (resolved.empty()) {
      m_logger.Warn("Explicit dependency link reference '" + raw_dep +
                    "' could not be mapped to any targets inside '" +
                    from_target.qualifiedName + "'");
    }
    return resolved;
  }

  std::string sibling = find_in_manifest_fast(from_target.manifest, dep_view);
  if (!sibling.empty()) {
    resolved.push_back(sibling);
    return resolved;
  }

  std::unordered_set<std::string> unique_targets;
  for (const auto &[key, rd] : from_target.manifest->resolved_dependencies) {
    if ((key == raw_dep ||
         (rd.manifest && rd.manifest->project.name == raw_dep)) &&
        rd.manifest && rd.manifest->exports) {

      for (const auto &def_tgt : rd.manifest->exports->default_targets) {
        std::string found = find_in_manifest_fast(rd.manifest, def_tgt);
        if (!found.empty() && !unique_targets.contains(found)) {
          unique_targets.insert(found);
          resolved.push_back(found);
        }
      }
    }
  }

  if (resolved.empty()) {
    m_logger.Error(
        "Unresolved compilation unit dependency token discovered: '" + raw_dep +
        "' requested by target node: '" + from_target.qualifiedName + "'");
  }

  return resolved;
}

void Graph::collectTransitive(const std::string &root_node,
                              std::unordered_set<std::string> &visited,
                              std::vector<std::string> &out_libs) {
  struct StackFrame {
    std::string node;
    size_t dep_index;
    std::vector<std::string> resolved_deps;
  };

  std::vector<StackFrame> stack;
  std::unordered_set<std::string> processing;

  const QualifiedTarget *root_qt = FindByQualifiedName(root_node);
  if (!root_qt) {
    m_logger.Error(
        "Root target mapping missing during transitive collection: '" +
        root_node + "'");
    return;
  }

  stack.push_back({root_node, 0, {}});
  processing.insert(root_node);

  while (!stack.empty()) {
    auto &frame = stack.back();
    const QualifiedTarget *qt = FindByQualifiedName(frame.node);

    if (!qt) {
      m_logger.Error("Target link broken during graph traversal loop: '" +
                     frame.node + "'");
      processing.erase(frame.node);
      stack.pop_back();
      continue;
    }

    if (frame.dep_index == 0) {
      for (const auto &raw_dep : qt->target.depends_on) {
        auto resolved = resolveDependsOnEntry(raw_dep, *qt);
        frame.resolved_deps.insert(frame.resolved_deps.end(), resolved.begin(),
                                   resolved.end());
      }
    }

    if (frame.dep_index < frame.resolved_deps.size()) {
      const std::string &dep_name = frame.resolved_deps[frame.dep_index];
      frame.dep_index++;

      if (processing.contains(dep_name)) {
        m_logger.Error(
            "Circular dependency chain detected involving target: '" +
            dep_name + "'");
        continue;
      }

      if (!visited.contains(dep_name)) {
        processing.insert(dep_name);
        stack.push_back({dep_name, 0, {}});
      }
    } else {
      if (frame.node != root_node) {
        visited.insert(frame.node);
        out_libs.push_back(frame.node);
      }
      processing.erase(frame.node);
      stack.pop_back();
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
std::string Graph::getTargetBuildSubdir() const {
  std::string pk =
      (m_options.profile == BuildProfile::RELEASE) ? "release" : "debug";
  return m_root_manifest->output.configs.count(pk)
             ? m_root_manifest->output.configs.at(pk).subdir
             : pk;
}

void Graph::matchGlobPattern(const std::string &pattern,
                             const std::string &base_dir,
                             std::vector<std::string> &matches) {
  std::string pat = pattern;
  if (pat.starts_with("./"))
    pat = pat.substr(2);
  std::replace(pat.begin(), pat.end(), '\\', '/');

  std::string rx = "^";
  for (size_t i = 0; i < pat.length(); ++i) {
    if (pat[i] == '*' && i + 1 < pat.length() && pat[i + 1] == '*') {
      rx += ".*";
      i++;
      if (i + 1 < pat.length() && pat[i + 1] == '/')
        i++;
    } else if (pat[i] == '*') {
      rx += "[^/]*";
    } else if (pat[i] == '?') {
      rx += "[^/]";
    } else if (strchr(".+^$()[]|{}", pat[i])) {
      rx += "\\";
      rx += pat[i];
    } else {
      rx += pat[i];
    }
  }
  rx += "$";

  std::regex filter;
  try {
    filter = std::regex(rx, std::regex_constants::ECMAScript |
                                std::regex_constants::optimize);
  } catch (const std::regex_error &e) {
    m_logger.Error("Malformed glob pattern regex translation: " + rx + " (" +
                   e.what() + ")");
    return;
  }
  std::string root = base_dir.empty() ? "." : base_dir;
  if (!fs::exists(root))
    return;

  size_t dot_idx = pat.rfind('.');
  std::string target_ext = (dot_idx != std::string::npos &&
                            pat.find('*', dot_idx) == std::string::npos)
                               ? pat.substr(dot_idx)
                               : "";

  if (pat.find("**") != std::string::npos) {
    try {
      auto it = fs::recursive_directory_iterator(
          root, fs::directory_options::skip_permission_denied);
      auto end = fs::recursive_directory_iterator();

      while (it != end) {
        try {
          std::string d = it->path().filename().string();
          if (d == "build" || d == ".git" || d == ".mokai" ||
              d == "node_modules") {
            it.disable_recursion_pending();
            ++it;
            continue;
          }

          if (fs::is_regular_file(*it)) {
            if (!target_ext.empty() &&
                it->path().extension().string() != target_ext) {
              ++it;
              continue;
            }

            std::string rel = fs::relative(it->path(), root).string();
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (std::regex_match(rel, filter)) {
              matches.push_back(it->path().lexically_normal().string());
            }
          }
        } catch (const std::exception &e) {
          m_logger.Warn(std::string("Skipping unreadable filesystem entry "
                                    "during recursive glob: ") +
                        e.what());
        }
        ++it;
      }
    } catch (const std::exception &e) {
      m_logger.Error(
          std::string(
              "Fatal error initializing recursive directory iterator: ") +
          e.what());
    }
  } else {
    try {
      for (const auto &e : fs::directory_iterator(
               root, fs::directory_options::skip_permission_denied)) {
        try {
          if (fs::is_regular_file(e)) {
            if (!target_ext.empty() &&
                e.path().extension().string() != target_ext) {
              continue;
            }

            std::string rel = fs::relative(e.path(), root).string();
            std::replace(rel.begin(), rel.end(), '\\', '/');
            if (std::regex_match(rel, filter)) {
              matches.push_back(e.path().lexically_normal().string());
            }
          }
        } catch (const std::exception &ex) {
          m_logger.Warn(
              std::string("Skipping unreadable shallow directory entry: ") +
              ex.what());
        }
      }
    } catch (const std::exception &e) {
      m_logger.Error(
          std::string("Fatal error initializing shallow directory iterator: ") +
          e.what());
    }
  }
}

std::vector<std::string>
Graph::resolveTargetSources(const Target &target,
                            const std::shared_ptr<ProjectManifest> &manifest) {
  std::vector<std::string> res;
  std::string b = manifest->base_dir.empty() ? "." : manifest->base_dir;

  auto ev = [&](const std::string &condition_str) -> bool {
    return m_conditionEngine->evaluate(condition_str);
  };

  for (const auto &s : target.getActiveSources(ev)) {
    if (s.starts_with("@")) {
      std::string_view gn = std::string_view(s).substr(1);
      for (const auto &fg : manifest->file_groups) {
        if (fg.name == gn) {
          for (const auto &p : fg.patterns)
            matchGlobPattern(p, b, res);
          break;
        }
      }
    } else if (s.find_first_of("*?{") != std::string::npos) {
      matchGlobPattern(s, b, res);
    } else {
      std::string_view c = s;
      if (c.starts_with("./"))
        c = c.substr(2);

      fs::path p = fs::path(b) / std::string(c);
      if (fs::exists(p) && fs::is_regular_file(p))
        res.push_back(p.lexically_normal().string());
    }
  }

  // Keep target lists unique and deterministically ordered
  std::sort(res.begin(), res.end());
  res.erase(std::unique(res.begin(), res.end()), res.end());
  return res;
}

std::vector<std::string>
Graph::computeBuildOrder(const std::vector<GraphEdge> &edges) {
  std::vector<std::string> order;
  std::unordered_map<std::string, std::vector<std::string>> adj;
  for (const auto &e : edges)
    adj[e.from].push_back(e.to);
  std::unordered_map<std::string, NodeState> states;
  for (auto const &[qn, qt] : m_targetRegistry)
    states[qn] = NodeState::Unvisited;
  auto dfs = [&](auto &self, const std::string &n) -> bool {
    states[n] = NodeState::Visiting;
    if (adj.count(n)) {
      for (const auto &d : adj[n]) {
        if (!states.count(d))
          continue;
        if (states[d] == NodeState::Visiting)
          return false;
        if (states[d] == NodeState::Unvisited && !self(self, d))
          return false;
      }
    }
    states[n] = NodeState::Done;
    order.push_back(n);
    return true;
  };
  for (auto const &[qn, qt] : m_targetRegistry) {
    if (states[qn] == NodeState::Unvisited && !dfs(dfs, qn))
      return {};
  }
  return order;
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
void Graph::executeHooks(const std::shared_ptr<ProjectManifest> &manifest,
                         HookTrigger trigger, const std::string &target_name) {
  for (const auto &hook : manifest->hooks) {
    if (hook.trigger != trigger ||
        (hook.target.has_value() && hook.target.value() != target_name))
      continue;
    std::string ts = triggerToString(trigger);
    fs::path cp =
        fs::temp_directory_path() / ("mokai_hook_ctx_" + hook.name + ".json");
    std::ofstream ctx_file(cp);
    ctx_file << "{\n  \"trigger\": \"" << escapeJsonString(ts) << "\",\n"
             << "  \"project\": \"" << escapeJsonString(manifest->project.name)
             << "\",\n"
             << "  \"target\": \"" << escapeJsonString(target_name)
             << "\"\n}\n";
    ctx_file.close();
    std::string env = (
#if defined(_WIN32) || defined(_WIN64)
                          "set MOKAI_CONTEXT_FILE="
#else
                          "MOKAI_CONTEXT_FILE="
#endif
                          ) +
                      cp.string() +
                      (
#if defined(_WIN32) || defined(_WIN64)
                          " && "
#else
                          " "
#endif
                          ) +
                      hook.run;
    std::system(env.c_str());
    fs::remove(cp);
  }
}

} // namespace mokai
