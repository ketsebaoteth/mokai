#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cli/cli.hpp"
#include "graph.hpp"
#include "graph/compiler/icompiler.hpp"
#include "link_pipeline.hxx"
#include "log/log.h"

namespace fs = std::filesystem;

namespace mokai {

class Graph::BuildPipeline {
private:
  std::atomic<int> m_total_compile_tasks{0};
  struct DirtyInfo {
    bool needsRecomp;
    std::time_t lastTimeStamp;
    std::string reason;
  };
  std::unordered_map<std::string, DirtyInfo> dirtySources;
  std::vector<std::string> include_dirs;

public:
  struct TargetContext {
    const QualifiedTarget *target_ref;
    std::atomic<size_t> remaining_tasks{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> structural_change{false};
    std::mutex record_mutex;
    std::vector<BuildRecord> records;
    std::vector<std::string> object_files;
  };

  BuildPipeline(Graph &graph, const std::vector<std::string> &build_order)
      : m_graph(graph), m_order(build_order), m_failed(false), m_stop(false),
        m_completed_tasks(0), m_processed_tasks(0), m_finished_targets(0) {
    m_total_targets = static_cast<int>(m_order.size());
    m_working_dir = fs::absolute(fs::current_path()).string();

    std::string raw_dir = m_graph.m_root_manifest->output.directory;
    fs::path base_out = raw_dir;
    bool is_root = (base_out == base_out.root_path());

    if (raw_dir.empty() || is_root) {
      base_out = "./build";
    }

    std::string subdir = m_graph.getTargetBuildSubdir();
    while (subdir.starts_with("/") || subdir.starts_with("\\")) {
      subdir = subdir.substr(1);
    }

    fs::path target_build_path = fs::absolute(base_out / subdir);
    m_build_dir = target_build_path.string();
    m_obj_dir = (target_build_path / "obj").string();
    m_cache_path = "./.mokai/mokai.cache";
  }

  ~BuildPipeline() { stopWorkers(); }

  int get_compiled_count() const { return m_total_compile_tasks.load(); }

  bool execute() {
    if (m_order.empty())
      return true;
    if (!m_graph.m_compiler)
      return false;

    PerfTimer timer;

    {
      fs::create_directories(m_obj_dir);
      fs::create_directories("./.mokai");

      loadStateCache();
      m_graph.executeHooks(m_graph.m_root_manifest, HookTrigger::PreBuild, "");

      computeDirtyTargets();
    }
    timer.Mark("Pipeline: Initialization & Dirty Calc");

    if (!m_needs_any_work && !m_graph.m_options.force_rebuild) {
      if (m_graph.m_options.verbosity != Verbosity::Quiet) {
        Log::Success("Build system synchronized: zero units dirty.");
      }
      return true;
    }

    {
      buildLocalDependencyGraph();
      for (const auto &qn : m_order) {
        if (m_in_degree[qn] == 0) {
          m_ready_queue.push(qn);
        }
      }
    }
    timer.Mark("Pipeline: Graph Setup");

    {
      std::unique_lock<std::mutex> lock(m_state_mutex);
      while (m_finished_targets < m_total_targets && !m_failed) {
        processReadyTargets();

        if (m_finished_targets >= m_total_targets || m_failed) {
          break;
        }

        m_cv.wait(lock, [this]() {
          return m_failed || m_completed_tasks > m_processed_tasks;
        });
        m_processed_tasks = m_completed_tasks;
        reapCompletedTargets();
      }
    }
    timer.Mark("Pipeline: Mainloop (compile + link wait)");
    stopWorkers();
    timer.Mark("Pipeline: Worker Execution Loop");

    if (m_failed) {
      Log::Error("Build pipeline failed: Compiler exited non-zero.");
      return false;
    }

    saveStateCache();
    m_graph.executeHooks(m_graph.m_root_manifest, HookTrigger::PostBuild, "");
    emitCompileCommands();

    timer.Mark("Pipeline: Post-build Finalization");

    return true;
  }

private:
  void loadStateCache() {
    if (!fs::exists(m_cache_path) || m_graph.m_options.force_rebuild)
      return;
    std::ifstream file(m_cache_path);
    std::string line, path, timestamp, hash;
    while (std::getline(file, line)) {
      std::stringstream ss(line);
      if (ss >> path >> timestamp >> hash) {
        m_state_cache[path] = {timestamp, hash};
      }
    }
  }

  void saveStateCache() {
    std::ofstream file(m_cache_path);
    if (!file.is_open())
      return;
    for (auto const &[path, data] : m_state_cache) {
      file << path << " " << data.first << " " << data.second << "\n";
    }
  }

  std::string getOutputPath(const QualifiedTarget *qt) {
    fs::path out(m_build_dir);
    std::string name = qt->target.name;
    if (qt->target.type == TargetType::StaticLibrary) {
      if (m_graph.m_compiler->getType() != CompilerType::MSVC) {
        name = "lib" + name;
      }
      out /=
          name +
          (m_graph.m_compiler->getType() == CompilerType::MSVC ? ".lib" : ".a");
    } else if (qt->target.type == TargetType::Executable) {
      out /=
          name +
          (m_graph.m_compiler->getType() == CompilerType::MSVC ? ".exe" : "");
    }
    return fs::absolute(out).string();
  }

  void computeDirtyTargets() {
    m_needs_any_work = false;
    for (const auto &qn : m_order) {
      const auto *qt = m_graph.FindByQualifiedName(qn);
      if (!qt)
        continue;

      std::string out_file = getOutputPath(qt);
      bool dirty = !fs::exists(out_file) || m_graph.m_options.force_rebuild;

      if (!dirty) {
        for (const auto &src : m_graph.m_resolvedSourcesCache[qn]) {
          std::string ext = m_graph.m_compiler->getObjExtension();
          std::string flattened = fs::relative(src, m_working_dir).string();
          for (char &c : flattened) {
            if (c == '/' || c == '\\' || c == ' ' || c == ':')
              c = '_';
          }
          std::string obj_path =
              (fs::path(m_obj_dir) / qt->target.name / (flattened + ext))
                  .string();

          if (!m_state_cache.count(src) ||
              m_state_cache[src].first !=
                  m_graph.getNormalizedFileTimestamp(src) ||
              headerDirty(obj_path)) {
            Log::Debug(std::format("Target marked dirty: {}", src));
            dirty = true;
            break;
          }
        }
      }
      m_target_needs_link[qn] = dirty;
      if (dirty)
        m_needs_any_work = true;
    }
  }

  void buildLocalDependencyGraph() {
    for (const auto &qn : m_order) {
      m_in_degree[qn] = 0;
    }
    for (const auto &qn : m_order) {
      const auto *qt = m_graph.FindByQualifiedName(qn);
      if (!qt)
        continue;
      for (const auto &rd : qt->target.depends_on) {
        for (const auto &dn : m_graph.resolveDependsOnEntry(rd, *qt)) {
          m_dep_graph[dn].push_back(qn);
          m_in_degree[qn]++;
        }
      }
    }
  }

  void spawnWorkers() {
    unsigned int threads = std::thread::hardware_concurrency();
    for (unsigned int i = 0; i < threads; ++i) {
      m_workers.emplace_back([this]() {
        while (true) {

          std::pair<std::shared_ptr<TargetContext>, Task> item;
          {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock, [this]() {
              return !m_task_queue.empty() || m_stop || m_failed;
            });
            if (m_stop || m_failed)
              break;
            item = std::move(m_task_queue.front());
            m_task_queue.pop();
          }

          auto &ctx = item.first;
          auto &task = item.second;

          int current = m_completed_tasks.load() + 1;
          int total = m_total_compile_tasks.load();
          Log::Compile(current, total,
                       fs::path(task.source).filename().string());

          if (!task.build_args) {
            Log::Error(std::format(
                "Task {} has null build arguments! Skipping.", task.source));
            ctx->failed = true;
            m_failed = true;
            m_cv.notify_one();
            continue;
          }

          std::vector<std::string> args =
              m_graph.m_compiler->OrchestrateCompileArgs(task);

          if (m_graph.m_compiler->getType() == CompilerType::MSVC) {
            if (m_graph.executeCommandFast(
                    args,
                    fs::path(task.output).replace_extension(".d").string()) !=
                0) {
              ctx->failed = true;
              m_failed = true;
            } else {
              std::lock_guard<std::mutex> record_lock(ctx->record_mutex);
              ctx->records.push_back(
                  {task.source, m_graph.getNormalizedFileTimestamp(task.source),
                   "-"});
              for (const auto &hdr : parseDependencyHeaders(task.output)) {
                ctx->records.push_back(
                    {hdr, m_graph.getNormalizedFileTimestamp(hdr), "-"});
              }
            }
          } else {
            if (m_graph.executeCommandFast(args) != 0) {
              ctx->failed = true;
              m_failed = true;
            } else {
              std::lock_guard<std::mutex> record_lock(ctx->record_mutex);
              ctx->records.push_back(
                  {task.source, m_graph.getNormalizedFileTimestamp(task.source),
                   "-"});
              for (const auto &hdr : parseDependencyHeaders(task.output)) {
                ctx->records.push_back(
                    {hdr, m_graph.getNormalizedFileTimestamp(hdr), "-"});
              }
            }
          }

          ctx->remaining_tasks--;
          m_completed_tasks++;

          m_cv.notify_one();
        }
      });
    }
  }

  void stopWorkers() {
    {
      std::lock_guard<std::mutex> lock(m_queue_mutex);
      m_stop = true;
    }
    m_queue_cv.notify_all();
    for (auto &worker : m_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    m_workers.clear();
  }

  std::shared_ptr<std::vector<std::string>>
  collectBuildFlags(const QualifiedTarget *qt, const std::string &qn) {
    auto b_args = std::make_shared<std::vector<std::string>>();
    b_args->push_back(m_graph.m_compiler->buildFlag(m_graph.m_options.profile));

    std::string pic = m_graph.m_compiler->positionIndependentCodeFlag();
    if (!pic.empty())
      b_args->push_back(pic);

    auto evaluator = [this](const std::string &cond) {
      return m_graph.m_conditionEngine->evaluate(cond);
    };

    for (const auto &flag : qt->target.getActiveFlags(evaluator)) {
      b_args->push_back(flag);
    }

    for (const auto &define : qt->target.defines)
      b_args->push_back("-D" + define);

    std::unordered_set<std::string> unique_includes;
    auto add_inc = [this, &unique_includes](const std::string &raw_p,
                                            const std::string &base) {
      if (raw_p.empty())
        return;
      fs::path p = raw_p;
      if (p.is_relative())
        p = fs::absolute(fs::path(base) / raw_p);
      unique_includes.insert(
          m_graph.m_compiler->formatInclude(p.lexically_normal().string()));
    };

    for (const auto &i : qt->target.include_dirs)
      add_inc(i, qt->manifest->base_dir);
    for (const auto &i : qt->manifest->project.include_dirs)
      add_inc(i, qt->manifest->base_dir);
    if (qt->manifest->exports) {
      for (const auto &i : qt->manifest->exports->include_dirs)
        add_inc(i, qt->manifest->base_dir);
    }

    auto resolve_includes = [&](auto &self,
                                std::shared_ptr<ProjectManifest> m) -> void {
      for (auto const &[name, dep] : m->resolved_dependencies) {
        if (dep.manifest) {
          add_inc(".", dep.manifest->base_dir);
          for (const auto &i : dep.manifest->project.include_dirs)
            add_inc(i, dep.manifest->base_dir);
          if (dep.manifest->exports) {
            for (const auto &i : dep.manifest->exports->include_dirs)
              add_inc(i, dep.manifest->base_dir);
          }
          self(self, dep.manifest);
        }
      }
    };
    resolve_includes(resolve_includes, qt->manifest);

    for (const auto &inc : unique_includes)
      b_args->push_back(inc);

    for (const auto &pr : qt->target.getActiveProperties(evaluator)) {
      if (pr.starts_with("@")) {
        for (const auto &pg : qt->manifest->property_groups) {
          if (pg.name == pr.substr(1) &&
              (!pg.condition ||
               m_graph.m_conditionEngine->evaluate(*pg.condition))) {
            for (const auto &d : pg.defines)
              b_args->push_back(m_graph.m_compiler->formatDefine(d));
          }
        }
      } else {
        b_args->push_back(m_graph.m_compiler->formatDefine(pr));
      }
    }
    return b_args;
  }

  void
  generateCompilationDatabaseEntries(const std::string &qn,
                                     const QualifiedTarget *qt,
                                     const std::vector<std::string> &args) {
    for (const auto &src : m_graph.m_resolvedSourcesCache[qn]) {
      bool is_c = fs::path(src).extension() == ".c";

      std::string version_str = is_c ? qt->manifest->project.c_version
                                     : qt->manifest->project.cpp_version;

      std::string cmd = m_graph.m_compiler->getCompilerBinary(is_c) + " " +
                        m_graph.m_compiler->standardFlag(version_str, is_c);

      for (const auto &arg : args)
        cmd += " " + arg;
      cmd += " " + m_graph.m_compiler->compileOnlyFlag() + " \"" + src + "\"";

      std::string entry =
          "  {\n    \"directory\": \"" +
          m_graph.escapeJsonString(m_working_dir) + "\",\n    \"command\": \"" +
          m_graph.escapeJsonString(cmd) + "\",\n    \"file\": \"" +
          m_graph.escapeJsonString(src) + "\"\n  }";

      static std::mutex db_mutex;
      std::lock_guard<std::mutex> lock(db_mutex);
      m_compilation_entries.push_back(entry);
    }
    m_compilation_database_dirty = true;
  }

  fs::path resolveHeaderPath(const std::string &token,
                             const std::vector<std::string> &includeDirs,
                             const fs::path &workingDir) {
    fs::path p(token);

    if (p.is_absolute() && fs::exists(p))
      return p;

    fs::path candidate = workingDir / p;
    if (fs::exists(candidate))
      return candidate;

    for (const auto &inc : includeDirs) {
      candidate = fs::path(inc) / p;
      if (fs::exists(candidate))
        return candidate;
    }

    return p;
  }

  bool headerDirty(const std::string &objPath) {
    for (const auto &key : parseDependencyHeaders(objPath)) {
      auto ts = m_graph.getNormalizedFileTimestamp(key);
      auto it = m_state_cache.find(key);
      if (it == m_state_cache.end() || it->second.first != ts)
        return true;
    }
    return false;
  }

  std::vector<std::string> parseDependencyHeaders(const std::string &objPath) {
    std::vector<std::string> headers;
    std::string depFile = fs::path(objPath).replace_extension(".d").string();
    if (!fs::exists(depFile))
      return headers;

    std::ifstream in(depFile);
    std::string token;
    bool first = true;
    while (in >> token) {
      if (token == "\\")
        continue;
      if (first) {
        first = false;
        continue;
      }
      fs::path resolved = resolveHeaderPath(token, include_dirs, m_working_dir);
      if (fs::exists(resolved))
        headers.push_back(resolved.lexically_normal().string());
    }
    return headers;
  }

  void processReadyTargets() {
    while (!m_ready_queue.empty()) {
      std::string cr = m_ready_queue.front();
      m_ready_queue.pop();

      const auto *qt = m_graph.FindByQualifiedName(cr);
      if (!qt)
        continue;

      std::string current_out = getOutputPath(qt);
      if (!m_target_needs_link[cr]) {
        m_lib_path_map[cr] = current_out;
        m_finished_targets++;
        for (const auto &d : m_dep_graph[cr]) {
          if (--m_in_degree[d] == 0)
            m_ready_queue.push(d);
        }
        continue;
      }

      std::shared_ptr<std::vector<std::string>> b_args = nullptr;
      bool lazy_init_done = false;

      auto ctx = std::make_shared<TargetContext>();
      ctx->target_ref = qt;
      ctx->object_files.resize(m_graph.m_resolvedSourcesCache[cr].size());
      fs::create_directories(fs::path(m_obj_dir) / qt->target.name);

      {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        for (size_t i = 0; i < m_graph.m_resolvedSourcesCache[cr].size(); ++i) {
          std::string s = m_graph.m_resolvedSourcesCache[cr][i];
          std::string ext = m_graph.m_compiler->getObjExtension();
          std::string flattened = fs::relative(s, m_working_dir).string();
          for (char &c : flattened) {
            if (c == '/' || c == '\\' || c == ' ' || c == ':')
              c = '_';
          }
          std::string o =
              (fs::path(m_obj_dir) / qt->target.name / (flattened + ext))
                  .string();

          ctx->object_files[i] = o;

          if (m_graph.m_options.force_rebuild || !fs::exists(o) ||
              headerDirty(o) || !m_state_cache.count(s) ||
              m_state_cache[s].first != m_graph.getNormalizedFileTimestamp(s)) {
            if (!lazy_init_done) {
              b_args = collectBuildFlags(qt, cr);
              for (const auto &arg : *b_args) {
                if (arg.rfind("-I", 0) == 0) {
                  include_dirs.push_back(arg.substr(2));
                } else if (arg.rfind("/I", 0) == 0) {
                  include_dirs.push_back(arg.substr(2));
                }
              }
              generateCompilationDatabaseEntries(cr, qt, *b_args);
              lazy_init_done = true;
            }

            ctx->remaining_tasks++;
            ctx->structural_change = true;
            m_total_compile_tasks++;

            bool is_c = fs::path(s).extension() == ".c";
            std::string std_flag = m_graph.m_compiler->standardFlag(
                is_c ? qt->manifest->project.c_version
                     : qt->manifest->project.cpp_version,
                is_c);

            m_task_queue.push(
                {ctx, {s, o, is_c, std_flag, m_working_dir, b_args}});
          }
        }
      }

      if (ctx->remaining_tasks == 0) {
        if (!ctx->object_files.empty() && !fs::exists(current_out)) {
          if (!linkContextTarget(ctx, current_out)) {
            m_failed = true;
            break;
          }
        } else if (ctx->object_files.empty()) {
          Log::Warn(
              std::format("Target '{}' has no resolved source files; skipping.",
                          qt->target.name));
        }
        m_lib_path_map[cr] = current_out;
        m_finished_targets++;
        for (const auto &d : m_dep_graph[cr]) {
          if (--m_in_degree[d] == 0)
            m_ready_queue.push(d);
        }
      } else {
        if (m_workers.empty()) {
          spawnWorkers();
        }
        m_queue_cv.notify_all();
        m_active_contexts.push_back(ctx);
      }
    }
  }

  bool linkContextTarget(std::shared_ptr<TargetContext> ctx,
                         const std::string &out_file) {
    std::vector<std::string> lk_args;

    if (ctx->target_ref->target.type == TargetType::StaticLibrary) {
      lk_args.push_back(m_graph.m_compiler->getArchiverBinary());

      std::string archive_fmt =
          m_graph.m_compiler->formatArchiveCommand(out_file);

      if (archive_fmt.starts_with("rcs \"") && archive_fmt.ends_with("\"")) {
        lk_args.push_back("rcs");
        lk_args.push_back(out_file);
      } else {
        lk_args.push_back(archive_fmt);
      }

      for (const auto &o : ctx->object_files)
        lk_args.push_back(o);
    } else {

      lk_args.push_back(m_graph.m_compiler->getCompilerBinary(false));

      std::vector<std::string> link_candidates;

      for (const auto &o : ctx->object_files)
        link_candidates.push_back(o);

      if (m_graph.m_compiler->getType() != CompilerType::MSVC) {

        std::unordered_set<std::string> visited;

        std::function<void(const QualifiedTarget *)> collectLibraries =
            [&](const QualifiedTarget *qt) {
              if (!qt)
                return;

              if (!visited.insert(qt->qualifiedName).second)
                return;

              for (const auto &dep : qt->target.depends_on) {

                auto resolved = m_graph.resolveDependsOnEntry(dep, *qt);

                for (const auto &name : resolved) {

                  const QualifiedTarget *depTarget =
                      m_graph.FindByQualifiedName(name);

                  if (!depTarget)
                    continue;

                  collectLibraries(depTarget);

                  auto it = m_lib_path_map.find(name);

                  if (it == m_lib_path_map.end())
                    continue;

                  if (it->second.ends_with(".a") ||
                      it->second.ends_with(".lib")) {
                    link_candidates.push_back(it->second);
                  }
                }
              }
            };

        collectLibraries(ctx->target_ref);

        LinkGraphResolver resolver;

        auto ordered = resolver.resolveLinkOrder(link_candidates);

        for (const auto &f : ordered)
          lk_args.push_back(f);

        auto evaluator = [this](const std::string &cond) {
          return m_graph.m_conditionEngine->evaluate(cond);
        };

        for (const auto &lib :
             ctx->target_ref->target.getActiveSystemLibs(evaluator)) {
          lk_args.push_back("-l" + lib);
        }

      } else {

        for (const auto &o : ctx->object_files)
          lk_args.push_back(o);
      }

      lk_args.push_back("-o");
      lk_args.push_back(out_file);
    }

    Log::Info(
        std::format("Linking {}", fs::path(out_file).filename().string()));

    return m_graph.executeCommandFast(lk_args) == 0;
  }

  void reapCompletedTargets() {
    for (auto it = m_active_contexts.begin(); it != m_active_contexts.end();) {
      auto ctx = *it;
      if (ctx->remaining_tasks == 0) {
        if (ctx->failed) {
          m_failed = true;
          break;
        }
        std::string out_file = getOutputPath(ctx->target_ref);
        if (ctx->structural_change || !fs::exists(out_file)) {
          if (!linkContextTarget(ctx, out_file)) {
            m_failed = true;
            break;
          }
        }

        m_lib_path_map[ctx->target_ref->qualifiedName] = out_file;
        m_finished_targets++;

        {
          std::lock_guard<std::mutex> lock(m_cache_mutex);
          for (const auto &r : ctx->records)
            m_state_cache[r.source] = {r.timestamp, r.hash};
        }

        m_graph.executeHooks(ctx->target_ref->manifest,
                             HookTrigger::PostTargetBuild,
                             ctx->target_ref->target.name);

        for (const auto &d : m_dep_graph[ctx->target_ref->qualifiedName]) {
          if (--m_in_degree[d] == 0)
            m_ready_queue.push(d);
        }
        it = m_active_contexts.erase(it);
      } else {
        ++it;
      }
    }
  }

  void emitCompileCommands() {
    if (!m_compilation_database_dirty)
      return;
    std::ofstream file("compile_commands.json");
    if (!file.is_open())
      return;
    file << "[\n";
    for (size_t i = 0; i < m_compilation_entries.size(); ++i) {
      file << m_compilation_entries[i]
           << (i == m_compilation_entries.size() - 1 ? "" : ",\n");
    }
    file << "\n]";
  }

  Graph &m_graph;
  const std::vector<std::string> &m_order;

  std::string m_working_dir;
  std::string m_build_dir;
  std::string m_obj_dir;
  std::string m_cache_path;

  bool m_needs_any_work;
  bool m_compilation_database_dirty = false;
  std::atomic<bool> m_failed;
  std::atomic<bool> m_stop;
  std::atomic<int> m_completed_tasks{0};
  int m_processed_tasks;
  int m_finished_targets;
  int m_total_targets;

  StateCacheMap m_state_cache;
  std::unordered_map<std::string, bool> m_target_needs_link;
  std::unordered_map<std::string, std::string> m_lib_path_map;
  std::unordered_map<std::string, std::vector<std::string>> m_dep_graph;
  std::unordered_map<std::string, int> m_in_degree;

  std::queue<std::string> m_ready_queue;
  std::queue<std::pair<std::shared_ptr<TargetContext>, Task>> m_task_queue;
  std::vector<std::shared_ptr<TargetContext>> m_active_contexts;
  std::vector<std::string> m_compilation_entries;
  std::vector<std::thread> m_workers;

  std::mutex m_state_mutex;
  std::mutex m_queue_mutex;
  std::mutex m_cache_mutex;
  std::condition_variable m_cv;
  std::condition_variable m_queue_cv;
};

bool Graph::BuildAllTree(const std::vector<std::string> &build_order) {
  auto start_time = std::chrono::high_resolution_clock::now();

  BuildPipeline pipeline(*this, build_order);
  if (!pipeline.execute()) {
    return false;
  }

  if (m_options.verbosity != Verbosity::Quiet) {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now() - start_time)
                        .count();
    int compiled = pipeline.get_compiled_count();
    if (compiled > 0) {
      Log::Success(std::format("Build completed in {} ms ({} {} compiled).",
                               duration, compiled,
                               compiled == 1 ? "file" : "files"));
    } else {
      Log::Success(std::format("Build completed in {} ms.", duration));
    }
  }
  return true;
}
} // namespace mokai
