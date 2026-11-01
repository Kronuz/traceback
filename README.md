# traceback

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/traceback/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/traceback/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

Stack traces for C++20: demangled call stacks, optional macOS file:line, and an opt-in
all-thread crash dumper. `format()` walks the live stack and resolves each return address
with `dladdr` and the C++ demangler; on macOS, a long-lived `atos` child adds source
locations. Reach for it to attach a stack to error reports and assertion failures, or to
see where every thread of a hung server is stuck.

## ✨ Highlights

- **One call:** `traceback::format()` returns the current stack, most recent call first,
  as demangled `symbol + offset` lines under an optional header.
- **A raw layer:** `capture()` returns the return addresses and `describe()` resolves
  one; a frame that does not resolve reads `[unknown symbol]`.
- **Source locations on macOS:** `enable_atos(true)` keeps one `atos` process alive and
  adds `file:line`; it is off by default and a no-op elsewhere.
- **Opt-in crash dumper** (`TRACEBACK_HOOKS`): a fixed, lock-free thread registry and a
  SIGUSR2 handler that walks each thread's stack, so `dump_callstacks()` reports every
  registered thread and tells idle ones from active ones.
- **Opt-in throw-site stacks** (`TRACEBACK_THROW_HOOKS`, Linux and FreeBSD):
  `exception_callstack()` recovers where an in-flight exception was thrown.

## 🚀 Quick start

```cpp
#include "traceback.h"

#include <cstdio>
#include <source_location>
#include <string>

int main() {
	std::string tb = traceback::format(std::source_location::current());
	std::fputs(tb.c_str(), stderr);
}
```

```text
== Traceback (most recent call first): example.cc:8 at int main():
      1 0x0000000102b70e80 main + 48
      0 0x00000001858dd4e4 start + 6992
```

[`examples/demo.cc`](examples/demo.cc) walks nested calls, the raw `capture()` and
`describe()` layer, and the `atos` opt-in.

## 📦 Installation

Requires C++20 and CMake 3.16 or newer. CMake fetches
[strings](https://github.com/Kronuz/strings),
[errno-names](https://github.com/Kronuz/errno-names),
[term-color](https://github.com/Kronuz/term-color) and
[nanosleep](https://github.com/Kronuz/nanosleep), and links `dl`, `util` and `execinfo`
where the platform has them. CI builds and tests it on Linux (x86_64 with GCC and Clang,
arm64), macOS (arm64, x86_64) and FreeBSD, plus ASan, UBSan and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(traceback
  GIT_REPOSITORY https://github.com/Kronuz/traceback.git
  GIT_TAG        d9c6f5b270894a3a2d8b21b9e9a6f788ed1a1baa)  # or main, to track the tip
FetchContent_MakeAvailable(traceback)

target_link_libraries(my_app PRIVATE traceback::traceback)
```

`traceback::traceback` is a compiled library (`traceback.cc`). Include `traceback.h`. Set
`-DTRACEBACK_HOOKS=ON` for the crash dumper and `-DTRACEBACK_THROW_HOOKS=ON` for
throw-site stacks; build with `-fno-omit-frame-pointer` for reliable frames in optimized
code.

## 🎯 When to use it

- A stack in error reports, assertion failures and crash handlers.
- Finding where every thread is stuck in a hung server (`TRACEBACK_HOOKS`).

Not for:

- Throw-site stacks on macOS, or next to tcmalloc, jemalloc or sanitizers:
  `TRACEBACK_THROW_HOOKS` interposes `malloc` and `free`, which needs global symbol
  interposition and conflicts with other allocators. CMake refuses it on Apple platforms.
- `file:line` off macOS: other platforms get `symbol + offset` only.
- Async-signal-safe use of `format()`: it allocates. The dumper's SIGUSR2 handler is the
  signal-side path.

## 📚 Documentation

- [docs/API.md](docs/API.md): the formatting API, the raw layer, the crash dumper and its
  host glue, the two opt-ins and `atos`.
- [ARCHITECTURE.md](ARCHITECTURE.md): capture, symbolization, the registry and the
  signal-handler walk.
- [AGENTS.md](AGENTS.md): repository map, build options and traps.
- [Catalog entry](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#traceback)
  in Kronuz C++ Libraries.

## 🛠️ Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`./build/traceback_demo` runs the tour. Configure a second build with
`-DTRACEBACK_HOOKS=ON` to build and test the crash dumper.

## 🤝 Contributing

Issues and pull requests are welcome; read [AGENTS.md](AGENTS.md) and the catalog's
[rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first.

## 📄 License

MIT, see [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
