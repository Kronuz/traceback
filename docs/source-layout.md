# Source layout

What each source file holds, and why the large ones stay whole. Splits follow cpp-libraries'
[`docs/SPLITTING.md`](https://github.com/Kronuz/cpp-libraries/blob/main/docs/SPLITTING.md).

## Library

- `traceback.h` (238 lines): the API, with the dumper's declarations under `TRACEBACK_HOOKS`.
- `traceback.cc` (1,054 lines): the whole implementation. It stays one translation unit.
  - The contract is one source file. Consumers that skip this CMake compile `traceback.cc` into
    their own target (Xapiand fetches the repository with `SOURCE_SUBDIR do-not-configure` and
    adds `${traceback_SOURCE_DIR}/traceback.cc` to its sources, setting the hook options
    itself). Splitting it would change the source list every such consumer compiles.
  - Its sections are already delimited and gated: the formatting box (`capture`, the macOS
    `atos` symbolizer, `describe`, `format`, about 190 lines), the raw layer (`backtrace`,
    `traceback`, `exception_callstack`, about 115 lines), the throw-site tracker under
    `TRACEBACK_THROW_HOOKS` (about 95 lines), and the crash dumper under `TRACEBACK_HOOKS` (the
    thread registry and the signal handler, dump, snapshot and registration, about 470 lines).
  - The sections share file-local internals: the `__has_include` feature block that defines
    `TRACEBACK_HAVE_EXECINFO` and `TRACEBACK_HAVE_DLADDR`, the frame cap `MAX_DUMP_FRAMES` in an
    anonymous namespace (used by `backtrace()` and the registry), and the `__cxa_exception`
    layout (read by `exception_callstack` and written by the `__cxa_throw` hook). Separate
    translation units would need an internal header and namespace changes for these.
  - The platform code does not form whole units: the macOS `atos` symbolizer is about 75 lines
    and belongs with `describe()`, its only caller, and the per-architecture register reads sit
    inside the one signal handler.

## Tests and examples

No other file is over 1,000 lines: the largest are `test/test.cc` (173), `test/hooks_test.cc`
(109) and `examples/demo.cc` (108).
