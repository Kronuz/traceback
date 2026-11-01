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
test/hooks_test.cc      The dumper and throw-site hooks (CTest: traceback_hooks).
examples/demo.cc        A tour (target traceback_demo).
docs/API.md             The API reference and the opt-ins.
```

## Build and test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
cmake -B build-hooks -DTRACEBACK_HOOKS=ON && cmake --build build-hooks && ctest --test-dir build-hooks
```

`TRACEBACK_HOOKS` builds the crash dumper; `TRACEBACK_THROW_HOOKS` adds the
throw-site tracker; `TRACEBACK_MAX_THREADS` (1024) and `TRACEBACK_MAX_FRAMES`
(128) size the registry. The default configuration, the one CI builds, also
compiles a second copy of the library with the hooks for `traceback_hooks`; the
`TRACEBACK_HOOKS=ON` build runs `traceback` itself against the dumper build.

## Verification and benchmarks

The method shared by the Kronuz libraries is in
[docs/PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md).

- **Tests.** The commands above run `traceback` (`test/test.cc`) and `traceback_hooks`
  (`test/hooks_test.cc`: the dumper over a registered worker thread and, on Linux and FreeBSD
  without a sanitizer, the throw-site tracker). A symbolized stack has no exact oracle, so the
  tests check its shape: frames captured, the header, the `test.cc` source location, the dump's
  thread list, and that a registry slot freed by one thread is reused by the next.
- **Sanitizers.** CI (`.github/workflows/ci.yml`, which calls the shared
  [cmake.yml](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml))
  runs both in an ASan+UBSan build and a TSan build (the registry and the SIGUSR2 walk are
  concurrent), with clang on Linux. In a build whose flags name a sanitizer, CMake compiles
  `traceback_hooks` without the throw hooks, since they replace `malloc` and `free`. The two
  jobs are:

  ```sh
  CC=clang CXX=clang++ cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
  CC=clang CXX=clang++ cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g -fsanitize=thread -fno-omit-frame-pointer" \
    -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
  cmake --build build-tsan && TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" ctest --test-dir build-tsan --output-on-failure
  ```

- **Not here.** The repository has no benchmark target, differential test, fuzz target or
  mutation script.

## Invariants

- **`format` and `describe` never throw** and always return text; error paths
  call them.
- **The signal handler does not allocate or lock.** The registry is a fixed array
  of slots claimed and released with atomics; the SIGUSR2 handler walks the stack
  in place and writes into the thread's own slot.
- **The default build does not touch the allocator.** Only
  `TRACEBACK_THROW_HOOKS` interposes `malloc`, `calloc`, `realloc`, `free`, and
  `__cxa_throw`. Each block carries a header whose last word holds the stashed
  callstack; `malloc`, `realloc`, `free`, and `__cxa_throw` must all use that slot.
- **Throw hooks need global interposition.** On Apple, system libraries keep the
  system allocator (two-level namespace), so their blocks reach the hooked `free`
  without a header and crash it; CMake refuses the option there. The test builds a
  second library (`traceback_hooks`) with both hooks, the throw half only on
  Linux/FreeBSD without sanitizers.
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
