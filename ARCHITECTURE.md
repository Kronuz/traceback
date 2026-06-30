# Architecture

`traceback` is one `.cc` over `traceback.h`, split into two layers that the
build separates cleanly.

## Two layers, one gate

The **formatting box** (`capture` / `describe` / `format` / `enable_atos`, plus
the raw `backtrace()` / `traceback()` helpers) is the default build. It captures
return addresses with `::backtrace`, symbolizes them with `dladdr` +
`abi::__cxa_demangle`, and optionally enriches them with `file:line` via a long-
lived macOS `atos` child. Nothing here is heavy: no static array, no signal
handler, no allocator override.

The **whole-process crash dumper** is everything else, and it all lives behind
the `TRACEBACK_HOOKS` build option: the thread registry, the SIGUSR2 stack-walk
handler, `dump_callstacks()` / `callstacks_snapshot()`, the
`register_thread` / `deregister_thread` / `thread_registration` API, and the
`__cxa_throw` + `malloc`/`free` interposition that records throw-site stacks. A
consumer that only wants readable tracebacks builds with the hooks off and pays
nothing for any of it. The matching declarations in `traceback.h` are guarded by
the same macro, so the symbols do not even exist in a default build.

`TRACEBACK_HOOKS` is off by default because the allocator interposition is a
landmine: it fights tcmalloc/jemalloc and sanitizers and leans on
libc++/libsupc++ ABI internals. It also has a sharp edge that the test works
around. Symbolizing a C++-mangled frame calls `abi::__cxa_demangle`, whose buffer
is then `free`d. With the hooks on, that `free` is the interposed one, but on some
platforms (macOS among them) the demangle buffer was allocated by a `malloc` that
bypassed the interposer, so freeing it corrupts the heap. The dumper's own
formatting runs on the dumping thread, so a host that calls `dump_callstacks()`
from one place is fine in practice, but it is a real reason to keep the hooks off
unless you need throw-site stacks.

## The thread registry

The registry is a fixed-size, lock-free array: `std::array<ThreadInfo,
TRACEBACK_MAX_THREADS>` (default 1024 slots). It has to be fixed and lock-free
because the SIGUSR2 handler reads it from interrupt context, where it cannot
allocate or take a lock. That rules out a `std::vector` or a mutex-guarded map.
The handler only ever reads slots and writes into the slot of the thread it
interrupted, both with plain atomics.

Each slot is a `ThreadInfo` holding the thread's name (by pointer, not copied),
its `pthread_t`, and two `std::array<std::atomic<void*>, TRACEBACK_MAX_FRAMES>`
callstacks: the live one the handler fills, and a baseline `snapshot`. At 128
frames each, a slot is roughly 2 KB, so the whole array is about 2 MB. That cost
is entirely inside the `TRACEBACK_HOOKS` gate. The second `snapshot` array is the
only part that exists purely for the active/idle diff: `callstacks_snapshot()`
records a baseline once the process has settled, and `dump_callstacks()` then
reports a thread as idle when its current stack matches the baseline and active
otherwise. It earns its keep by keeping the dump readable (idle threads are not
rendered), and it is gated like everything else, so a default build never
allocates it.

### Slot reclamation

A slot is free when its `pthread` is zero. `register_thread` first scans the
slots below the high-water mark and claims a free one with a CAS (so two
concurrent registrations cannot grab the same slot); only if none is free does it
extend the high-water mark with a `fetch_add`. `deregister_thread` clears the
calling thread's slot (frame counts first, then the `pthread` last) so it looks
empty to a concurrent dump before it is offered back. It never frees memory and
never shrinks the high-water mark, which keeps the handler's read side async-
signal-safe. The `thread_registration` RAII helper pairs the two: construct it at
the top of a thread function and the slot tracks the thread's lifetime.

Reclamation is what keeps the registry from leaking under thread churn. Without
it, a long-running process that starts and stops threads would march the high-
water mark up to the cap and then silently drop registrations. The dump/snapshot
wait loops count only live slots (nonzero `pthread`) so a reclaimed hole below the
high-water mark does not stall them waiting for an ack that never comes.

## Forward compatibility

The capture/symbolize paths are shaped so they can be swapped for C++23's
`std::stacktrace` behind the same API, guarded on `__cpp_lib_stacktrace`. As of
mid-2026 neither Apple clang's nor Homebrew LLVM's libc++ ships it, so the manual
`dladdr` path is what runs.
