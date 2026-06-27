#pragma once

#include "Icompiler.hpp"
#include "log/log.h"
#include <expected>
#include <memory>
#include <string>

namespace mokai {
class ToolchainFinder {
public:
  explicit ToolchainFinder(mokai::log::Logger &logger);

  std::expected<std::unique_ptr<ICompiler>, std::string>
  discover(const std::string &user_pref);

private:
  std::string findBinary(const std::string &name) const;
  std::string findUnixBinary(const std::string &name) const;
  std::string findWindowsBinary(const std::string &name) const;

  log::Logger &m_logger;
};
} // namespace mokai
