#include "cli.hpp"

namespace mokai {
void Cli::initCommands() {
  m_supported_commands = {
      {"create",
       {CREATEUSEANDEXPLANATION,
        [this](auto &args) { return handleCreateProject(args); }}},
      {"add",
       {ADDUSEANDEXPLANATION,
        [this](auto &args) { return handlePackageAdd(args); }}},
      {"build", {BUILDUSEANDEXPLANATION, [this](auto &args) {
                   return handleBuild(args);
                 }}}};
};
} // namespace mokai
