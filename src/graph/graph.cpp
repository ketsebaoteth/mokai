#include "graph.hpp"
#include "cli/cli.hpp"
#include "graph/compiler/tool_chain_finder.hpp"
#include "graph/types.hpp"
#include "log/log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
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

int Graph::executeCommandFast(const std::vector<std::string> &args) {
  return executeCommandFast(args, "");
}

int Graph::executeCommandFast(const std::vector<std::string> &args,
                              const std::string &redirectStdoutTo) {
  if (args.empty()) {
    return -1;
  }

#ifdef _WIN32
  std::string cmdLine;
  bool pipestdout = false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "/showIncludes") {
      pipestdout = true;
    }
    cmdLine += (i > 0 ? " \"" : "\"") + args[i] + "\"";
  }

  HANDLE hRead = nullptr, hWrite = nullptr;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  SecureZeroMemory(&si, sizeof(si));
  SecureZeroMemory(&pi, sizeof(pi));
  si.cb = sizeof(si);

  if (pipestdout) {
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
      return -1;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }

  if (!CreateProcessA(nullptr, &cmdLine[0], nullptr, nullptr, TRUE, 0, nullptr,
                      nullptr, &si, &pi)) {
    if (hRead) {
      CloseHandle(hRead);
    }
    if (hWrite) {
      CloseHandle(hWrite);
    }
    return -1;
  }

  if (pipestdout) {
    CloseHandle(hWrite);
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  if (pipestdout && !redirectStdoutTo.empty()) {
    std::ofstream outFile(redirectStdoutTo);
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr)) {
      if (bytesRead == 0) {
        break;
      }
      buffer[bytesRead] = '\0';
      outFile << buffer;
    }
    CloseHandle(hRead);
  }

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exitCode);
#else
  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &arg : args) {
    argv.push_back(const_cast<char *>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = fork();

  if (pid < 0) {
    return -1;
  } else if (pid == 0) {
    execvp(argv[0], argv.data());
    _exit(127);
  } else {
    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
      return -1;
    }
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      return -1;
    }
    return -1;
  }
#endif
}

std::string Graph::escapeJsonString(const std::string &input) {
  std::string output;
  for (char c : input) {
    if (c == '\\' || c == '"') {
      output += '\\';
    }
    output += c;
  }
  return output;
}

std::expected<Graph, std::string>
Graph::Create(std::shared_ptr<ProjectManifest> rootManifest,
              const GlobalOptions &options) {
  ToolchainFinder finder;
  auto result = finder.discover(options.default_compiler);

  if (!result) {
    return std::unexpected(result.error());
  }
  return Graph(std::move(rootManifest), options, std::move(result.value()));
}

Graph::Graph(std::shared_ptr<ProjectManifest> rootManifest,
             const GlobalOptions &options, std::unique_ptr<ICompiler> compiler)
    : m_root_manifest(std::move(rootManifest)), m_options(options),
      m_conditionEngine(std::make_unique<ConditionEngine>()),
      m_compiler(std::move(compiler)) {
  loadSourcesCache();
  populateRegistry(m_root_manifest, m_rootPrefix);
  saveSourcesCache();
  m_edges = buildEdges();
}

std::string Graph::getCachePath() const {
  return (fs::path(".mokai") / "graph.bin").string();
}

std::string Graph::getSourcesCachePath() const {
  return (fs::path(".mokai") / "sources.cache").string();
}

void Graph::loadSourcesCache() {
  std::string src_cache = getSourcesCachePath();
  if (!fs::exists(src_cache) || m_options.force_rebuild) {
    return;
  }

  std::ifstream file(src_cache, std::ios::binary);
  if (!file.is_open()) {
    return;
  }

  size_t total_targets = 0;
  if (!(file.read(reinterpret_cast<char *>(&total_targets),
                  sizeof(total_targets)))) {
    return;
  }

  for (size_t i = 0; i < total_targets; ++i) {
    size_t qn_len = 0;
    file.read(reinterpret_cast<char *>(&qn_len), sizeof(qn_len));
    std::string qn(qn_len, '\0');
    file.read(&qn[0], qn_len);

    size_t timestamp_len = 0;
    file.read(reinterpret_cast<char *>(&timestamp_len), sizeof(timestamp_len));
    std::string ts(timestamp_len, '\0');
    file.read(&ts[0], timestamp_len);

    size_t paths_count = 0;
    file.read(reinterpret_cast<char *>(&paths_count), sizeof(paths_count));
    std::vector<std::string> paths;
    paths.reserve(paths_count);

    for (size_t j = 0; j < paths_count; ++j) {
      size_t path_len = 0;
      file.read(reinterpret_cast<char *>(&path_len), sizeof(path_len));
      std::string p(path_len, '\0');
      file.read(&p[0], path_len);
      paths.push_back(p);
    }
    m_diskSourcesCache[qn] = {ts, paths};
  }
}

void Graph::saveSourcesCache() {
  fs::create_directories("./.mokai");
  std::ofstream file(getSourcesCachePath(), std::ios::binary);
  if (!file.is_open()) {
    return;
  }

  size_t total_targets = m_resolvedSourcesCache.size();
  file.write(reinterpret_cast<const char *>(&total_targets),
             sizeof(total_targets));

  for (auto const &[qn, paths] : m_resolvedSourcesCache) {
    size_t qn_len = qn.size();
    file.write(reinterpret_cast<const char *>(&qn_len), sizeof(qn_len));
    file.write(qn.data(), qn_len);

    std::string ts = m_manifestTimestamps[m_targetManifestPaths[qn]];
    size_t timestamp_len = ts.size();
    file.write(reinterpret_cast<const char *>(&timestamp_len),
               sizeof(timestamp_len));
    file.write(ts.data(), timestamp_len);

    size_t paths_count = paths.size();
    file.write(reinterpret_cast<const char *>(&paths_count),
               sizeof(paths_count));
    for (const auto &p : paths) {
      size_t path_len = p.size();
      file.write(reinterpret_cast<const char *>(&path_len), sizeof(path_len));
      file.write(p.data(), path_len);
    }
  }
}

void Graph::populateRegistry(std::shared_ptr<ProjectManifest> manifest,
                             const std::string_view path_prefix) {
  if (!manifest || m_processedManifests.contains(manifest.get())) {
    return;
  }
  m_processedManifests.insert(manifest.get());

  fs::path tomlPath = fs::path(manifest->base_dir) / "mokai.toml";
  std::string tomlPathStr = tomlPath.string();
  if (fs::exists(tomlPath)) {
    auto ftime = fs::last_write_time(tomlPath);
    auto duration = ftime.time_since_epoch();
    auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    m_manifestTimestamps[tomlPathStr] = std::to_string(millis);
  }

  for (auto &target : manifest->targets) {
    std::string qn = generateQualifiedName(path_prefix, target.name);

    if (m_targetRegistry.contains(qn)) {
      Log::Error(std::format("Duplicate qualified target name detected: '{}' "
                             "in manifest location: "
                             "{}",
                             qn, manifest->base_dir));
      throw std::runtime_error(
          "Mokai compilation halted: Target name collision on " + qn);
    }

    m_targetRegistry[qn] = {qn, target, manifest};
    m_targetManifestPaths[qn] = tomlPathStr;

    if (!m_resolvedSourcesCache.count(qn)) {
      bool cache_hit = false;
      if (m_diskSourcesCache.count(qn)) {
        const auto &[cached_mtime, cached_paths] = m_diskSourcesCache[qn];
        if (cached_mtime == m_manifestTimestamps[tomlPathStr]) {
          m_resolvedSourcesCache[qn] = cached_paths;
          cache_hit = true;
        }
      }

      if (!cache_hit) {
        try {
          m_resolvedSourcesCache[qn] = resolveTargetSources(target, manifest);
        } catch (const std::exception &e) {
          Log::Error(std::format(
              "Failed to resolve target file components for '{}': {}", qn,
              e.what()));
          continue;
        }
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
    Log::Warn(std::format("Queried non-existent build target token: '{}'",
                          qualified_name));
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
          Log::Error(std::format(
              "Target '{}' references a missing or unresolved dependency: '{}'",
              qn, to_name));
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
      return m_rootPrefix;
    }
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
      Log::Warn(std::format(
          "Explicit dependency link reference '{}' could not be mapped to "
          "any targets inside '{}'",
          raw_dep, from_target.qualifiedName));
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
    Log::Error(std::format(
        "Unresolved compilation unit dependency token discovered: '{}' "
        "requested by target node: '{}'",
        raw_dep, from_target.qualifiedName));
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
    Log::Error(std::format(
        "Root target mapping missing during transitive collection: '{}'",
        root_node));
    return;
  }

  stack.push_back({root_node, 0, {}});
  processing.insert(root_node);

  while (!stack.empty()) {
    auto &frame = stack.back();
    const QualifiedTarget *qt = FindByQualifiedName(frame.node);

    if (!qt) {
      Log::Error(std::format(
          "Target link broken during graph traversal loop: '{}'", frame.node));
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
        Log::Error(std::format(
            "Circular dependency chain detected involving target: '{}'",
            dep_name));
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
  if (pat.starts_with("./")) {
    pat = pat.substr(2);
  }
  std::replace(pat.begin(), pat.end(), '\\', '/');

  std::string rx = "^";
  for (size_t i = 0; i < pat.length(); ++i) {
    if (pat[i] == '*' && i + 1 < pat.length() && pat[i + 1] == '*') {
      rx += ".*";
      i++;
      if (i + 1 < pat.length() && pat[i + 1] == '/') {
        i++;
      }
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
    Log::Error(std::format("Malformed glob pattern regex translation: {} ({})",
                           rx, e.what()));
    return;
  }
  std::string root = base_dir.empty() ? "." : base_dir;
  if (!fs::exists(root)) {
    return;
  }

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
          Log::Warn(std::format("Skipping unreadable filesystem entry during "
                                "recursive glob: {}",
                                e.what()));
        }
        ++it;
      }
    } catch (const std::exception &e) {
      Log::Error(std::format(
          "Fatal error initializing recursive directory iterator: {}",
          e.what()));
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
          Log::Warn(std::format(
              "Skipping unreadable shallow directory entry: {}", ex.what()));
        }
      }
    } catch (const std::exception &e) {
      Log::Error(std::format(
          "Fatal error initializing shallow directory iterator: {}", e.what()));
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
          for (const auto &p : fg.patterns) {
            matchGlobPattern(p, b, res);
          }
          break;
        }
      }
    } else if (s.find_first_of("*?{") != std::string::npos) {
      matchGlobPattern(s, b, res);
    } else {
      std::string_view c = s;
      if (c.starts_with("./")) {
        c = c.substr(2);
      }

      fs::path p = fs::path(b) / std::string(c);
      if (fs::exists(p) && fs::is_regular_file(p)) {
        res.push_back(p.lexically_normal().string());
      } else {
        Log::Warn(std::format(
            "Source file not found, skipping: '{}'",
            p.lexically_normal().string()));
      }
    }
  }

  std::sort(res.begin(), res.end());
  res.erase(std::unique(res.begin(), res.end()), res.end());
  return res;
}

std::vector<std::string>
Graph::computeBuildOrder(const std::vector<GraphEdge> &edges) {
  std::vector<std::string> order;
  std::unordered_map<std::string, std::vector<std::string>> adj;
  for (const auto &e : edges) {
    adj[e.from].push_back(e.to);
  }
  std::unordered_map<std::string, NodeState> states;
  for (auto const &[qn, qt] : m_targetRegistry) {
    states[qn] = NodeState::Unvisited;
  }
  auto dfs = [&](auto &self, const std::string &n) -> bool {
    states[n] = NodeState::Visiting;
    if (adj.count(n)) {
      for (const auto &d : adj[n]) {
        if (!states.count(d)) {
          continue;
        }
        if (states[d] == NodeState::Visiting) {
          return false;
        }
        if (states[d] == NodeState::Unvisited && !self(self, d)) {
          return false;
        }
      }
    }
    states[n] = NodeState::Done;
    order.push_back(n);
    return true;
  };
  for (auto const &[qn, qt] : m_targetRegistry) {
    if (states[qn] == NodeState::Unvisited && !dfs(dfs, qn)) {
      return {};
    }
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
        (hook.target.has_value() && hook.target.value() != target_name)) {
      continue;
    }
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
