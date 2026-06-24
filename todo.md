### Core CLI Lifecycle & Dispatch Mechanics

* [x] `Cli::Cli()`
* [x] `int Cli::Run(int argc, char *argv[])`
* [x] `std::expected<std::monostate, CliError> Cli::ParseCliArgs(int argc, char *argv[])`
* [x] `Cli::~Cli()`

### Sub-Handler Operations (Command Domains)

* [x] `std::expected<std::monostate, CliError> Cli::handleHelp(const std::vector<std::string> &args)`
* [ ] `std::expected<std::monostate, CliError> Cli::handleBuild(const std::vector<std::string> &args)`
* [ ] `std::expected<std::monostate, CliError> Cli::handlePackageAdd(const std::vector<std::string> &args)`
* [ ] `std::expected<std::monostate, CliError> Cli::handleCreateProject(const std::vector<std::string> &args)`

### UI, Formatting, & Interactive Prompt Engines

* [ ] `void printWrapped(const std::string &text, size_t max_line_length, const std::string &indent)`
* [ ] `size_t promptChoice(const std::string &title, const std::vector<std::string> &options, size_t default_idx)`
* [ ] `std::string promptText(const std::string &prompt, const std::string &default_val)`

### Internal System Utilities

* [ ] `void processTemplatePlaceholders(const fs::path &file_path, const std::string &project_name, const std::string &cpp_version)`
* [ ] `void Cli::logSupportedCommands()`

---
