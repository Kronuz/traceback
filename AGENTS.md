# AGENTS.md — traceback

Working notes for anyone changing this repository. [README.md](README.md) is for
users; [ARCHITECTURE.md](ARCHITECTURE.md) explains the design.

## Repo map

```
traceback.h             The API: format/describe/capture, the raw layer, the dumper.
traceback.cc            The one translation unit.
traceback_config.h.in   CMake template: TRACEBACK_HOOKS, TRACEBACK_THROW_HOOKS, registry sizes.
likely.h                Vendored, self-contained branch hints.
test/test.cc            Formatting, describe, capture (CTest: traceback).
examples/demo.cc        A tour (target traceback_demo).
docs/API.md             The API reference and the opt-ins.
docs/traceback.svg      The demo's output, rendered for the README.
```

## Build and test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
cmake -B build-hooks -DTRACEBACK_HOOKS=ON && cmake --build build-hooks && ctest --test-dir build-hooks
```

`TRACEBACK_HOOKS` builds the crash dumper; `TRACEBACK_THROW_HOOKS` adds the
throw-site tracker; `TRACEBACK_MAX_THREADS` (1024) and `TRACEBACK_MAX_FRAMES`
(128) size the registry. CI builds only the default configuration, so build and
test with `TRACEBACK_HOOKS=ON` by hand when touching the dumper.

## Invariants

- **`format` and `describe` never throw** and always return text; error paths
  call them.
- **The signal handler does not allocate or lock.** The registry is a fixed array
  of slots claimed and released with atomics; the SIGUSR2 handler walks the stack
  in place and writes into the thread's own slot.
- **The default build does not touch the allocator.** Only
  `TRACEBACK_THROW_HOOKS` interposes `malloc`, `calloc`, `realloc`, `free`, and
  `__cxa_throw`.
- **Thread names are stored by pointer.** Callers pass long-lived strings.

## Traps

- **Throw-site hooks and `__cxa_demangle`.** With `TRACEBACK_THROW_HOOKS`, a
  demangle buffer allocated outside the interposed `malloc` (macOS among others)
  can be freed through the override and corrupt the heap. Keep the two options
  independent.
- **Frame pointers.** Optimized builds without `-fno-omit-frame-pointer` lose
  frames, and the in-place walk depends on the frame-pointer chain.
- **`atos` is a child process.** `enable_atos(true)` keeps one alive in a pty;
  never enable it on a path that must not fork.
