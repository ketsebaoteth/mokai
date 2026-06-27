#pragma once

#include "log/log.h"
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mokai {
class ConditionEngine {
public:
  ConditionEngine() = default;

  void setVariable(std::string_view key, std::string_view value) {
    m_registry[std::string(key)] = std::string(value);
  };

  // evaluates full condifitons
  bool evaluate(const std::string &expression) const;
  void registerSystemPropreties();

private:
  mutable log::Logger m_logger;
  std::unordered_map<std::string, std::string> m_registry;

  std::vector<std::string> tokenize(const std::string &expr) const;
  bool evalAtom(const std::string &left, const std::string &op,
                const std::string &right) const;

  bool parseLogicalExpr(const std::vector<std::string> &tokens,
                        size_t &index) const;

  // 2. Handles Relational Operations: ==, !=, <, >, <=, >= and Unary !
  bool parseBooleanFactor(const std::vector<std::string> &tokens,
                          size_t &index) const;

  // 3. Resolves Raw Identity State (Highest Precedence)
  std::string parseValue(const std::vector<std::string> &tokens,
                         size_t &index) const;
};
} // namespace mokai
