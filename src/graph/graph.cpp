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

static std::mutex g_log_mutex;

// ============================================================================
// Platform-Native Process Spawning (Zero-Shell Overhead)
// ============================================================================
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
  for (const auto &arg : args)
    c_args.push_back(const_cast<char *>(arg.c_str()));
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
  std::string cpp_compiler, c_compiler, archiver;
  bool is_msvc = false;
};

static Toolchain discoverToolchain() {
  static Toolchain cached;
  static bool done = false;
  if (!done) {
    const char *env_cxx = std::getenv("CXX");
    const char *env_cc = std::getenv("CC");
    cached.cpp_compiler = env_cxx ? env_cxx : "clang++";
    cached.c_compiler = env_cc ? env_cc : "clang";
    cached.archiver = "ar";
#if defined(_WIN32) || defined(_WIN64)
    if (std::system("where cl > NUL 2>&1") == 0)
      cached = {"cl", "cl", "lib", true};
#endif
    done = true;
  }
  return cached;
}

static std::string escapeJsonString(const std::string &input) {
  std::string output;
  for (char c : input) {
    if (c == '\\' || c == '"')
      output += '\\';
    output += c;
  }
  return output;
}

// ============================================================================
// Binary Cache Logic (Serialization)
// ============================================================================
static void writeString(std::ostream &os, const std::string &s) {
  size_t len = s.size();
  os.write(reinterpret_cast<const char *>(&len), sizeof(len));
  os.write(s.data(), len);
}

static std::string readString(std::istream &is) {
  size_t len;
  is.read(reinterpret_cast<char *>(&len), sizeof(len));
  std::string s(len, '\0');
  is.read(&s[0], len);
  return s;
}

std::string Graph::getCachePath() const {
  return (fs::path(".mokai") / "graph.bin").string();
}

bool Graph::tryLoadGraphCache() {
  std::string path = getCachePath();
  if (!fs::exists(path))
    return false;

  std::ifstream is(path, std::ios::binary);
  if (!is.is_open())
    return false;

  size_t manifestCount;
  is.read(reinterpret_cast<char *>(&manifestCount), sizeof(manifestCount));
  for (size_t i = 0; i < manifestCount; ++i) {
    std::string mPath = readString(is);
    std::string mTime = readString(is);

    // Validate timestamp against disk
    if (!fs::exists(mPath))
      return false;
    std::string currentTime =
        std::to_string(fs::last_write_time(mPath).time_since_epoch().count());
    if (currentTime != mTime)
      return false;
    m_manifestTimestamps[mPath] = currentTime;
  }

  size_t edgeCount;
  is.read(reinterpret_cast<char *>(&edgeCount), sizeof(edgeCount));
  m_edges.clear();
  for (size_t i = 0; i < edgeCount; ++i) {
    std::string from = readString(is);
    std::string to = readString(is);
    m_edges.push_back({from, to});
  }

  return true;
}

void Graph::saveGraphCache() {
  std::ofstream os(getCachePath(), std::ios::binary);
  if (!os.is_open())
    return;

  size_t manifestCount = m_manifestTimestamps.size();
  os.write(reinterpret_cast<const char *>(&manifestCount),
           sizeof(manifestCount));
  for (auto const &[path, time] : m_manifestTimestamps) {
    writeString(os, path);
    writeString(os, time);
  }

  size_t edgeCount = m_edges.size();
  os.write(reinterpret_cast<const char *>(&edgeCount), sizeof(edgeCount));
  for (const auto &edge : m_edges) {
    writeString(os, edge.from);
    writeString(os, edge.to);
  }
}

// ============================================================================
// Core Graph Implementation
// ============================================================================

Graph::Graph(std::shared_ptr<ProjectManifest> rootManifest,
             const GlobalOptions &options)
    : m_options(options), m_root_manifest(rootManifest) {
  m_logger.SetPrefix("mokai");

  // Try loading binary cache to skip re-discovery
  if (!tryLoadGraphCache() || m_options.force_rebuild) {
    populateRegistry(m_root_manifest, m_rootPrefix);
    m_edges = buildEdges();
    saveGraphCache();
  } else {
    // We still need to populate registry data for compilation even if edges are
    // cached
    populateRegistry(m_root_manifest, m_rootPrefix);
  }
}

void Graph::populateRegistry(std::shared_ptr<ProjectManifest> manifest,
                             const std::string &path_prefix) {
  if (!manifest || m_processedManifests.contains(manifest.get()))
    return;
  m_processedManifests.insert(manifest.get());

  // Record timestamp for caching
  fs::path tomlPath = fs::path(manifest->base_dir) / "mokai.toml";
  if (fs::exists(tomlPath)) {
    m_manifestTimestamps[tomlPath.string()] = std::to_string(
        fs::last_write_time(tomlPath).time_since_epoch().count());
  }

  for (auto &target : manifest->targets) {
    std::string qn = generateQualifiedName(path_prefix, target.name);
    m_targetRegistry[qn] = {qn, target, manifest};
  }
  for (auto &[dep_key, resolved] : manifest->resolved_dependencies) {
    if (resolved.manifest) {
      std::string child_prefix =
          path_prefix + m_packageSeparator + resolved.manifest->project.name;
      populateRegistry(resolved.manifest, child_prefix);
    }
  }
}

std::string Graph::generateQualifiedName(const std::string &prefix,
                                         const std::string &name) const {
  return prefix + m_namespaceSeparator + name;
}

const QualifiedTarget *
Graph::FindByQualifiedName(const std::string &qualified_name) const {
  auto it = m_targetRegistry.find(qualified_name);
  return (it != m_targetRegistry.end()) ? &it->second : nullptr;
}

std::vector<GraphEdge> Graph::buildEdges() {
  std::vector<GraphEdge> edges;
  for (auto &[qn, qt] : m_targetRegistry) {
    for (const auto &raw_dep : qt.target.depends_on) {
      for (const auto &to_name : resolveDependsOnEntry(raw_dep, qt)) {
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
  auto find_in_manifest = [&](std::shared_ptr<ProjectManifest> manifest,
                              const std::string &target_name) -> std::string {
    for (const auto &[qn, qt] : m_targetRegistry) {
      if (qt.manifest == manifest && qt.target.name == target_name)
        return qn;
    }
    return "";
  };
  size_t colon = raw_dep.find(':');
  if (colon != std::string::npos) {
    std::string pkg = raw_dep.substr(0, colon), tgt = raw_dep.substr(colon + 1);
    for (const auto &[key, rd] : from_target.manifest->resolved_dependencies) {
      if ((key == pkg || (rd.manifest && rd.manifest->project.name == pkg)) &&
          rd.manifest) {
        std::string found = find_in_manifest(rd.manifest, tgt);
        if (!found.empty())
          resolved.push_back(found);
      }
    }
    return resolved;
  }
  std::string sibling = find_in_manifest(from_target.manifest, raw_dep);
  if (!sibling.empty()) {
    resolved.push_back(sibling);
    return resolved;
  }
  for (const auto &[key, rd] : from_target.manifest->resolved_dependencies) {
    if ((key == raw_dep ||
         (rd.manifest && rd.manifest->project.name == raw_dep)) &&
        rd.manifest && rd.manifest->exports) {
      for (const auto &def_tgt : rd.manifest->exports->default_targets) {
        std::string found = find_in_manifest(rd.manifest, def_tgt);
        if (!found.empty())
          resolved.push_back(found);
      }
    }
  }
  return resolved;
}

void Graph::collectTransitive(const std::string &node,
                              std::unordered_set<std::string> &visited,
                              std::vector<std::string> &out_libs) {
  const QualifiedTarget *qt = FindByQualifiedName(node);
  if (!qt)
    return;
  for (const auto &raw_dep : qt->target.depends_on) {
    for (const auto &dep_name : resolveDependsOnEntry(raw_dep, *qt)) {
      if (visited.insert(dep_name).second) {
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

std::string Graph::getTargetBuildSubdir() const {
  std::string profile_key =
      (m_options.profile == BuildProfile::Release) ? "release" : "debug";
  if (m_root_manifest->output.configs.count(profile_key)) {
    return m_root_manifest->output.configs.at(profile_key).subdir;
  }
  return profile_key;
}

bool Graph::evaluateConditionExpression(
    const std::string &condition, const Target &target,
    const std::shared_ptr<ProjectManifest> &manifest) {
  if (condition.empty())
    return true;
  std::regex expr_regex(R"(([a-zA-Z0-9_\.]+)\s*(==|!=)\s*([a-zA-Z0-9_\.]+))");
  std::smatch m;
  if (!std::regex_match(condition, m, expr_regex))
    return this->evaluateCond(condition);
  std::string left = m[1].str(), op = m[2].str(), right = m[3].str();
  bool res = false;
  if (left == "os") {
#if defined(_WIN32) || defined(_WIN64)
    res = (right == "windows");
#elif defined(__APPLE__)
    res = (right == "macos");
#else
    res = (right == "linux");
#endif
  } else if (left == "compiler") {
    Toolchain tc = discoverToolchain();
    std::string act =
        tc.is_msvc
            ? "msvc"
            : (tc.cpp_compiler.find("clang") != std::string::npos ? "clang"
                                                                  : "gcc");
    res = (act == right);
  } else if (left.starts_with("options.")) {
    std::string k = left.substr(8);
    res = manifest->options.count(k)
              ? (manifest->options.at(k) == (right == "true"))
              : (right == "false");
  } else
    return this->evaluateCond(condition);
  return (op == "==") ? res : !res;
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
    } else if (pat[i] == '*')
      rx += "[^/]*";
    else if (strchr(".+^$()[]|", pat[i])) {
      rx += "\\";
      rx += pat[i];
    } else
      rx += pat[i];
  }
  rx += "$";
  std::regex filter(rx);
  std::string root = base_dir.empty() ? "." : base_dir;
  if (pat.find("**") != std::string::npos) {
    for (auto it = fs::recursive_directory_iterator(root);
         it != fs::recursive_directory_iterator(); ++it) {
      std::string d = it->path().filename().string();
      if (d == "build" || d == ".git" || d == ".mokai") {
        it.disable_recursion_pending();
        continue;
      }
      if (fs::is_regular_file(*it)) {
        std::string rel = fs::relative(it->path(), root).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (std::regex_match(rel, filter))
          matches.push_back(it->path().lexically_normal().string());
      }
    }
  } else {
    if (!fs::exists(root))
      return;
    for (const auto &e : fs::directory_iterator(root)) {
      if (fs::is_regular_file(e)) {
        std::string rel = fs::relative(e.path(), root).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (std::regex_match(rel, filter))
          matches.push_back(e.path().lexically_normal().string());
      }
    }
  }
}

std::vector<std::string>
Graph::resolveTargetSources(const Target &target,
                            const std::shared_ptr<ProjectManifest> &manifest) {
  std::vector<std::string> res;
  std::string b = manifest->base_dir.empty() ? "." : manifest->base_dir;
  auto ev = [&](const std::string &c) {
    return evaluateConditionExpression(c, target, manifest);
  };
  for (const auto &s : target.getActiveSources(ev)) {
    if (s.starts_with("@")) {
      std::string gn = s.substr(1);
      for (const auto &fg : manifest->file_groups) {
        if (fg.name == gn) {
          for (const auto &p : fg.patterns)
            matchGlobPattern(p, b, res);
          break;
        }
      }
    } else if (s.find_first_of("*?{") != std::string::npos)
      matchGlobPattern(s, b, res);
    else {
      std::string c = s;
      if (c.starts_with("./"))
        c = c.substr(2);
      fs::path p = fs::path(b) / c;
      if (fs::exists(p) && fs::is_regular_file(p))
        res.push_back(p.lexically_normal().string());
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

bool Graph::BuildAllTree(const std::vector<std::string> &build_order) {
  if (build_order.empty())
    return true;
  auto start_t = std::chrono::high_resolution_clock::now();
  Toolchain tc = discoverToolchain();

  std::string out_root = m_root_manifest->output.directory;
  std::string sub_p = getTargetBuildSubdir();
  std::string b_dir = out_root + "/" + sub_p, obj_dir = b_dir + "/obj";
  fs::create_directories(obj_dir);

  std::string c_path = "./.mokai/mokai.cache";
  fs::create_directories("./.mokai");
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      state_cache;
  std::mutex c_mutex;
  if (fs::exists(c_path) && !m_options.force_rebuild) {
    std::ifstream f(c_path);
    std::string l, p, t, h;
    while (std::getline(f, l)) {
      std::stringstream ss(l);
      if (ss >> p >> t >> h)
        state_cache[p] = {t, h};
    }
  }

  std::unordered_map<std::string, std::string> lib_map;
  std::vector<std::string> cmds;
  std::mutex db_mutex;
  std::unordered_map<std::string, std::vector<std::string>> dep_graph;
  std::unordered_map<std::string, int> in_degree;
  std::unordered_set<std::string> pipe(build_order.begin(), build_order.end());

  for (const auto &qn : build_order) {
    in_degree[qn] = 0;
    const auto *qt = FindByQualifiedName(qn);
    if (!qt)
      continue;
    for (const auto &rd : qt->target.depends_on) {
      for (const auto &dn : resolveDependsOnEntry(rd, *qt)) {
        if (pipe.contains(dn)) {
          dep_graph[dn].push_back(qn);
          in_degree[qn]++;
        }
      }
    }
  }

  std::queue<std::string> ready;
  std::atomic<bool> failed{false};
  std::atomic<int> completed_f{0};
  int processed_f = 0, total_t = 0, finished_t = 0;
  for (const auto &qn : build_order) {
    total_t++;
    if (in_degree[qn] == 0)
      ready.push(qn);
  }

  std::condition_variable cv;
  std::mutex s_mutex;
  unsigned int n_workers = (m_options.job_count > 0)
                               ? m_options.job_count
                               : std::thread::hardware_concurrency();

  struct Task {
    std::string s, o, c, f, w;
    std::shared_ptr<const std::vector<std::string>> a;
    bool m;
  };
  struct Record {
    std::string s, t, h;
  };
  struct Ctx {
    const QualifiedTarget *q;
    std::atomic<size_t> r{0};
    std::atomic<bool> f{false}, l{false};
    std::vector<Record> recs;
    std::mutex rm;
    std::vector<std::string> objs;
  };

  std::queue<std::pair<std::shared_ptr<Ctx>, Task>> q;
  std::mutex q_mutex;
  std::condition_variable q_cv;
  std::atomic<bool> stop{false};
  std::atomic<size_t> hits{0}, misses{0};

  std::vector<std::thread> workers;
  for (unsigned int i = 0; i < n_workers; ++i) {
    workers.emplace_back([&]() {
      while (true) {
        std::pair<std::shared_ptr<Ctx>, Task> itm;
        {
          std::unique_lock<std::mutex> lk(q_mutex);
          q_cv.wait(lk, [&]() { return !q.empty() || stop || failed; });
          if (stop || failed)
            break;
          itm = std::move(q.front());
          q.pop();
        }
        auto &ctx = itm.first;
        auto &t = itm.second;
        if (failed || ctx->f) {
          ctx->r--;
          completed_f++;
          cv.notify_one();
          continue;
        }
        bool compile = true;
        std::string ct;
        try {
          if (fs::exists(t.s)) {
            ct = std::to_string(
                fs::last_write_time(t.s).time_since_epoch().count());
            std::lock_guard<std::mutex> lk(c_mutex);
            if (!m_options.force_rebuild && state_cache.count(t.s) &&
                state_cache[t.s].first == ct && fs::exists(t.o)) {
              compile = false;
              hits++;
            }
          }
        } catch (...) {
        }
        if (compile) {
          misses++;
          ctx->l = true;
          std::vector<std::string> args;
          args.reserve(t.a->size() + 10);
          args.push_back(t.c);
          args.push_back(t.f);
          for (const auto &a : *t.a)
            args.push_back(a);
          if (t.m) {
            args.push_back("/c");
            args.push_back(t.s);
            args.push_back("/Fo" + t.o);
          } else {
            args.push_back("-c");
            args.push_back(t.s);
            args.push_back("-o");
            args.push_back(t.o);
          }
          std::string fcmd;
          for (const auto &a : args)
            fcmd += a + " ";
          {
            std::lock_guard<std::mutex> lk(db_mutex);
            cmds.push_back("{\"directory\":\"" + escapeJsonString(t.w) +
                           "\",\"command\":\"" + escapeJsonString(fcmd) +
                           "\",\"file\":\"" + escapeJsonString(t.s) +
                           "\",\"output\":\"" + escapeJsonString(t.o) + "\"}");
          }
          if (executeCommandFast(args) != 0) {
            ctx->f = true;
            failed = true;
          } else if (!ct.empty()) {
            std::lock_guard<std::mutex> lk(ctx->rm);
            ctx->recs.push_back({t.s, ct, "-"});
          }
        }
        ctx->r--;
        completed_f++;
        cv.notify_one();
      }
    });
  }

  std::vector<std::shared_ptr<Ctx>> active;
  std::string wd = fs::current_path().string();
  std::unique_lock<std::mutex> sl(s_mutex);

  if (!m_targetRegistry.empty())
    executeHooks(m_root_manifest, HookTrigger::PreBuild, "");

  while (finished_t < total_t && !failed) {
    while (!ready.empty()) {
      std::string cr = ready.front();
      ready.pop();
      const auto *qt = FindByQualifiedName(cr);
      auto srcs = resolveTargetSources(qt->target, qt->manifest);
      if (srcs.empty()) {
        finished_t++;
        lib_map[cr] = qt->target.name;
        for (const auto &d : dep_graph[cr])
          if (--in_degree[d] == 0)
            ready.push(d);
        continue;
      }

      if (m_options.verbosity != Verbosity::Quiet) {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        m_logger.Step(active.size() + finished_t + 1, total_t,
                      "Compiling: " + cr);
      }

      executeHooks(qt->manifest, HookTrigger::PreTargetBuild, qt->target.name);

      auto b_args = std::make_shared<std::vector<std::string>>();
      if (tc.is_msvc) {
        b_args->push_back(m_options.profile == BuildProfile::Release ? "/O2"
                                                                     : "/Zi");
        b_args->push_back("/EHsc");
      } else {
        b_args->push_back("-fPIC");
        b_args->push_back(m_options.profile == BuildProfile::Release ? "-O3"
                                                                     : "-g");
      }

      auto ev = [&](const std::string &c) {
        return evaluateConditionExpression(c, qt->target, qt->manifest);
      };
      for (const auto &f : qt->target.getActiveFlags(ev))
        b_args->push_back(f);

      std::string inc_p = tc.is_msvc ? "/I" : "-I",
                  def_p = tc.is_msvc ? "/D" : "-D";
      auto add_inc = [&](std::string p, std::string b) {
        fs::path fp(p);
        if (p.starts_with("./"))
          fp = fs::path(b) / p.substr(2);
        b_args->push_back(inc_p + fp.lexically_normal().string());
      };
      for (const auto &i : qt->target.include_dirs)
        add_inc(i, qt->manifest->base_dir);
      for (const auto &i : qt->manifest->project.include_dirs)
        add_inc(i, qt->manifest->base_dir);

      auto trans = getTransitiveDependencies(cr);
      std::reverse(trans.begin(), trans.end());
      for (const auto &dn : trans) {
        if (const auto *dqt = FindByQualifiedName(dn))
          if (dqt->manifest->exports)
            for (const auto &i : dqt->manifest->exports->include_dirs)
              add_inc(i, dqt->manifest->base_dir);
      }
      for (const auto &pr : qt->target.getActiveProperties(ev)) {
        if (pr.starts_with("@")) {
          std::string n = pr.substr(1);
          for (const auto &pg : qt->manifest->property_groups)
            if (pg.name == n &&
                (!pg.condition || evaluateConditionExpression(
                                      *pg.condition, qt->target, qt->manifest)))
              for (const auto &d : pg.defines)
                b_args->push_back(def_p + d);
        } else
          b_args->push_back(def_p + pr);
      }

      auto ctx = std::make_shared<Ctx>();
      ctx->q = qt;
      ctx->r = srcs.size();
      ctx->objs.resize(srcs.size());
      fs::create_directories(fs::path(obj_dir) / qt->target.name);
      {
        std::lock_guard<std::mutex> lk(q_mutex);
        for (size_t i = 0; i < srcs.size(); ++i) {
          bool is_c = fs::path(srcs[i]).extension() == ".c" ||
                      fs::path(srcs[i]).extension() == ".m";
          std::string o = (fs::path(obj_dir) / qt->target.name /
                           (fs::path(srcs[i]).filename().string() + "_" +
                            std::to_string(std::hash<std::string>{}(srcs[i])) +
                            (tc.is_msvc ? ".obj" : ".o")))
                              .string();
          ctx->objs[i] = o;
          q.push({ctx,
                  {srcs[i], o, is_c ? tc.c_compiler : tc.cpp_compiler,
                   tc.is_msvc ? (is_c ? "/std:c11" : "/std:c++20")
                              : (is_c ? "-std=c11" : "-std=c++23"),
                   wd, b_args, tc.is_msvc}});
        }
      }
      q_cv.notify_all();
      active.push_back(ctx);
    }
    if (finished_t >= total_t || failed)
      break;
    cv.wait(sl, [&]() { return failed || completed_f > processed_f; });
    processed_f = completed_f;
    for (auto it = active.begin(); it != active.end();) {
      auto ctx = *it;
      if (ctx->r == 0) {
        if (ctx->f) {
          failed = true;
          break;
        }
        std::string out_f = b_dir + "/" + ctx->q->target.name;
#if defined(_WIN32) || defined(_WIN64)
        if (ctx->q->target.type == TargetType::Executable)
          out_f += ".exe";
        else if (ctx->q->target.type == TargetType::StaticLibrary)
          out_f += ".lib";
        else
          out_f += ".dll";
#else
        if (ctx->q->target.type == TargetType::StaticLibrary)
          out_f = b_dir + "/lib" + ctx->q->target.name + ".a";
        else if (ctx->q->target.type == TargetType::SharedLibrary)
          out_f = b_dir + "/lib" + ctx->q->target.name + ".so";
#endif
        if (ctx->l || !fs::exists(out_f)) {
          std::string link;
          auto trans = getTransitiveDependencies(ctx->q->qualifiedName);
          std::reverse(trans.begin(), trans.end());
          if (ctx->q->target.type == TargetType::StaticLibrary) {
            link = tc.is_msvc ? (tc.archiver + " /OUT:" + out_f)
                              : (tc.archiver + " rcs " + out_f);
            for (const auto &o : ctx->objs)
              link += " " + o;
          } else {
            if (tc.is_msvc) {
              link = "link /OUT:" + out_f + " ";
              if (ctx->q->target.type == TargetType::SharedLibrary)
                link += "/DLL ";
            } else
              link =
                  tc.cpp_compiler + " " +
                  (ctx->q->target.type == TargetType::SharedLibrary ? "-shared "
                                                                    : "");
            for (const auto &o : ctx->objs)
              link += " " + o;
            if (tc.is_msvc) {
              for (const auto &dn : trans)
                if (lib_map.count(dn) && !lib_map[dn].empty())
                  link +=
                      " " +
                      fs::path(lib_map[dn]).replace_extension(".lib").string();
              for (const auto &s : ctx->q->target.system_libs)
                link += " " + s + ".lib";
            } else {
              link += " -o " + out_f + " ";
              for (const auto &dn : trans)
                if (lib_map.count(dn) && !lib_map[dn].empty())
                  link += " " + lib_map[dn] + " ";
              for (const auto &s : ctx->q->target.system_libs)
                link += " -l" + s + " ";
            }
          }
          if (std::system(link.c_str()) != 0) {
            failed = true;
            break;
          }
        }
        lib_map[ctx->q->qualifiedName] = out_f;
        {
          std::lock_guard<std::mutex> lk(c_mutex);
          for (const auto &r : ctx->recs)
            state_cache[r.s] = {r.t, r.h};
        }
        executeHooks(ctx->q->manifest, HookTrigger::PostTargetBuild,
                     ctx->q->target.name);
        finished_t++;
        for (const auto &d : dep_graph[ctx->q->qualifiedName])
          if (--in_degree[d] == 0)
            ready.push(d);
        it = active.erase(it);
      } else
        ++it;
    }
  }
  sl.unlock();
  {
    std::lock_guard<std::mutex> lock(q_mutex);
    stop = true;
  }
  q_cv.notify_all();
  for (auto &w : workers)
    if (w.joinable())
      w.join();
  if (failed)
    return false;
  std::ofstream oc(c_path);
  if (oc.is_open())
    for (auto const &[p, d] : state_cache)
      oc << p << " " << d.first << " " << d.second << "\n";
  if (!cmds.empty()) {
    std::ofstream db(b_dir + "/compile_commands.json");
    if (db.is_open()) {
      db << "[\n";
      for (size_t i = 0; i < cmds.size(); ++i)
        db << cmds[i] << (i + 1 < cmds.size() ? ",\n" : "\n");
      db << "]";
    }
  }

  if (!m_targetRegistry.empty())
    executeHooks(m_root_manifest, HookTrigger::PostBuild, "");

  if (m_options.verbosity != Verbosity::Quiet) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    m_logger.Success(
        "Build complete in " +
        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::high_resolution_clock::now() - start_t)
                           .count()) +
        "ms.");
  }
  return true;
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
    {
      std::lock_guard<std::mutex> l(g_log_mutex);
      m_logger.Info("Executing Hook [" + hook.name + "] context '" + ts + "'");
    }
    fs::path cp =
        fs::temp_directory_path() / ("mokai_hook_ctx_" + hook.name + ".json");
    std::ofstream ctx_file(cp);
    ctx_file << "{\n  \"trigger\": \"" << escapeJsonString(ts)
             << "\",\n  \"project\": \""
             << escapeJsonString(manifest->project.name)
             << "\",\n  \"target\": \"" << escapeJsonString(target_name)
             << "\"\n}\n";
    ctx_file.close();
    std::string env_var = (
#if defined(_WIN32) || defined(_WIN64)
        "set MOKAI_CONTEXT_FILE="
#else
        "MOKAI_CONTEXT_FILE="
#endif
    );
    std::string cmd = env_var + cp.string() +
                      (
#if defined(_WIN32) || defined(_WIN64)
                          " && "
#else
                          " "
#endif
                          ) +
                      hook.run;
    std::system(cmd.c_str());
    fs::remove(cp);
  }
}

} // namespace mokai
