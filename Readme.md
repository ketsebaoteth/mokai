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

```
mokai build
```

That's a complete, working project.

> **Status: early alpha.** Mokai is under active development. The core build pipeline (config parsing, dependency resolution, dependency graph construction, caching, parallel builds) is implemented and has been tested against real-world libraries, including [fmt](https://github.com/fmtlib/fmt) and [SFML](https://github.com/SFML/SFML). The package registry and versioning system described below are designed and in progress. Expect rough edges, breaking changes, and missing features.

---

## Why Mokai

C++ build tooling has a real, well-known problem: CMake is powerful but unfriendly to learn, generates intermediate files for other tools to consume, and has no built-in package manager. Alternatives exist, but none combine **a real package manager**, **zero new syntax to learn**, and **a fast, in-process build** in one tool.

Mokai's approach:

- **TOML, not a scripting language.** Your project is data — targets, sources, dependencies — not a program the build system has to execute.
- **No intermediate generator step.** Mokai compiles and links directly. No Makefiles or `.ninja` files are generated and handed off to a second tool.
- **Dependencies that just work.** Depend on a library by name. Mokai resolves it — locally, from git, or eventually from a recipe registry — and wires up the right include paths, libraries, and link order automatically.
- **Caching by default.** File content hashing plus timestamps means unchanged files are never recompiled. Independent targets and sources compile in parallel.
- **Errors that try to help.** Validation errors point at the exact line and field that's wrong, with a hint where possible — not a wall of templated output.

## What works today

- TOML-based project manifests (`mokai.toml`) with targets, file groups, property groups, conditional sources/flags/defines, and hooks
- Local path and git-based dependency resolution, recursive across nested projects
- Automatic dependency graph construction with topological build ordering and cycle detection
- A glob engine supporting `*`, `**`, and brace expansion (`{a,b}`)
- Parallel, content-hash-cached compilation
- `compile_commands.json` generation for editor/IDE tooling (clangd, VS Code, etc.)
- Project scaffolding (`mokai create`) with interactive template, C++ standard, and git-init prompts
- Verified against real third-world projects, including a full build of **fmt** with conditional compiler-version-gated warning flags, and **SFML 3**, including its transitive dependencies (FreeType, HarfBuzz, SheenBidi, miniaudio, and several X11 extension libraries)

## What's in progress

- A central package registry, so common libraries can be added by name with no manual setup
- Smart version resolution (`sdl >= 2.7`) against upstream git tags
- A shared, machine-wide package cache to avoid duplicate clones across projects
- Visual Studio "Open Folder" / solution-adjacent tooling
- Expanded diagnostics (typo suggestions, richer source-span errors)

## Getting started

```bash
mokai create myapp
cd myapp
mokai build
./build/debug/myapp
```

`mokai create` will ask for a C++ standard, a starting template, and whether to initialize git — then scaffold a working project.

## Project layout

A minimal `mokai.toml`:

```toml
[project]
name = "myapp"
cpp_version = "c++20"

[target.myapp]
type = "executable"
sources = ["./main.cpp"]
```

A project with a local dependency:

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

This works against any dependency that exposes a matching target name or declares it in `[exports]` — for example, a real, tested build against [fmt](https://github.com/fmtlib/fmt):

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

For dependencies with multiple targets, depend on a specific one with `package:target`:

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

This is how Mokai built a real, working [SFML 3](https://github.com/SFML/SFML) target with its full transitive dependency chain — FreeType, HarfBuzz, SheenBidi, miniaudio, and the relevant X11 extension libraries — all expressed in SFML's own `mokai.toml`, invisible to the consuming project above.

See [`docs/`](./docs) for the full configuration reference, including file groups, property groups, conditional compilation, hooks, and exports.

## Contributing

Mokai is early and the design is still settling in places. Issues, ideas, and pull requests are welcome — especially real-world test cases (try building a library you use and open an issue with what broke).

## License

[Add your chosen license here]
