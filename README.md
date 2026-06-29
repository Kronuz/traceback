# traceback

A small, modern C++ stack-trace box, extracted and modernized from
[Xapiand](https://github.com/Kronuz/Xapiand).

It captures the current call stack and symbolizes it into a readable traceback:
demangled `symbol + offset` everywhere (via `dladdr` + `abi::__cxa_demangle`),
and `file:line` on macOS when you opt into `atos`.

```cpp
#include "traceback.h"

std::string tb = traceback::format();          // current stack, formatted
std::string tb2 = traceback::format(std::source_location::current());  // with a header

// == Traceback (most recent call first): file.cc:42 at foo():
//       2 0x0000000104298... demo::inner() + 56
//       1 0x0000000104298... main + 36
//       0 0x000000018861f... start + 6992
```

## API

```cpp
namespace traceback {
    std::vector<void*> capture(size_t skip = 1, size_t max = 128);
    std::string        describe(const void* address);
    std::string        format(std::string_view header = {}, size_t skip = 2, size_t max = 128);
    std::string        format(const std::source_location& loc, size_t skip = 2, size_t max = 128);
    void               enable_atos(bool on) noexcept;   // macOS file:line, opt-in
    bool               atos_enabled() noexcept;
}
```

`describe`/`format` never throw and always return something (`[unknown symbol]`
for frames that don't resolve), so they are safe to call from error paths.

## What it deliberately leaves out

This is the reusable core. Two things from the original Xapiand `traceback.cc`
are intentionally **not** here, because they don't belong in a general library:

- **Throw-site stack capture** (`exception_callstack`), which relied on
  interposing `__cxa_throw` and overriding global `malloc`/`free`. That is an
  allocator-interposition landmine: it fights tcmalloc/jemalloc and sanitizers
  and depends on ABI internals. If you want a throw-site stack, capture one in a
  custom exception's constructor instead. `format()` gives the catch-site stack,
  which is what you usually want anyway.
- **The signal-time, all-thread crash dumper** + thread registry. That is
  crash-diagnostics infrastructure, a separate concern from "format a backtrace,"
  and the only part that needed an atomic shared pointer.

## Forward compatibility with `std::stacktrace`

C++23's `<stacktrace>` is the right long-term home for this. As of mid-2026
neither Apple clang's nor Homebrew LLVM's libc++ ships it, so the manual path
runs today. The API is shaped so the internals can be swapped for
`std::stacktrace` (guarded on `__cpp_lib_stacktrace`) with no change to callers.

## macOS `atos`

`enable_atos(true)` keeps a single `atos` child process alive in a pty and uses
it to resolve `file:line`. It is off by default because forking a subprocess per
process is heavy; turn it on if you want source locations and can afford it. It
is a no-op off macOS.

## Build

One `.cc` over `traceback.h`. Build standalone or via `FetchContent`:

```cmake
FetchContent_Declare(traceback GIT_REPOSITORY https://github.com/Kronuz/traceback.git GIT_TAG <sha>)
FetchContent_MakeAvailable(traceback)
target_link_libraries(your_target PRIVATE traceback::traceback)
```

Requires C++20 (for `std::source_location` and `std::format`). Build it
`-fno-omit-frame-pointer` for reliable frames in optimized builds.

## License

MIT. Extracted from Xapiand (c) Dubalu LLC; modernized (c) Germán Méndez Bravo.
