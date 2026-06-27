#pragma once

#include "compiler/Icompiler.hpp"
#include "graph/condition/ConditionEval.hpp"
#include "graph/types.hpp"
#include "log/log.h"
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace mokai {

struct GlobalOptions;

static std::mutex g_log_mutex;

constexpr uint32_t MOKAI_CACHE_MAGIC = 0x4D4F4B41; // "MOKA"
constexpr uint32_t MOKAI_CACHE_VERSION = 1;

class Graph {
public:
  Graph(std::shared_ptr<ProjectManifest> rootManifest,
        const GlobalOptions &options, std::unique_ptr<ICompiler> compiler);

  static std::expected<Graph, std::string>
  Create(std::shared_ptr<ProjectManifest> rootManifest,
         const GlobalOptions &options);

  bool BuildAllTree(const std::vector<std::string> &build_order);

  std::vector<std::string>
  computeBuildOrder(const std::vector<GraphEdge> &edges);
  std::vector<GraphEdge> getEdges() const { return m_edges; }

  std::vector<std::string>
  getTransitiveDependencies(const std::string &qualified_name);

  void executeHooks(const std::shared_ptr<ProjectManifest> &manifest,
                    HookTrigger trigger, const std::string &target_name);

  void writeString(std::ostream &os, const std::string &s);
  std::string readString(std::istream &is);
  void writeVector(std::ostream &os, const std::vector<std::string> &v);
  std::vector<std::string> readVector(std::istream &is);
  std::string escapeJsonString(const std::string &input);

private:
  void populateRegistry(std::shared_ptr<ProjectManifest> manifest,
                        const std::string_view path_prefix);

  // Binary Graph Caching (Includes resolved sources)
  bool tryLoadGraphCache();
  void saveGraphCache();
  std::string getCachePath() const;

  std::string generateQualifiedName(const std::string_view prefix,
                                    const std::string_view name) const;
  std::vector<GraphEdge> buildEdges();

  std::vector<std::string>
  resolveDependsOnEntry(const std::string &raw_dep,
                        const QualifiedTarget &from_target);

  std::vector<std::string>
  resolveTargetSources(const Target &target,
                       const std::shared_ptr<ProjectManifest> &manifest);

  void matchGlobPattern(const std::string &pattern, const std::string &base_dir,
                        std::vector<std::string> &matches);

  std::vector<std::string> expandBraceNotation(const std::string &pattern);

  const QualifiedTarget *
  FindByQualifiedName(const std::string &qualified_name) const;

  void collectTransitive(const std::string &node,
                         std::unordered_set<std::string> &visited,
                         std::vector<std::string> &out_libs);

  std::string getTargetBuildSubdir() const;

  // Direct getter exposing the engine to config.cpp so it can bind options
  // safely
  ConditionEngine &getConditionEngine() { return *m_conditionEngine; }
  const ConditionEngine &getConditionEngine() const {
    return *m_conditionEngine;
  }
  struct BuildRecord {
    std::string source, timestamp, hash;
  };
  using StateCacheMap =
      std::unordered_map<std::string, std::pair<std::string, std::string>>;

  class BuildPipeline;

  int executeCommandFast(const std::vector<std::string> &args);
  std::string
  getNormalizedFileTimestamp(const std::filesystem::path &path) const;

  // Private Member Data fields
  std::unique_ptr<ConditionEngine> m_conditionEngine;
  std::unique_ptr<ICompiler> m_compiler;
  enum class NodeState { Unvisited, Visiting, Done };

  mutable mokai::log::Logger m_logger;
  const GlobalOptions &m_options;

  std::unordered_map<std::string, QualifiedTarget> m_targetRegistry;
  std::unordered_set<ProjectManifest *> m_processedManifests;

  // Cache of Manifest Timestamps
  std::unordered_map<std::string, std::string> m_manifestTimestamps;

  // Cache of Resolved Source Files (FQDN -> List of Paths)
  // This prevents re-running regex globbing on every build.
  std::unordered_map<std::string, std::vector<std::string>>
      m_resolvedSourcesCache;

  std::shared_ptr<ProjectManifest> m_root_manifest;
  std::vector<GraphEdge> m_edges;

  static constexpr std::string_view m_namespaceSeparator = "::";
  static constexpr std::string_view m_packageSeparator = ".";
  static constexpr std::string_view m_rootPrefix = "root";
};

} // namespace mokai
