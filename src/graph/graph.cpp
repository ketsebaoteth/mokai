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
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <regex>
#include <sstream>
#include <string>
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

inline bool safe_isspace(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

static std::vector<std::string> tokenizeCondition(const std::string &expr) {
  std::vector<std::string> tokens;
  std::string cur;
  for (size_t i = 0; i < expr.size(); ++i) {
    char c = expr[i];
    if (safe_isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
    } else if (c == '(' || c == ')' || c == '!') {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
      tokens.push_back(std::string(1, c));
    } else if (i + 1 < expr.size() && expr.substr(i, 2) == "&&") {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
      tokens.push_back("&&");
      i++;
    } else if (i + 1 < expr.size() && expr.substr(i, 2) == "||") {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
      tokens.push_back("||");
      i++;
    } else if (i + 1 < expr.size() && expr.substr(i, 2) == "==") {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
      tokens.push_back("==");
      i++;
    } else if (i + 1 < expr.size() && expr.substr(i, 2) == "!=") {
      if (!cur.empty()) {
        tokens.push_back(cur);
        cur.clear();
      }
      tokens.push_back("!=");
      i++;
    } else {
      cur += c;
    }
  }
  if (!cur.empty())
    tokens.push_back(cur);
  return tokens;
}

static bool evalAtom(const std::string &left, const std::string &op,
                     const std::string &right, const Target &target,
                     const std::shared_ptr<ProjectManifest> &manifest,
                     Graph *graphInstance) {
  if (left == "os") {
#if defined(_WIN32) || defined(_WIN64)
    return (op == "==" ? right == "windows" : right != "windows");
#elif defined(__APPLE__)
    return (op == "==" ? right == "macos" : right != "macos");
#else
    return (op == "==" ? right == "linux" : right != "linux");
#endif
  } else if (left == "compiler") {
    std::string act = "gcc";
#if defined(__clang__)
    act = "clang";
#elif defined(_MSC_VER)
    act = "msvc";
#endif
    return (op == "==" ? act == right : act != right);
  } else if (left.rfind("options.", 0) == 0) {
    std::string k = left.substr(8);
    bool val = manifest->options.count(k) && manifest->options.at(k);
    if (op == "==")
      return (right == "true" ? val : !val);
    if (op == "!=")
      return (right == "true" ? !val : val);
  }
  return graphInstance->evaluateCond(left + " " + op + " " + right);
}

static bool parseExpr(const std::vector<std::string> &tokens, size_t &pos,
                      const Target &target,
                      const std::shared_ptr<ProjectManifest> &manifest,
                      Graph *graphInstance);
static bool parseTerm(const std::vector<std::string> &tokens, size_t &pos,
                      const Target &target,
                      const std::shared_ptr<ProjectManifest> &manifest,
                      Graph *graphInstance);
static bool parseFactor(const std::vector<std::string> &tokens, size_t &pos,
                        const Target &target,
                        const std::shared_ptr<ProjectManifest> &manifest,
                        Graph *graphInstance);

static bool parseExpr(const std::vector<std::string> &tokens, size_t &pos,
                      const Target &target,
                      const std::shared_ptr<ProjectManifest> &manifest,
                      Graph *graphInstance) {
  bool val = parseTerm(tokens, pos, target, manifest, graphInstance);
  while (pos < tokens.size() && tokens[pos] == "||") {
    pos++;
    val = val || parseTerm(tokens, pos, target, manifest, graphInstance);
  }
  return val;
}

static bool parseTerm(const std::vector<std::string> &tokens, size_t &pos,
                      const Target &target,
                      const std::shared_ptr<ProjectManifest> &manifest,
                      Graph *graphInstance) {
  bool val = parseFactor(tokens, pos, target, manifest, graphInstance);
  while (pos < tokens.size() && tokens[pos] == "&&") {
    pos++;
    val = val && parseFactor(tokens, pos, target, manifest, graphInstance);
  }
  return val;
}

static bool parseFactor(const std::vector<std::string> &tokens, size_t &pos,
                        const Target &target,
                        const std::shared_ptr<ProjectManifest> &manifest,
                        Graph *graphInstance) {
  if (pos >= tokens.size())
    return false;
  if (tokens[pos] == "!") {
    pos++;
    return !parseFactor(tokens, pos, target, manifest, graphInstance);
  }
  if (tokens[pos] == "(") {
    pos++;
    bool val = parseExpr(tokens, pos, target, manifest, graphInstance);
    if (pos < tokens.size() && tokens[pos] == ")")
      pos++;
    return val;
  }
  std::string left = tokens[pos++];
  if (pos < tokens.size() && (tokens[pos] == "==" || tokens[pos] == "!=")) {
    std::string op = tokens[pos++];
    if (pos >= tokens.size())
      return false;
    std::string right = tokens[pos++];
    return evalAtom(left, op, right, target, manifest, graphInstance);
  }
  if (left.rfind("options.", 0) == 0) {
    std::string k = left.substr(8);
    return manifest->options.count(k) && manifest->options.at(k);
  }
  return graphInstance->evaluateCond(left);
}

static int executeCommandFast(const std::vector<std::string> &args) {
  if (args.empty())
    return -1;
#if defined(_WIN32) || defined(_WIN64)
  std::string command;
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
  for (const auto &arg : args)
    c_args.push_back(const_cast<char *>(arg.c_str()));
  c_args.push_back(nullptr);
  pid_t pid;
  int status;
  if (posix_spawnp(&pid, c_args[0], NULL, NULL, c_args.data(), environ) == 0) {
    if (waitpid(pid, &status, 0) != -1 && WIFEXITED(status))
      return WEXITSTATUS(status);
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
    const char *env_cxx = std::getenv("CXX"), *env_cc = std::getenv("CC");
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

static void writeString(std::ostream &os, const std::string &s) {
  size_t len = s.size();
  os.write(reinterpret_cast<const char *>(&len), sizeof(len));
  os.write(s.data(), len);
}

static std::string readString(std::istream &is) {
  size_t len = 0;
  is.read(reinterpret_cast<char *>(&len), sizeof(len));
  std::string s(len, '\0');
  is.read(&s[0], len);
  return s;
}

static void writeVector(std::ostream &os, const std::vector<std::string> &v) {
  size_t sz = v.size();
  os.write(reinterpret_cast<const char *>(&sz), sizeof(sz));
  for (const auto &s : v)
    writeString(os, s);
}

static std::vector<std::string> readVector(std::istream &is) {
  size_t sz = 0;
  is.read(reinterpret_cast<char *>(&sz), sizeof(sz));
  std::vector<std::string> v(sz);
  for (size_t i = 0; i < sz; ++i)
    v[i] = readString(is);
  return v;
}

Graph::Graph(std::shared_ptr<ProjectManifest> rootManifest,
             const GlobalOptions &options)
    : m_options(options), m_root_manifest(rootManifest) {
  m_logger.SetPrefix("mokai");
  if (!tryLoadGraphCache() || m_options.force_rebuild) {
    populateRegistry(m_root_manifest, m_rootPrefix);
    m_edges = buildEdges();
    saveGraphCache();
  } else {
    populateRegistry(m_root_manifest, m_rootPrefix);
  }
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
  try {
    size_t mCount;
    is.read(reinterpret_cast<char *>(&mCount), sizeof(mCount));
    for (size_t i = 0; i < mCount; ++i) {
      std::string p = readString(is), t = readString(is);
      if (!fs::exists(p) ||
          std::to_string(fs::last_write_time(p).time_since_epoch().count()) !=
              t)
        return false;
      m_manifestTimestamps[p] = t;
    }
    size_t eCount;
    is.read(reinterpret_cast<char *>(&eCount), sizeof(eCount));
    m_edges.clear();
    for (size_t i = 0; i < eCount; ++i) {
      std::string f = readString(is), t = readString(is);
      m_edges.push_back({f, t});
    }
    size_t sCount;
    is.read(reinterpret_cast<char *>(&sCount), sizeof(sCount));
    for (size_t i = 0; i < sCount; ++i) {
      std::string qn = readString(is);
      m_resolvedSourcesCache[qn] = readVector(is);
    }
  } catch (...) {
    return false;
  }
  return true;
}

void Graph::saveGraphCache() {
  std::ofstream os(getCachePath(), std::ios::binary);
  if (!os.is_open())
    return;
  size_t mCount = m_manifestTimestamps.size();
  os.write(reinterpret_cast<const char *>(&mCount), sizeof(mCount));
  for (auto const &[p, t] : m_manifestTimestamps) {
    writeString(os, p);
    writeString(os, t);
  }
  size_t eCount = m_edges.size();
  os.write(reinterpret_cast<const char *>(&eCount), sizeof(eCount));
  for (const auto &e : m_edges) {
    writeString(os, e.from);
    writeString(os, e.to);
  }
  size_t sCount = m_resolvedSourcesCache.size();
  os.write(reinterpret_cast<const char *>(&sCount), sizeof(sCount));
  for (auto const &[qn, srcs] : m_resolvedSourcesCache) {
    writeString(os, qn);
    writeVector(os, srcs);
  }
}

void Graph::populateRegistry(std::shared_ptr<ProjectManifest> manifest,
                             const std::string &path_prefix) {
  if (!manifest || m_processedManifests.contains(manifest.get()))
    return;
  m_processedManifests.insert(manifest.get());
  fs::path tomlPath = fs::path(manifest->base_dir) / "mokai.toml";
  if (fs::exists(tomlPath))
    m_manifestTimestamps[tomlPath.string()] = std::to_string(
        fs::last_write_time(tomlPath).time_since_epoch().count());
  for (auto &target : manifest->targets) {
    std::string qn = generateQualifiedName(path_prefix, target.name);
    m_targetRegistry[qn] = {qn, target, manifest};
    if (!m_resolvedSourcesCache.count(qn))
      m_resolvedSourcesCache[qn] = resolveTargetSources(target, manifest);
  }
  for (auto &[dep_key, resolved] : manifest->resolved_dependencies) {
    if (resolved.manifest)
      populateRegistry(resolved.manifest, path_prefix + m_packageSeparator +
                                              resolved.manifest->project.name);
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
      for (const auto &to_name : resolveDependsOnEntry(raw_dep, qt))
        edges.push_back({qn, to_name});
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
  std::string pk =
      (m_options.profile == BuildProfile::Release) ? "release" : "debug";
  return m_root_manifest->output.configs.count(pk)
             ? m_root_manifest->output.configs.at(pk).subdir
             : pk;
}

bool Graph::evaluateConditionExpression(
    const std::string &condition, const Target &target,
    const std::shared_ptr<ProjectManifest> &manifest) {
  if (condition.empty())
    return true;
  auto tokens = tokenizeCondition(condition);
  if (tokens.empty())
    return true;
  size_t pos = 0;
  bool result = parseExpr(tokens, pos, target, manifest, this);
  return (pos == tokens.size()) ? result : false;
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
    if (!fs::exists(root))
      return;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
      std::string d = it->path().filename().string();
      if (d == "build" || d == ".git" || d == ".mokai" || d == "node_modules") {
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

  // --- INTERNAL STRUCTURES ---
  struct Record {
    std::string s, t, h;
  };
  struct Ctx {
    const QualifiedTarget *q;
    std::atomic<size_t> r{0};
    std::atomic<bool> f{false}, l{false};
    std::mutex rm;
    std::vector<Record> recs;
    std::vector<std::string> objs;
  };
  struct Task {
    std::string s, o, c, f, w;
    std::shared_ptr<const std::vector<std::string>> a;
    bool m;
  };

  std::vector<std::string> compilation_entries;
  auto start_t = std::chrono::high_resolution_clock::now();
  Toolchain tc = discoverToolchain();
  std::string wd = fs::absolute(fs::current_path()).string();

  std::string b_dir =
      m_root_manifest->output.directory + "/" + getTargetBuildSubdir();
  std::string obj_dir = b_dir + "/obj";
  fs::create_directories(obj_dir);
  fs::create_directories("./.mokai");
  std::string c_path = "./.mokai/mokai.cache";

  std::mutex c_mutex;
  std::unordered_map<std::string, std::pair<std::string, std::string>>
      state_cache;
  if (fs::exists(c_path) && !m_options.force_rebuild) {
    std::ifstream f(c_path);
    std::string l, p, t, h;
    while (std::getline(f, l)) {
      std::stringstream ss(l);
      if (ss >> p >> t >> h)
        state_cache[p] = {t, h};
    }
  }

  auto get_out_path = [&](const QualifiedTarget *qt) {
    std::string out =
        (fs::path(b_dir) /
         (qt->target.type == TargetType::StaticLibrary && !tc.is_msvc ? "lib"
                                                                      : ""))
            .string();
    out += qt->target.name;
    if (qt->target.type == TargetType::StaticLibrary)
      out += (tc.is_msvc ? ".lib" : ".a");
    else if (qt->target.type == TargetType::Executable)
      out += (tc.is_msvc ? ".exe" : "");
    return fs::absolute(out).string();
  };

  std::unordered_map<std::string, bool> target_needs_link;
  bool needs_any_work = false;
  for (const auto &qn : build_order) {
    const auto *qt = FindByQualifiedName(qn);
    if (!qt)
      continue;
    std::string out_f = get_out_path(qt);
    bool dirty = !fs::exists(out_f) || m_options.force_rebuild;
    if (!dirty) {
      for (const auto &s : m_resolvedSourcesCache[qn]) {
        if (!state_cache.count(s) ||
            state_cache[s].first !=
                std::to_string(
                    fs::last_write_time(s).time_since_epoch().count())) {
          dirty = true;
          break;
        }
      }
    }
    target_needs_link[qn] = dirty;
    if (dirty)
      needs_any_work = true;
  }

  if (!needs_any_work && !m_options.force_rebuild) {
    if (m_options.verbosity != Verbosity::Quiet)
      m_logger.Success("Build is up to date.");
    return true;
  }

  std::unordered_map<std::string, std::string> lib_path_map;
  std::unordered_map<std::string, std::vector<std::string>> dep_graph;
  std::unordered_map<std::string, int> in_degree;
  for (const auto &qn : build_order)
    in_degree[qn] = 0;
  for (const auto &qn : build_order) {
    const auto *qt = FindByQualifiedName(qn);
    if (!qt)
      continue;
    for (const auto &rd : qt->target.depends_on) {
      for (const auto &dn : resolveDependsOnEntry(rd, *qt)) {
        dep_graph[dn].push_back(qn);
        in_degree[qn]++;
      }
    }
  }

  std::queue<std::string> ready;
  for (const auto &qn : build_order) {
    if (in_degree[qn] == 0)
      ready.push(qn);
  }

  std::atomic<bool> failed{false};
  std::atomic<int> completed_f{0};
  int processed_f = 0, total_t = (int)build_order.size(), finished_t = 0;
  std::condition_variable cv;
  std::mutex s_mutex;

  std::queue<std::pair<std::shared_ptr<Ctx>, Task>> q;
  std::mutex q_mutex;
  std::condition_variable q_cv;
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  std::vector<std::shared_ptr<Ctx>> active;

  auto spawn_workers = [&]() {
    for (unsigned int i = 0;
         i < (unsigned int)std::thread::hardware_concurrency(); ++i) {
      workers.emplace_back([&]() {
        while (true) {
          std::pair<std::shared_ptr<Ctx>, Task> itm;
          {
            std::unique_lock<std::mutex> lk{q_mutex};
            q_cv.wait(lk, [&]() { return !q.empty() || stop || failed; });
            if (stop || failed)
              break;
            itm = std::move(q.front());
            q.pop();
          }
          auto &ctx = itm.first;
          auto &t = itm.second;
          std::vector<std::string> args = {t.c, t.f};
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
          if (executeCommandFast(args) != 0) {
            ctx->f = true;
            failed = true;
          } else {
            std::lock_guard<std::mutex> clk{ctx->rm};
            ctx->recs.push_back(
                {t.s,
                 std::to_string(
                     fs::last_write_time(t.s).time_since_epoch().count()),
                 "-"});
          }
          ctx->r--;
          completed_f++;
          cv.notify_one();
        }
      });
    }
  };

  executeHooks(m_root_manifest, HookTrigger::PreBuild, "");
  std::unique_lock<std::mutex> sl{s_mutex};

  while (finished_t < total_t && !failed) {
    while (!ready.empty()) {
      std::string cr = ready.front();
      ready.pop();
      const auto *qt = FindByQualifiedName(cr);
      if (!qt)
        continue;

      std::string current_out = get_out_path(qt);
      if (!target_needs_link[cr]) {
        lib_path_map[cr] = current_out;
        finished_t++;
        for (const auto &d : dep_graph[cr])
          if (--in_degree[d] == 0)
            ready.push(d);
        continue;
      }

      if (workers.empty())
        spawn_workers();
      auto b_args = std::make_shared<std::vector<std::string>>();
      if (tc.is_msvc) {
        b_args->push_back("/O2");
        b_args->push_back("/EHsc");
      } else {
        b_args->push_back("-fPIC");
        b_args->push_back("-O3");
      }

      auto ev = [&](const std::string &c) {
        return evaluateConditionExpression(c, qt->target, qt->manifest);
      };
      for (const auto &f : qt->target.getActiveFlags(ev))
        b_args->push_back(f);

      // --- BULLETPROOF INCLUDE RESOLUTION ---
      std::unordered_set<std::string> unique_includes;
      auto add_inc_safe = [&](const std::string &target_qn,
                              const std::string &raw_p,
                              const std::string &manifest_base) {
        if (raw_p.empty())
          return;
        fs::path p = raw_p;
        if (p.is_relative())
          p = fs::absolute(fs::path(manifest_base) / raw_p);
        std::string final_path = p.lexically_normal().string();
        std::string flag = (tc.is_msvc ? "/I" : "-I") + final_path;
      };

      for (const auto &i : qt->target.include_dirs)
        add_inc_safe(cr, i, qt->manifest->base_dir);
      for (const auto &i : qt->manifest->project.include_dirs)
        add_inc_safe(cr, i, qt->manifest->base_dir);
      if (qt->manifest->exports)
        for (const auto &i : qt->manifest->exports->include_dirs)
          add_inc_safe(cr, i, qt->manifest->base_dir);

      auto resolve_recursive_includes =
          [&](auto &self, std::shared_ptr<ProjectManifest> m) -> void {
        for (auto const &[name, dep] : m->resolved_dependencies) {
          if (dep.manifest) {
            add_inc_safe(cr, ".", dep.manifest->base_dir);
            for (const auto &i : dep.manifest->project.include_dirs)
              add_inc_safe(cr, i, dep.manifest->base_dir);
            if (dep.manifest->exports)
              for (const auto &i : dep.manifest->exports->include_dirs)
                add_inc_safe(cr, i, dep.manifest->base_dir);
            self(self, dep.manifest); // Go deeper
          }
        }
      };
      resolve_recursive_includes(resolve_recursive_includes, qt->manifest);
      for (const auto &inc : unique_includes)
        b_args->push_back(inc);

      for (const auto &pr : qt->target.getActiveProperties(ev)) {
        if (pr.starts_with("@")) {
          for (const auto &pg : qt->manifest->property_groups)
            if (pg.name == pr.substr(1) &&
                (!pg.condition || evaluateConditionExpression(
                                      *pg.condition, qt->target, qt->manifest)))
              for (const auto &d : pg.defines)
                b_args->push_back((tc.is_msvc ? "/D" : "-D") + d);
        } else
          b_args->push_back((tc.is_msvc ? "/D" : "-D") + pr);
      }

      for (const auto &src : m_resolvedSourcesCache[cr]) {
        bool is_c = fs::path(src).extension() == ".c";
        std::string cmd = (is_c ? tc.c_compiler : tc.cpp_compiler) +
                          (tc.is_msvc ? " /std:c11" : " -std=c++23");
        for (const auto &arg : *b_args)
          cmd += " " + arg;
        cmd += (tc.is_msvc ? " /c \"" : " -c \"") + src + "\"";
        std::string entry = "  {\n    \"directory\": \"" +
                            escapeJsonString(wd) + "\",\n    \"command\": \"" +
                            escapeJsonString(cmd) + "\",\n    \"file\": \"" +
                            escapeJsonString(src) + "\"\n  }";
        static std::mutex json_mutex;
        std::lock_guard<std::mutex> jlk{json_mutex};
        compilation_entries.push_back(entry);
      }

      auto ctx = std::make_shared<Ctx>();
      ctx->q = qt;
      ctx->objs.resize(m_resolvedSourcesCache[cr].size());
      fs::create_directories(fs::path(obj_dir) / qt->target.name);
      {
        std::lock_guard<std::mutex> qlk{q_mutex};
        for (size_t i = 0; i < m_resolvedSourcesCache[cr].size(); ++i) {
          std::string s = m_resolvedSourcesCache[cr][i],
                      o = (fs::path(obj_dir) / qt->target.name /
                           (fs::path(s).filename().string() + "_" +
                            std::to_string(std::hash<std::string>{}(s)) +
                            (tc.is_msvc ? ".obj" : ".o")))
                              .string();
          ctx->objs[i] = o;
          if (m_options.force_rebuild || !state_cache.count(s) ||
              state_cache[s].first !=
                  std::to_string(
                      fs::last_write_time(s).time_since_epoch().count()) ||
              !fs::exists(o)) {
            ctx->r++;
            ctx->l = true;
            q.push({ctx,
                    {s, o,
                     fs::path(s).extension() == ".c" ? tc.c_compiler
                                                     : tc.cpp_compiler,
                     (fs::path(s).extension() == ".c"
                          ? (tc.is_msvc ? "/std:c11" : "-std=c11")
                          : (tc.is_msvc ? "/std:c++20" : "-std=c++23")),
                     wd, b_args, tc.is_msvc}});
          }
        }
      }
      if (ctx->r == 0) {
        lib_path_map[cr] = current_out;
        finished_t++;
        for (const auto &d : dep_graph[cr])
          if (--in_degree[d] == 0)
            ready.push(d);
      } else {
        q_cv.notify_all();
        active.push_back(ctx);
      }
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
        std::string out_f = get_out_path(ctx->q);
        if (ctx->l || !fs::exists(out_f)) {
          std::string lk =
              (tc.is_msvc ? (tc.archiver + " /OUT:\"" + out_f + "\" ")
                          : (tc.archiver + " rcs \"" + out_f + "\" "));
          if (ctx->q->target.type != TargetType::StaticLibrary) {
            lk = (tc.is_msvc ? "cl " : (tc.cpp_compiler + " "));
            for (const auto &o : ctx->objs)
              lk += " \"" + o + "\" ";
            if (!tc.is_msvc) {
              lk += " -o \"" + out_f + "\" ";
              std::unordered_set<std::string> linked_artifacts;
              for (const auto &[qn, path] : lib_path_map)
                if (path.find(".a") != std::string::npos)
                  linked_artifacts.insert(path);
              for (const auto &p : linked_artifacts) {
                std::cout << " ℹ [DEBUG] " << ctx->q->qualifiedName
                          << " <- linking: " << p << std::endl;
                lk += " \"" + p + "\" ";
              }
              for (const auto &s : ctx->q->target.system_libs)
                lk += " -l" + s;
            }
          } else {
            for (const auto &o : ctx->objs)
              lk += " \"" + o + "\" ";
          }
          if (std::system(lk.c_str()) != 0) {
            failed = true;
            break;
          }
        }
        lib_path_map[ctx->q->qualifiedName] = out_f;
        finished_t++;
        {
          std::lock_guard<std::mutex> mlk{c_mutex};
          for (const auto &r : ctx->recs)
            state_cache[r.s] = {r.t, r.h};
        }
        executeHooks(ctx->q->manifest, HookTrigger::PostTargetBuild,
                     ctx->q->target.name);
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
    std::lock_guard<std::mutex> stop_lk{q_mutex};
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
  executeHooks(m_root_manifest, HookTrigger::PostBuild, "");

  std::ofstream ccmds("compile_commands.json");
  if (ccmds.is_open()) {
    ccmds << "[\n";
    for (size_t i = 0; i < compilation_entries.size(); ++i)
      ccmds << compilation_entries[i]
            << (i == compilation_entries.size() - 1 ? "" : ",\n");
    ccmds << "\n]";
  }
  if (m_options.verbosity != Verbosity::Quiet) {
    std::lock_guard<std::mutex> log_lk{g_log_mutex};
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
