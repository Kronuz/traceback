/*
 * Copyright (c) 2015-2026 Germán Méndez Bravo (Kronuz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// A small, modern C++ crash-traceback toolkit, extracted and modernized from
// Xapiand.
//
// Two layers live here:
//
//   * The formatting box. It captures the current call stack and symbolizes it
//     (demangled symbol + offset via dladdr/abi, plus file:line via `atos` on
//     macOS), and formats a human-readable traceback: capture(), describe(),
//     format().
//
//   * The whole-process crash dumper. A registry of named threads, a SIGUSR2
//     handler that walks each thread's stack in place (arch-specific), a
//     __cxa_throw + global-allocator interposition that records throw-site
//     callstacks, and dump_callstacks() / callstacks_snapshot(), which broadcast
//     the signal and render every thread's stack at once. The host registers its
//     threads (register_thread), installs the signal handler on SIGUSR2, and
//     calls the dump entry points from a crash or debug path.
//
// Forward-compatible with C++23 <stacktrace>: when libc++/libstdc++ ship it, the
// capture/symbolize paths below can be swapped for std::stacktrace behind the
// same API (the `#if __cpp_lib_stacktrace` guard marks the seam). As of mid-2026
// neither Apple clang's nor Homebrew LLVM's libc++ ships it, so the manual path
// is what runs.

#pragma once

// The library owns its build options, split into two independent opt-ins:
//
//   * TRACEBACK_HOOKS gates the crash dumper: the fixed, multi-megabyte,
//     lock-free thread registry, the SIGUSR2 stack-walk handler, and the
//     all-thread dump_callstacks()/snapshot(). No allocator override here, so the
//     process allocator is untouched and describe()/format() stay safe.
//
//   * TRACEBACK_THROW_HOOKS gates the throw-site tracker: a __cxa_throw +
//     malloc/calloc/realloc/free interposition that stashes the throw-site
//     callstack on every thrown exception (read back via exception_callstack()).
//     This is an allocator-interposition landmine — it fights tcmalloc/jemalloc
//     and sanitizers and routes every free (including foreign buffers like
//     abi::__cxa_demangle's result) through the override — so it is deliberately
//     separate: a host can take the dumper without paying for it.
//
// Both default OFF. The DEFAULT build is just the lightweight formatting box
// (capture/describe/format/enable_atos) plus backtrace()/traceback(); no static
// array, no signal handler, no allocator override is compiled in. They compose:
// -DTRACEBACK_HOOKS=ON for the dumper, add -DTRACEBACK_THROW_HOOKS=ON for
// throw-site stacks on top.
//
// TRACEBACK_MAX_THREADS (default 1024) sizes the registry array, and
// TRACEBACK_MAX_FRAMES (default 128) sizes each slot's per-thread callstack;
// both only matter under TRACEBACK_HOOKS and are overridable.
//
// CMake generates traceback_config.h via configure_file from the CMake cache
// variables; a FetchContent-populate consumer that never runs this CMake can
// instead -D these on its compile line, so the include is optional.
#if defined(__has_include)
#  if __has_include("traceback_config.h")
#    include "traceback_config.h"   // for TRACEBACK_HOOKS (generated)
#  endif
#endif

#include <cstddef>            // for std::size_t
#include <exception>         // for std::exception_ptr
#include <source_location>   // for std::source_location
#include <string>            // for std::string
#include <string_view>       // for std::string_view
#include <vector>            // for std::vector

#include <pthread.h>         // for pthread_t
#include <signal.h>          // for siginfo_t

namespace traceback {

// ---- The formatting box --------------------------------------------------

// Capture up to `max` instruction (return) addresses of the current call stack,
// skipping the innermost `skip` frames (skip >= 1 drops capture() itself).
std::vector<void*> capture(std::size_t skip = 1, std::size_t max = 128);

// Best-effort symbolization of one instruction address: a demangled
// "symbol + offset", enriched to "file:line" where the platform can (macOS
// `atos`). Returns "[unknown symbol]" when nothing resolves.
std::string describe(const void* address);

// A formatted, multi-line traceback of the current call stack (most recent call
// first), with an optional header line. `skip` drops innermost frames so the
// logging frames do not show up.
std::string format(std::string_view header = {}, std::size_t skip = 2, std::size_t max = 128);

// Convenience: format with a header built from a std::source_location.
std::string format(const std::source_location& loc, std::size_t skip = 2, std::size_t max = 128);

// Opt into macOS `atos` symbolization, which enriches `describe`/`format` with
// file:line at the cost of forking an `atos` subprocess (kept alive across calls).
// Off by default; a no-op (stays false) on non-Apple platforms. A host that wants
// the original Xapiand file:line tracebacks calls enable_atos(true) at startup.
void enable_atos(bool on) noexcept;
bool atos_enabled() noexcept;


// ---- The whole-process crash dumper --------------------------------------

// Capture the current call stack as a raw, malloc'd `void**` block: callstack[0]
// points one-past the last frame (a sentinel/length marker), callstack[1..N] are
// the return addresses. The caller owns the block and must free() it. This is the
// low-level form the dumper and the __cxa_throw hook share; most callers want
// capture()/format() instead.
void** backtrace();

// The thread registry, the SIGUSR2 handler, and the all-thread dump are the
// heavy part of the dumper: the registry is a fixed, lock-free, multi-megabyte
// static array (so the handler can read it without allocating or locking), and
// it is only compiled when the library is built with TRACEBACK_HOOKS. The
// declarations below therefore exist only under that macro; a consumer that
// wants the dumper builds with -DTRACEBACK_HOOKS=ON (CMake option) or
// -DTRACEBACK_HOOKS on the compile line. The formatting box above, plus
// backtrace()/traceback() below, are always available.
#if defined(TRACEBACK_HOOKS)

// How many threads the registry can hold (the fixed array's size) and how many
// frames each slot stores. Overridable via -DTRACEBACK_MAX_THREADS /
// -DTRACEBACK_MAX_FRAMES or the generated traceback_config.h.
#if !defined(TRACEBACK_MAX_THREADS)
#  define TRACEBACK_MAX_THREADS 1024
#endif
#if !defined(TRACEBACK_MAX_FRAMES)
#  define TRACEBACK_MAX_FRAMES 128
#endif

// Register the calling (or any) thread in the dumper's fixed-size registry under
// a human-readable `name`. A registered thread is one that dump_callstacks() and
// callstacks_snapshot() will signal and render. The host calls this once per
// thread it wants to appear in a dump (typically right after the thread starts),
// passing pthread_self(). Names are stored by pointer, not copied, so pass a
// pointer with static/long-enough lifetime (e.g. a string literal). The registry
// holds up to TRACEBACK_MAX_THREADS threads; registrations beyond that are
// silently dropped. Slots freed by deregister_thread() are reused first, so a
// process with thread churn does not leak slots up to the cap.
void register_thread(pthread_t pthread, const char* name);

// Release the *calling* thread's registry slot so it can be reused by a later
// register_thread(). Call it from a thread that is about to exit (or use the
// RAII thread_registration helper, which does this for you). It only clears the
// slot's atomics, so it is safe to call concurrently with a dump.
void deregister_thread();

// RAII wrapper: register the calling thread on construction, deregister it on
// destruction. Declare one at the top of each thread function so registration
// tracks the thread's lifetime:
//
//   void* worker(void*) {
//     traceback::thread_registration reg("worker");
//     ...
//   }
class thread_registration {
public:
	explicit thread_registration(const char* name);
	~thread_registration();
	thread_registration(const thread_registration&) = delete;
	thread_registration& operator=(const thread_registration&) = delete;
};

// SIGUSR2 handler that walks the interrupted thread's stack in place and stores
// it into that thread's registry slot. The host installs this on SIGUSR2 with
// SA_SIGINFO (the third argument carries the ucontext_t the walk needs):
//
//   struct sigaction sa{};
//   sa.sa_flags = SA_SIGINFO | SA_RESTART;
//   sa.sa_sigaction = traceback::collect_callstack_sig_handler;
//   sigaction(SIGUSR2, &sa, nullptr);
//
// dump_callstacks()/callstacks_snapshot() broadcast SIGUSR2 to drive it.
void collect_callstack_sig_handler(int signum, siginfo_t* info, void* ptr);

// Broadcast SIGUSR2 to every registered thread, wait briefly for each to record
// its stack, and return a formatted, colored report of all threads (which are
// active vs idle, with each active thread's traceback). Call from a crash or
// debug path. Requires the host to have installed collect_callstack_sig_handler
// on SIGUSR2 and registered its threads.
std::string dump_callstacks();

// Take a stable baseline snapshot of every registered thread's callstack, used by
// dump_callstacks() to tell idle threads (unchanged since the snapshot) from
// active ones. Call once the process has settled, before the first dump.
void callstacks_snapshot();

#endif  // TRACEBACK_HOOKS

// Recover the throw-site callstack stashed for an in-flight exception by the
// __cxa_throw hook. Only meaningful when the library was built with
// TRACEBACK_THROW_HOOKS; otherwise the returned block is unspecified. The
// returned `void**` is owned by the exception object, do not free it.
void** exception_callstack(std::exception_ptr& eptr);

// Format a raw `void**` callstack (as produced by backtrace()) into a multi-line
// traceback, with an optional `function`/`filename`/`line` header. `skip` drops
// innermost frames. Symbolization routes through describe(), so it honors
// enable_atos(). The no-callstack overload captures the current stack itself.
std::string traceback(const char* function, const char* filename, int line, void** callstack, int skip = 1);
std::string traceback(const char* function, const char* filename, int line, int skip = 1);

}  // namespace traceback


// Macros adapted from Xapiand. They live at global scope (where Xapiand's did)
// but call into namespace traceback. TRACEBACK() formats the current stack with
// a call-site header; BACKTRACE() yields a raw `void**` only when the throw-site
// hooks are compiled in, else nullptr (so callers can pass it unconditionally).
#define TRACEBACK() ::traceback::traceback(__func__, __FILE__, __LINE__)

#ifdef TRACEBACK_THROW_HOOKS
#define BACKTRACE() ::traceback::backtrace()
#else
#define BACKTRACE() nullptr
#endif
