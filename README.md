# traceback

A small, modern C++ crash-traceback toolkit, extracted and modernized from
[Xapiand](https://github.com/Kronuz/Xapiand).

It captures the current call stack and symbolizes it into a readable traceback:
demangled `symbol + offset` everywhere (via `dladdr` + `abi::__cxa_demangle`),
and `file:line` on macOS when you opt into `atos`. On top of that formatting box
it carries the original whole-process crash dumper: a registry of named threads,
a SIGUSR2 stack-walk handler, and `dump_callstacks()` / `callstacks_snapshot()`
that signal every registered thread and render all their stacks at once.

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

The formatting box:

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

The whole-process crash dumper:

```cpp
namespace traceback {
    void**      backtrace();                         // raw void** callstack (caller frees)
    void        register_thread(pthread_t, const char* name);
    void        collect_callstack_sig_handler(int, siginfo_t*, void* ptr);  // install on SIGUSR2
    std::string dump_callstacks();                   // broadcast + render all threads
    void        callstacks_snapshot();               // baseline so idle vs active can be told
    void**      exception_callstack(std::exception_ptr&);   // throw-site stack (needs TRACEBACK_HOOKS)
    std::string traceback(const char* fn, const char* file, int line, void** cs, int skip = 1);
    std::string traceback(const char* fn, const char* file, int line, int skip = 1);
}
// TRACEBACK() formats the current stack with a call-site header;
// BACKTRACE() yields a raw void** only when TRACEBACK_HOOKS is on, else nullptr.
```

### Using the dumper

The dumper needs a little host glue: install the signal handler, register the
threads you want to appear, take a baseline, then dump.

```cpp
struct sigaction sa{};
sa.sa_flags = SA_SIGINFO | SA_RESTART;
sa.sa_sigaction = traceback::collect_callstack_sig_handler;
sigaction(SIGUSR2, &sa, nullptr);                  // the dumper broadcasts SIGUSR2

traceback::register_thread(pthread_self(), "main");  // once per thread, after it starts
// ... worker threads call register_thread(pthread_self(), "...") too ...

traceback::callstacks_snapshot();                  // once the process has settled
std::string report = traceback::dump_callstacks(); // from a crash/debug path
```

The registry holds up to 1000 threads; names are stored by pointer (pass a
literal or otherwise long-lived string). `dump_callstacks()` tells idle threads
(unchanged since the snapshot) from active ones and colorizes the report.

### Throw-site capture (`TRACEBACK_HOOKS`, opt-in)

`exception_callstack()` recovers the stack from where an exception was *thrown*,
but that needs the `__cxa_throw` + global `malloc`/`free` interposition, which is
off by default. Turn it on with the CMake option `-DTRACEBACK_HOOKS=ON` (or
`-DTRACEBACK_HOOKS` on the compile line for a FetchContent-populate consumer).
It is an allocator-interposition landmine: it fights tcmalloc/jemalloc and
sanitizers and leans on libc++/libsupc++ ABI internals, which is why it is
opt-in. With it off, the all-thread snapshot and the per-thread formatter still
work; only `exception_callstack()` goes dark.

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
`-fno-omit-frame-pointer` for reliable frames in optimized builds (the signal
handler's in-place stack walk depends on a valid frame-pointer chain).

The dumper's colored, per-thread output pulls four Kronuz siblings via
`FetchContent` (tracked at tip): [`strings`](https://github.com/Kronuz/strings)
(`strings::format`/`indent`), [`errno-names`](https://github.com/Kronuz/errno-names)
(`error::name`), [`term-color`](https://github.com/Kronuz/term-color) (the color
palette), and [`nanosleep`](https://github.com/Kronuz/nanosleep). `likely.h` is
vendored in-repo.

## Examples

[`examples/demo.cc`](examples/demo.cc) is a runnable tour. A top-level CMake build
produces it next to the test:

```sh
cmake -B build && cmake --build build && ./build/traceback_demo
```

It walks a few nested, non-inlined calls (`outer` -> `middle` -> `inner`) and
prints the `format()` box for that live stack, most recent call first, with a
`source_location` header; the same `format()` with a plain header string; the raw
layer underneath (`capture()` return addresses paired with `describe()` of each,
plus a bogus address that comes back as `[unknown symbol]`); and the macOS `atos`
opt-in (`enable_atos(true)`), which enriches frames with `file:line` where it can
and is a no-op off macOS. Symbolization is best-effort: depending on the build,
some frames may resolve only to `symbol + offset`, and that is what the demo shows.

## License

MIT. Extracted from Xapiand (c) Dubalu LLC; modernized (c) Germán Méndez Bravo.
