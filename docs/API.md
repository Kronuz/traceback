# traceback API reference

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

The raw layer (always available, part of the toolkit but no heavy registry):

```cpp
namespace traceback {
    void**      backtrace();                         // raw void** callstack (caller frees)
    std::string traceback(const char* fn, const char* file, int line, void** cs, int skip = 1);
    std::string traceback(const char* fn, const char* file, int line, int skip = 1);
    void**      exception_callstack(std::exception_ptr&);   // throw-site stack (needs TRACEBACK_THROW_HOOKS)
}
// TRACEBACK() formats the current stack with a call-site header;
// BACKTRACE() yields a raw void** only when TRACEBACK_THROW_HOOKS is on, else nullptr.
```

The whole-process crash dumper (only compiled with `TRACEBACK_HOOKS`, see below):

```cpp
namespace traceback {
    void        register_thread(pthread_t, const char* name);
    void        deregister_thread();                 // release the calling thread's slot
    class       thread_registration { ... };         // RAII: register on ctor, deregister on dtor
    void        collect_callstack_sig_handler(int, siginfo_t*, void* ptr);  // install on SIGUSR2
    std::string dump_callstacks();                   // broadcast + render all threads
    void        callstacks_snapshot();               // baseline so idle vs active can be told
}
```

## Using the dumper (`TRACEBACK_HOOKS`)

The dumper is opt-in: build with `-DTRACEBACK_HOOKS=ON` (its symbols and the
multi-megabyte thread registry only exist then). It needs a little host glue:
install the signal handler, register the threads you want to appear, take a
baseline, then dump.

```cpp
struct sigaction sa{};
sa.sa_flags = SA_SIGINFO | SA_RESTART;
sa.sa_sigaction = traceback::collect_callstack_sig_handler;
sigaction(SIGUSR2, &sa, nullptr);                  // the dumper broadcasts SIGUSR2

// Declare an RAII guard at the top of each thread: it registers on entry and
// reclaims the slot on exit, so the registry does not leak under thread churn.
void* worker(void*) {
    traceback::thread_registration reg("worker");
    // ... thread body ...
}
traceback::thread_registration main_reg("main");   // for the main thread too

traceback::callstacks_snapshot();                  // once the process has settled
std::string report = traceback::dump_callstacks(); // from a crash/debug path
```

The registry is a fixed, lock-free array (so the SIGUSR2 handler can read it
without allocating or locking). It holds up to `TRACEBACK_MAX_THREADS` threads
(default 1024, overridable); each slot stores `TRACEBACK_MAX_FRAMES` frames
(default 128). Names are stored by pointer (pass a literal or otherwise
long-lived string). A thread that exits should free its slot via
`deregister_thread()` (the `thread_registration` RAII guard does this for you);
freed slots are reused by later registrations, so a long-running process with
thread churn does not leak slots up to the cap. `dump_callstacks()` tells idle
threads (unchanged since the snapshot) from active ones and colorizes the report.

## Two opt-ins: the dumper and the throw-site tracker

The heavy parts live behind two independent CMake options, both off by default:

- **`TRACEBACK_HOOKS`** — the crash dumper: the fixed multi-megabyte thread
  registry, the SIGUSR2 stack-walk handler, and the all-thread
  `dump_callstacks()`/`callstacks_snapshot()`. It does **not** touch the process
  allocator, so `describe()`/`format()` stay safe.

- **`TRACEBACK_THROW_HOOKS`** — the throw-site tracker: the `__cxa_throw` +
  global `malloc`/`calloc`/`realloc`/`free` interposition that lets
  `exception_callstack()` recover the stack from where an exception was *thrown*.
  Linux and FreeBSD only: CMake refuses it on Apple platforms.

The default build carries neither: it is just the formatting box plus
`backtrace()`/`traceback()`, so a consumer that only wants readable tracebacks
pays nothing for the registry. The two options compose — `-DTRACEBACK_HOOKS=ON`
for the dumper, add `-DTRACEBACK_THROW_HOOKS=ON` for throw-site stacks on top (or
the same `-D`s on the compile line for a FetchContent-populate consumer). Tune
the registry footprint with `-DTRACEBACK_MAX_THREADS` / `-DTRACEBACK_MAX_FRAMES`.

The throw-site tracker is split out from the dumper precisely because the
allocator interposition is a landmine: it fights tcmalloc/jemalloc and sanitizers
and leans on libc++/libsupc++ ABI internals. The sharp edge: with it on,
symbolizing a C++-mangled frame goes through `abi::__cxa_demangle`, whose buffer
is then `free`d through the override; on some platforms (macOS among them) that
allocation does not route through the interposed `malloc`, so freeing it can
corrupt the heap. Taking just `TRACEBACK_HOOKS` (the dumper) leaves the allocator
untouched and sidesteps that entirely — enable `TRACEBACK_THROW_HOOKS` only when
you specifically need throw-site stacks.

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
