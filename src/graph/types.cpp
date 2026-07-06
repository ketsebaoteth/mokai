#include "graph/types.hpp"

namespace mokai {

// Helper to evaluate and collect elements from a conditional vector
template <typename T, typename GetItemsFn>
static std::vector<std::string> getActiveConditionalItems(
    const std::vector<std::string> &base_items,
    const std::vector<T> &conditional_blocks,
    const std::function<bool(const std::string &)> &eval_fn,
    GetItemsFn get_items) {
  std::vector<std::string> active_items = base_items;

  for (const auto &block : conditional_blocks) {
    if (eval_fn(block.condition)) {
      auto items = get_items(block);
      active_items.insert(active_items.end(), items.begin(), items.end());
    }
  }

  return active_items;
}

std::vector<std::string> Target::getActiveSources(
    const std::function<bool(const std::string &)> &eval_fn) const {
  return getActiveConditionalItems(
      sources, sources_if, eval_fn,
      [](const ConditionalSources &block) { return block.patterns; });
}

std::vector<std::string> Target::getActiveFlags(
    const std::function<bool(const std::string &)> &eval_fn) const {
  return getActiveConditionalItems(
      flags, flags_if, eval_fn,
      [](const ConditionalFlags &block) { return block.flags; });
}

std::vector<std::string> Target::getActiveProperties(
    const std::function<bool(const std::string &)> &eval_fn) const {
  return getActiveConditionalItems(
      properties, properties_if, eval_fn,
      [](const ConditionalProperties &block) { return block.defines; });
}

std::vector<std::string> Target::getActiveSystemLibs(
    const std::function<bool(const std::string &)> &eval_fn) const {
  return getActiveConditionalItems(
      system_libs, system_libs_if, eval_fn,
      [](const ConditionalSystemLibs &block) { return block.libs; });
}

} // namespace mokai
