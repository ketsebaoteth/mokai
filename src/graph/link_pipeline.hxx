#include <algorithm>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mokai {

class LinkGraphResolver {
public:
  struct ArtifactNode {
    std::string path;
    std::unordered_set<std::string> provides;
    std::unordered_set<std::string> wants;
    std::vector<std::shared_ptr<ArtifactNode>> dependencies;
  };

  std::vector<std::string>
  resolveLinkOrder(const std::vector<std::string> &unordered_artifacts) {
    std::vector<std::shared_ptr<ArtifactNode>> nodes;
    std::unordered_map<std::string, std::shared_ptr<ArtifactNode>>
        symbol_provider_map;

    for (const auto &path : unordered_artifacts) {
      auto node = std::make_shared<ArtifactNode>();
      node->path = path;

      if (extractSymbols(node)) {
        nodes.push_back(node);
        for (const auto &sym : node->provides) {
          symbol_provider_map[sym] = node;
        }
      }
    }

    for (auto &node : nodes) {
      for (const auto &needed_sym : node->wants) {
        auto it = symbol_provider_map.find(needed_sym);
        // If another file in our pool provides it, create a dependency edge
        if (it != symbol_provider_map.end() && it->second->path != node->path) {
          node->dependencies.push_back(it->second);
        }
      }
    }

    std::vector<std::string> ordered_link_line;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> temp_stack;

    for (const auto &node : nodes) {
      if (visited.find(node->path) == visited.end()) {
        topoSort(node, visited, temp_stack, ordered_link_line);
      }
    }

    std::reverse(ordered_link_line.begin(), ordered_link_line.end());
    return ordered_link_line;
  }

private:
  bool extractSymbols(std::shared_ptr<ArtifactNode> &node) {
    std::string command = "nm -gP " + node->path + " 2>/dev/null";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"),
                                                  pclose);

    if (!pipe)
      return false;

    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
      std::string line(buffer);
      std::istringstream iss(line);
      std::string sym, type;

      if (iss >> sym >> type) {
        if (type == "U") {
          node->wants.insert(sym);
        } else if (type == "T" || type == "W" || type == "D") {
          node->provides.insert(sym);
        }
      }
    }
    return true;
  }

  void topoSort(std::shared_ptr<ArtifactNode> node,
                std::unordered_set<std::string> &visited,
                std::unordered_set<std::string> &temp_stack,
                std::vector<std::string> &output) {

    if (temp_stack.find(node->path) != temp_stack.end()) {
      return;
    }

    if (visited.find(node->path) == visited.end()) {
      temp_stack.insert(node->path);

      for (const auto &dep : node->dependencies) {
        topoSort(dep, visited, temp_stack, output);
      }

      temp_stack.erase(node->path);
      visited.insert(node->path);
      output.push_back(node->path);
    }
  }
};

} // namespace mokai
