# Contributing

## Code style

This project follows the [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines). Google and LLVM style guides are not used — both prohibit exceptions, which CommonLibSSE-NG relies on.

### Key rules

**Null checks** — use `if (!ptr)` / `if (ptr)`, never `== nullptr` or `!= nullptr` (ES.87).

**Memory** — no naked `new`/`delete` (R.11). No raw owning pointers (R.3). Use `std::unique_ptr` / `std::shared_ptr`.

**Control flow** — always use braces, even for single-statement bodies.

**Types and algorithms** — prefer `std::` types and algorithms over hand-rolled equivalents.

**Return values** — always check them. If a function returns `bool` or an error code, check it.

**Precompiled header** — `PCH.h` must be the first include in plugin-only `.cpp` files (`src/`). Sources compiled in both the plugin and native test targets (e.g. `SharedUtils.cpp`), and test-only sources (`tests/`), must not include `PCH.h`.

**Comments** — only where a non-obvious decision needs a reason. No filler, no redundant restatements of the code.
