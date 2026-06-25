#pragma once

#include "graph/types.hpp"
#include "log/log.h"
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mokai {

struct GlobalOptions;

class Graph {
public:
  Graph(std::shared_ptr<ProjectManifest> rootManifest,
        const GlobalOptions &options);

  bool BuildAllTree(const std::vector<std::string> &build_order);
  std::vector<std::string>
  computeBuildOrder(const std::vector<GraphEdge> &edges);
  std::vector<GraphEdge> getEdges() const { return m_edges; }
  std::vector<std::string>
  getTransitiveDependencies(const std::string &qualified_name);

  bool
  evaluateConditionExpression(const std::string &condition,
                              const Target &target,
                              const std::shared_ptr<ProjectManifest> &manifest);

  void executeHooks(const std::shared_ptr<ProjectManifest> &manifest,
                    HookTrigger trigger, const std::string &target_name);

private:
  void populateRegistry(std::shared_ptr<ProjectManifest> manifest,
                        const std::string &path_prefix);

  // Binary Graph Caching (Includes resolved sources)
  bool tryLoadGraphCache();
  void saveGraphCache();
  std::string getCachePath() const;

  std::string generateQualifiedName(const std::string &prefix,
                                    const std::string &name) const;
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

  bool evaluateCond(const std::string &cond_expr);
  const QualifiedTarget *
  FindByQualifiedName(const std::string &qualified_name) const;
  void collectTransitive(const std::string &node,
                         std::unordered_set<std::string> &visited,
                         std::vector<std::string> &out_libs);

  std::string getTargetBuildSubdir() const;

private:
  enum class NodeState { Unvisited, Visiting, Done };

  log::Logger m_logger;
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

  const std::string m_namespaceSeparator = "::";
  const std::string m_packageSeparator = ".";
  const std::string m_rootPrefix = "root";
};

} // namespace mokai
