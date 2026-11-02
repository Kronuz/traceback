# Changelog

Notable changes to `traceback`, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no releases, so
entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#traceback) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-01

### Changed

- The strings pin moves from `485f882` to `5c98ec5` (`c2ab6dd`, `b607242`), over strings' fixes
  for small joins, replaces and case mappings, which give the same results.
- The errno-names pin moves from `73b7493` to `0ec0801` (`c2ab6dd`), which reads both
  `strerror_r` forms so `error::description()` is right on glibc. traceback calls
  `error::name`, which is unchanged.
- The term-color pin moves from `534e309` to `e453301` and the nanosleep pin from `256aaf6` to
  `4f62605` (`c2ab6dd`). term-color's `collapse()` and `color::ansi()` give the same bytes;
  nanosleep's range is documentation only.
- The README follows the family README standard (`bd06aa8`): a quick start before Installation
  with the output it prints, a FetchContent snippet pinned to a commit, and the build options
  under Installation. It no longer claims readiness for `std::stacktrace` (the code has no
  `__cpp_lib_stacktrace` seam) or that `format` and `describe` never throw (they allocate). The
  terminal screenshot, which repeated the quick start's output, is gone.
- AGENTS.md gives the commands that verify the library from a fresh clone: the two tests and the
  sanitizer jobs (`8388580`).

## 2026-10-31

### Changed

- The strings pin moves from `b42d3c4` to `485f882` (`d9c6f5b`), across strings' `indent` fix
  (`bb85615`): `indent` follows Python's `textwrap.indent` and keeps blank lines. With
  `TRACEBACK_HOOKS`, `dump_callstacks` passes each thread's traceback through `strings::indent`,
  which now keeps the traceback's leading newline, so a blank line follows each `<Thread ...>`
  header. A parser of crash dumps that expects the `== Traceback` line right after the header
  needs to skip it.
