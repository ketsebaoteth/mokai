#include "cli.hpp"

namespace mokai {
void Cli::initCommands() {
  m_supported_commands = {
      {"help",
       {"mokai --help [command]",
        "logs usage and explanation for command specifed",
        [this](auto &args) { return handleHelp(args); }}},
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
