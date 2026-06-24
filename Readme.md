# Mokai

**A fast, minimal build system, package manager, and developer toolkit for C++ — no new language to learn.**

Mokai lets you describe a C++ project in a few lines of TOML and get a real, working build — no CMake-style scripting, no generator step, no Ninja/Make in between. One tool resolves dependencies, builds your targets in the right order, and links the result, with caching and parallel compilation built in from the start.

```toml
[project]
name = "myapp"
cpp_version = "c++20"

[target.myapp]
type = "executable"
sources = ["./main.cpp"]

```

```bash
mokai build

```

That's a complete, working project.

> **Status: early alpha.** Mokai is under active development. The core build pipeline (config parsing, dependency resolution, dependency graph construction, caching, parallel builds) is implemented and verified cross-platform. Linux stability is guaranteed, and native Windows compilation has succeeded in recent test pipelines. Ready for small to medium-scale production software with local or Git-based source dependencies. Note that native package registry capabilities and cross-compilation are currently under construction. Expect rough edges and breaking changes.

---

## Why Mokai

C++ build tooling has a real, well-known problem: CMake is powerful but has a steep learning curve, requires generating intermediate build files, and lacks an integrated package manager. Alternatives exist, but few combine **a real package manager**, **zero new syntax to learn**, and **a fast, in-process build** in one unified tool.

Mokai's approach:

* **TOML, not a scripting language.** Your project is data — targets, sources, dependencies — not a program the build system has to execute.
* **No intermediate generator step.** Mokai compiles and links directly. No Makefiles or `.ninja` files are generated and handed off to a second tool.
* **Dependencies that just work.** Depend on a library by name or path. Mokai resolves it — locally, from git, or eventually from a recipe registry — and wires up the right include paths, libraries, and link order automatically.
* **Caching by default.** File content hashing plus timestamps means unchanged files are never recompiled. Independent targets and sources compile in parallel.
* **Clear, actionable errors.** Validation errors point at the exact line and field that failed, providing a helpful hint where possible rather than a wall of compiler output.

## What works today

* TOML-based project manifests (`mokai.toml`) with targets, file groups, property groups, conditional sources/flags/defines, and hooks
* Local path and git-based dependency resolution, recursive across nested projects
* Automatic dependency graph construction with topological build ordering and cycle detection
* A glob engine supporting `*`, ``, and brace expansion (`{a,b}`)
* Parallel, content-hash-cached compilation
* Native compilation working on both Linux and Windows environments
* Project bootstrapping via a straight-forward single-command execution pipeline
* `compile_commands.json` generation for editor/IDE tooling (clangd, VS Code, etc.), automatically synchronized to your workspace root or configurable build directory
* Project scaffolding (`mokai create`) with interactive template, C++ standard, and git-init prompts
* Verified against real-world projects, including a full build of **fmt** with conditional compiler-version-gated warning flags, and **SFML 3**, including its transitive dependencies (FreeType, HarfBuzz, SheenBidi, miniaudio, and several X11 extension libraries)

## What's in progress

* A config system in `mokai.toml` to explicitly specify target paths for `compile_commands.json` output (defaults to `build/`)
* A central package registry, so common libraries can be added by name with no manual setup
* Smart version resolution (`sdl >= 2.7`) against upstream git tags
* A shared, machine-wide package cache to avoid duplicate clones across projects
* Cross-compilation toolchain infrastructure
* Expanded diagnostics (typo suggestions, richer source-span errors)

---

## Getting started

### Linux Installation (One-Liner)

Install the pre-compiled, stripped native binary straight into your path:

```bash
curl -fsSL [https://raw.githubusercontent.com/ketsebaoteth/mokai/main/get.sh](https://raw.githubusercontent.com/ketsebaoteth/mokai/main/get.sh) | bash

```

### Scaffolding a New Project

To create a new environment, run:

```bash
mokai create myapp

```

This starts an interactive scaffolder to pick a C++ standard, a starting template, and initialize git:

```
✨ Mokai Initializer – Create a new environment
──────────────────────────────────────────────────
Project name (my_mokai_project) myapp
Select C++ Language Specification Target:
    c++11
    c++14
    c++17
    c++20
  ❯ c++23
  (Use ↑/↓ or j/k to navigate. Enter to select | item 5/6)

Select Project Skeleton Blueprint:
  ❯ minimal
  (Use ↑/↓ or j/k to navigate. Enter to select)

Initialize empty local Git version control tree?
  ❯ Yes
    No
  (Use ↑/↓ or j/k to navigate. Enter to select)

Project setup initialized perfectly!
  Location: /home/k/algos/myapp
  Navigate and trigger production builds via:
  cd myapp
  mokai build

```

### The Build Loop

Mokai eliminates the traditional boilerplate (`CMakeLists.txt`, `cmake_minimum_required`, etc.) and the distinct configuration step (`cmake -B build`). Instead of managing generators and multi-step setups before writing your first line of code, you can go from an empty directory to a running binary in one workflow:

```bash
cd myapp
mokai build
./build/debug/myapp

```

Mokai compiles and links directly. No configuration files are generated for a secondary build tool to read.

---

## Project layout

A minimal `mokai.toml` is a complete, real, buildable project, not just a snippet:

```toml
[project]
name = "myapp"
cpp_version = "c++20"

[target.myapp]
type = "executable"
sources = ["./main.cpp"]

```

### Advanced Layout with LSP Configuration

By default, `mokai` outputs language server compilation databases to your build folder. To override this and specify exactly where `compile_commands.json` should reside, use the `[lsp]` configuration block:

```toml
[project]
name = "myapp"
cpp_version = "c++23"

[lsp]
compile_commands_dir = "." # Places compile_commands.json at the workspace root

[target.myapp]
type = "executable"
sources = ["src/main.cpp"]

```

### Depending on another project

```toml
[project]
name = "testfmt"
cpp_version = "c++23"
dependencies = ["../fmt"]

[target.testfmt]
type = "executable"
sources = ["src/main.cpp"]
depends_on = ["fmt"]

```

This works against any dependency that exposes a matching target name or declares it in `[exports]`. For example, here is a configuration tested against [fmt](https://github.com/fmtlib/fmt):

```toml
# fmt's own mokai.toml (abbreviated)
[project]
name = "fmt"
cpp_version = "c++11"

[target.fmt]
type = "static_library"
sources = ["src/format.cc"]
include_dirs = ["include"]

[exports]
default_targets = ["fmt"]
include_dirs = ["include"]

```

`testfmt` does not need to manage `fmt`'s internal source files, specific compiler flags, or header paths—that configuration lives once within `fmt`'s own manifest.

### Depending on a specific target

Some libraries build multiple artifacts. You can depend on a specific target using the `package:target` syntax:

```toml
[project]
name = "mygame"
cpp_version = "c++23"
dependencies = ["sfml@3.1.0"]

[target.mygame]
type = "executable"
sources = ["src/main.cpp"]
depends_on = ["sfml:sfml-graphics"]

```

This approach allows Mokai to build complex dependency chains, such as [SFML 3](https://github.com/SFML/SFML) along with its transitive dependencies (FreeType, HarfBuzz, SheenBidi, miniaudio, and X11 libraries). The downstream project manifest remains clean, while the structural complexity is contained once at the source dependency level.

*Full configuration reference documentation covering file groups, property groups, conditional compilation, hooks, and exports is currently in progress and will land in `docs/` shortly.*

---

## Building from Source

Mokai is entirely self-hosting. The repository contains a native `mokai.toml` manifest to drive the build pipeline. To build Mokai from source, you use a bootstrapped instance of the Mokai binary itself to handle the target tree compilation automatically.

### Prerequisites

Ensure you have a modern C++ compiler supporting the C++23 standard:
* **Linux:** GCC 13+ or Clang 16+
* **Windows:** MSVC v19.38+ (Visual Studio 2022)

### Building the Project

1. Clone the repository layout:
```bash
git clone [https://github.com/ketsebaoteth/mokai](https://github.com/ketsebaoteth/mokai)
cd mokai

```

2. Bootstrapping Mokai into your localized environment:

```bash
# Pull the latest stable compiler binary to execute the pipeline
curl -fsSL [https://raw.githubusercontent.com/ketsebaoteth/mokai/main/get.sh](https://raw.githubusercontent.com/ketsebaoteth/mokai/main/get.sh) | bash
```

3. Compile the current repository source code directly using Mokai:

```bash
# This reads the native mokai.toml file and builds the updated binary targets
mokai build
```

---

## Contributing

Mokai is in its early stages and the design is still evolving. Issues, feature ideas, and pull requests are welcome. Testing Mokai against libraries you currently use and opening an issue with what broke is highly appreciated.


## License

MIT — see [`LICENSE`](https://www.google.com/search?q=./LICENSE).
