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

#include "traceback.h"

#include <array>             // for std::array
#include <atomic>            // for std::atomic, std::atomic_size_t
#include <cassert>           // for assert
#include <chrono>            // for std::chrono (snapshot timing)
#include <cstdint>           // for uintptr_t
#include <cstdio>            // for std::snprintf, std::fprintf
#include <cstdlib>           // for std::free, std::malloc
#include <cstring>           // for std::memset, std::memcpy
#include <exception>         // for std::exception_ptr
#include <format>            // for std::format
#include <sched.h>           // for sched_yield
#include <unwind.h>          // for _Unwind_Exception (throw-site hook)
#if defined(__FreeBSD__)
#include <ucontext.h>        // for ucontext_t
#else
#include <sys/ucontext.h>    // for ucontext_t
#endif

#include "error.hh"          // for error::name
#include "likely.h"          // for likely/unlikely
#include "nanosleep.h"       // for nanosleep
#include "strings.hh"        // for strings::format, strings::indent

// Color palette (term-color). Xapiand pulled these from its colors.h, which is
// term-color's palette. DEBUG_COL is Xapiand-local (#define DEBUG_COL
// rgb(105, 105, 105) in its log.h); term-color spells the same rgb as DIM_GRAY,
// so we alias it here.
#include "colors.h"         // for STEEL_BLUE, DARK_STEEL_BLUE, DARK_ORANGE, RED, ...
#define DEBUG_COL DIM_GRAY

#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>    // for ::backtrace
#    define TRACEBACK_HAVE_EXECINFO 1
#  endif
#  if __has_include(<dlfcn.h>)
#    include <dlfcn.h>       // for dladdr, dlsym
#    define TRACEBACK_HAVE_DLADDR 1
#  endif
#  if __has_include(<cxxabi.h>)
#    include <cxxabi.h>      // for abi::__cxa_demangle
#    define TRACEBACK_HAVE_CXA 1
#  endif
#endif

namespace traceback {

namespace {

constexpr std::size_t MAX_FRAMES = 256;

// The dumper's registry stores fixed-size per-thread callstacks; this caps how
// many frames each slot holds. Matches Xapiand's `max_frames`.
constexpr std::size_t MAX_DUMP_FRAMES = 128;

}  // namespace


std::vector<void*>
capture(std::size_t skip, std::size_t max)
{
#if defined(TRACEBACK_HAVE_EXECINFO)
	void* buf[MAX_FRAMES];
	if (max > MAX_FRAMES) {
		max = MAX_FRAMES;
	}
	int n = ::backtrace(buf, static_cast<int>(max));
	std::vector<void*> out;
	if (n <= 0) {
		return out;
	}
	std::size_t frames = static_cast<std::size_t>(n);
	for (std::size_t i = skip; i < frames; ++i) {
		out.push_back(buf[i]);
	}
	return out;
#else
	(void)skip;
	(void)max;
	return {};
#endif
}


// ---- macOS `atos` enrichment (opt-in: gives file:line) -------------------
//
// Ported from Xapiand. Keeps a single `atos` child alive in a pty so the symbol
// table is parsed once. Off by default: forking a subprocess per process is
// heavy, and std::stacktrace will supersede it. enable_atos(true) restores the
// original file:line behavior on macOS.

static std::atomic<bool> g_use_atos{false};

void enable_atos(bool on) noexcept { g_use_atos.store(on, std::memory_order_relaxed); }
bool atos_enabled() noexcept { return g_use_atos.load(std::memory_order_relaxed); }

#if defined(__APPLE__)
}  // namespace traceback

#include <mutex>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>           // for forkpty

namespace traceback {
static std::string
atos(const void* address)
{
	static std::mutex mtx;
	std::lock_guard<std::mutex> lk(mtx);

	char tmp[32];
	static int fd = -1;
	if (fd == -1) {
		Dl_info info;
		std::memset(&info, 0, sizeof(Dl_info));
		if (dladdr(reinterpret_cast<const void*>(&atos), &info) == 0) {
			return "";
		}
		struct termios term_opts;
		cfmakeraw(&term_opts);
		pid_t childpid = forkpty(&fd, nullptr, &term_opts, nullptr);
		if (childpid < 0) {
			return "";
		}
		if (childpid == 0) {
			std::snprintf(tmp, sizeof(tmp), "%p", static_cast<const void*>(info.dli_fbase));
			execlp("/usr/bin/atos", "atos", "-o", info.dli_fname, "-l", tmp, nullptr);
			std::_Exit(1);
		}
		int size = std::snprintf(tmp, sizeof(tmp), "%p\n", address);
		(void)::write(fd, tmp, static_cast<size_t>(size));
		struct pollfd fds;
		fds.fd = fd;
		fds.events = POLLIN;
		(void)::poll(&fds, 1, 3000);
	} else {
		int size = std::snprintf(tmp, sizeof(tmp), "%p\n", address);
		(void)::write(fd, tmp, static_cast<size_t>(size));
	}

	for (int t = 0; t <= 10; ++t) {
		constexpr unsigned MINLINE = 3;
		constexpr unsigned MAXLINE = 4096;
		char line[MAXLINE];
		size_t nread = 0;
		char c = '\0';
		while (c != '\n' && nread < MAXLINE) {
			if (::read(fd, &c, 1) <= 0) {
				::close(fd);
				fd = -1;
				return "";
			}
			if (c != '\n') {
				line[nread++] = c;
			}
		}
		if (nread > MAXLINE - 4 || nread < MINLINE) {
			return "";
		}
		if (t != 10 && line[nread - 3] == ':' && line[nread - 2] == '0' && line[nread - 1] == ')') {
			address = static_cast<const char*>(address) - 1;
			int size = std::snprintf(tmp, sizeof(tmp), "%p\n", address);
			(void)::write(fd, tmp, static_cast<size_t>(size));
		} else {
			return std::string(line, nread);
		}
	}
	return "";
}
#else
static std::string atos(const void*) { return ""; }
#endif


std::string
describe(const void* address)
{
	if (atos_enabled()) {
		std::string s = atos(address);
		// atos returns "0x..." style only when it failed to symbolize; otherwise
		// it is a real "symbol (in image) (file:line)" string.
		if (s.size() > 2 && s.compare(0, 2, "0x") != 0) {
			return s;
		}
	}

#if defined(TRACEBACK_HAVE_DLADDR)
	Dl_info info;
	std::memset(&info, 0, sizeof(Dl_info));
	if (dladdr(address, &info) != 0 && info.dli_sname != nullptr) {
		std::string sym;
#  if defined(TRACEBACK_HAVE_CXA)
		int status = 0;
		char* unmangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
		if (status == 0 && unmangled != nullptr) {
			sym = unmangled;
			std::free(unmangled);
		} else {
			sym = info.dli_sname;
			std::free(unmangled);
		}
#  else
		sym = info.dli_sname;
#  endif
		auto offset = static_cast<const char*>(address) - static_cast<const char*>(info.dli_saddr);
		return std::format("{} + {}", sym, offset);
	}
#endif
	return "[unknown symbol]";
}


std::string
format(std::string_view header, std::size_t skip, std::size_t max)
{
	auto frames = capture(skip, max);

	std::string tb = "\n== Traceback (most recent call first)";
	if (!header.empty()) {
		tb.append(": ");
		tb.append(header);
	}
	if (frames.empty()) {
		tb.append(":\n    <empty traceback>");
		return tb;
	}
	tb.push_back(':');
	for (std::size_t i = 0; i < frames.size(); ++i) {
		tb.append(std::format("\n    {:3} {:#018x} {}", frames.size() - i - 1,
			reinterpret_cast<std::uintptr_t>(frames[i]), describe(frames[i])));
	}
	return tb;
}


std::string
format(const std::source_location& loc, std::size_t skip, std::size_t max)
{
	// Skip one extra frame so this overload itself does not appear.
	return format(std::format("{}:{} at {}", loc.file_name(), loc.line(), loc.function_name()),
		skip + 1, max);
}


// ==========================================================================
// The whole-process crash dumper.
//
// Ported from Xapiand's traceback.cc. A fixed registry of named threads; a
// SIGUSR2 handler that walks the interrupted thread's stack in place and stores
// it into that thread's slot; dump_callstacks()/callstacks_snapshot(), which
// broadcast the signal and render every thread's stack; an optional __cxa_throw
// + malloc/free interposition (gated on TRACEBACK_HOOKS) that records throw-site
// callstacks. Symbol formatting routes through describe() above, not a second
// copy of the dladdr/atos logic.
// ==========================================================================

namespace {

std::atomic_size_t pthreads_req;
std::atomic_size_t pthreads_cnt;


// A captured callstack stored as a malloc'd void** block: slot 0 holds a
// one-past-the-end sentinel (so size() is end - base), slots 1..N hold the
// return addresses. backtrace() and the __cxa_throw hook both produce this shape.
class Callstack {
	void** callstack;

public:
	Callstack(void** callstack) : callstack(callstack) {}

	Callstack(const Callstack& other) {
		callstack = static_cast<void**>(std::malloc((other.size() + 1) * sizeof(void*)));
		std::memcpy(callstack + 1, other.callstack + 1, other.size() * sizeof(void*));
		callstack[0] = &callstack[other.size()];
	}

	~Callstack() {
		if (callstack) {
			std::free(callstack);
		}
	}

	std::size_t size() const {
		return callstack ? static_cast<void**>(*callstack) - callstack : 0;
	}

	bool empty() const {
		return callstack ? static_cast<void**>(*callstack) == callstack : true;
	}

	void** get() const {
		return callstack;
	}

	void** release() {
		auto ret = callstack;
		callstack = nullptr;
		return ret;
	}

	void* operator[](std::size_t idx) const {
		return idx < size() ? callstack[idx + 1] : nullptr;
	}

	bool operator==(const Callstack& other) const {
		if (size() != other.size()) {
			return false;
		}
		for (std::size_t idx = 0; idx < size(); ++idx) {
			if (callstack[idx + 1] != other.callstack[idx + 1]) {
				return false;
			}
		}
		return true;
	}

	bool operator!=(const Callstack& other) const {
		return !operator==(other);
	}

	std::string __repr__() const {
		std::string rep;
		rep.push_back('{');
		for (std::size_t idx = 0; idx < size(); ++idx) {
			rep.push_back(' ');
			rep.append(strings::format("{}", callstack[idx + 1]));
		}
		rep.append(" }");
		return rep;
	}
};


// One registry slot. The signal handler writes callstack/callstack_frames from
// interrupt context (lock-free atomics); callstacks_snapshot() maintains the
// baseline snapshot/snapshot_frames; req acknowledges a broadcast round.
struct ThreadInfo {
	const char* name;
	std::atomic<pthread_t> pthread;

	std::atomic_size_t callstack_frames;
	std::array<std::atomic<void*>, MAX_DUMP_FRAMES> callstack;

	std::atomic_size_t snapshot_frames;
	std::array<std::atomic<void*>, MAX_DUMP_FRAMES> snapshot;

	std::atomic_int errnum;

	std::atomic_size_t req;
};


std::array<ThreadInfo, 1000> pthreads;

}  // namespace


void**
backtrace()
{
#if defined(TRACEBACK_HAVE_EXECINFO)
	void* tmp[MAX_DUMP_FRAMES];
	auto frames = ::backtrace(tmp, MAX_DUMP_FRAMES);
	void** callstack = static_cast<void**>(std::malloc((frames + 1) * sizeof(void*)));
	if (callstack != nullptr) {
		std::memcpy(callstack + 1, tmp + 1, frames * sizeof(void*));
		callstack[0] = &callstack[frames];
	}
	return callstack;
#else
	return nullptr;
#endif
}


std::string
traceback(const char* function, const char* filename, int line, void** callstack, int skip)
{
	std::string tb = "\n== Traceback (most recent call first): ";
	if (filename && *filename) {
		tb.append(filename);
	}
	if (line) {
		if (filename && *filename) {
			tb.push_back(':');
		}
		tb.append(std::to_string(line));
	}
	if (function && *function) {
		if ((filename && *filename) || line) {
			tb.append(" at ");
		}
		tb.append(function);
	}

	if (callstack == nullptr) {
		tb.append(":\n    <invalid traceback>");
		return tb;
	}

	std::size_t frames = static_cast<void**>(*callstack) - callstack;

	if (frames == 0) {
		tb.append(":\n    <empty traceback>");
		return tb;
	}

	if (frames < 2) {
		tb.append(":\n    <no traceback>");
		return tb;
	}

	tb.push_back(':');

	// Iterate over the callstack. Skip the first, it is the address of this
	// function. Symbolization is the box's describe() so there is one copy of
	// the dladdr/demangle/atos logic.
	for (std::size_t i = static_cast<std::size_t>(skip); i < frames; ++i) {
		auto address = callstack[i + 1];
		tb.append(std::format("\n    {:3} {:#018x} {}", frames - i - 1,
			reinterpret_cast<std::uintptr_t>(address), describe(address)));
	}

	return tb;
}


std::string
traceback(const char* function, const char* filename, int line, int skip)
{
	// retrieve current stack addresses
	auto callstack = backtrace();
	auto tb = traceback(function, filename, line, callstack, skip);
	if (callstack != nullptr) {
		std::free(callstack);
	}
	return tb;
}


typedef void (*unexpected_handler)();

struct __cxa_exception {
	std::size_t referenceCount;

	std::type_info* exceptionType;
	void (*exceptionDestructor)(void*);
	unexpected_handler unexpectedHandler;
	std::terminate_handler terminateHandler;

	__cxa_exception* nextException;

	int handlerCount;

	int handlerSwitchValue;
	const unsigned char* actionRecord;
	const unsigned char* languageSpecificData;
	void* catchTemp;
	void* adjustedPtr;

	_Unwind_Exception unwindHeader;
};


void**
exception_callstack(std::exception_ptr& eptr)
{
	void* thrown_object = *static_cast<void**>(static_cast<void*>(&eptr));
	auto exception_header = static_cast<__cxa_exception*>(thrown_object) - 1;
	auto callstack = static_cast<void***>(static_cast<void*>(exception_header)) - 1;
	return *callstack;
}


#if defined(TRACEBACK_HOOKS)

// Global allocator + __cxa_throw interposition. With the hooks on, every thrown
// exception carries the throw-site callstack: malloc/calloc/realloc/free reserve
// one pointer's worth of slack ahead of each allocation, and __cxa_throw stashes
// backtrace() there before forwarding to the real implementation.
//
// This is an allocator-interposition landmine (it fights tcmalloc/jemalloc and
// sanitizers and leans on libc++/libsupc++ ABI internals), which is exactly why
// it is opt-in. Off by default; on, the all-thread snapshot and per-thread
// formatter still work, this only adds throw-site stacks.

extern "C" {

typedef void* (*malloc_type)(std::size_t);
void* malloc(std::size_t size) {
	static malloc_type orig_malloc = (malloc_type)dlsym(RTLD_NEXT, "malloc");
	assert(orig_malloc != nullptr);
	void* p = orig_malloc(size + alignof(max_align_t));
	if (p) {
		std::memset(p, 0, alignof(max_align_t));
		p = static_cast<char*>(p) + alignof(max_align_t);
	}
	return p;
}

void* calloc(std::size_t count, std::size_t size) {
	static malloc_type orig_malloc = (malloc_type)dlsym(RTLD_NEXT, "malloc");
	assert(orig_malloc != nullptr);
	void* p = orig_malloc(count * size + alignof(max_align_t));
	if (p) {
		std::memset(p, 0, count * size + alignof(max_align_t));
		p = static_cast<char*>(p) + alignof(max_align_t);
	}
	return p;
}

typedef void* (*realloc_type)(void*, std::size_t);
void* realloc(void* p, std::size_t size) {
	auto pp = p ? *(static_cast<void**>(p) - 1) : nullptr;
	if (p) {
		p = static_cast<char*>(p) - alignof(max_align_t);
	}
	static realloc_type orig_realloc = (realloc_type)dlsym(RTLD_NEXT, "realloc");
	assert(orig_realloc != nullptr);
	p = orig_realloc(p, size + alignof(max_align_t));
	if (p) {
		std::memset(p, 0, alignof(max_align_t));
		*static_cast<void**>(p) = pp;
		p = static_cast<char*>(p) + alignof(max_align_t);
	}
	return p;
}

typedef void (*free_type)(void*);
void free(void* p) {
	if (p) {
		auto pp = static_cast<void**>(p) - 1;
		if (*pp != nullptr) {
			free(*pp);
		}
		p = static_cast<char*>(p) - alignof(max_align_t);
	}
	static free_type orig_free = (free_type)dlsym(RTLD_NEXT, "free");
	assert(orig_free != nullptr);
	orig_free(p);
}

// GCC's built-in prototype for __cxa_throw uses 'void *', not 'std::type_info *'
#ifdef __clang__
typedef std::type_info __cxa_throw_type_info_t;
#else
typedef void __cxa_throw_type_info_t;
#endif
typedef void (*cxa_throw_type)(void*, __cxa_throw_type_info_t*, void (*)(void*));
void __cxa_throw(void* thrown_object, __cxa_throw_type_info_t* tinfo, void (*dest)(void*))
{
	// save callstack for exception (at the start of the reserved memory)
	auto exception_header = static_cast<__cxa_exception*>(thrown_object) - 1;
	auto callstack = static_cast<void**>(static_cast<void*>(exception_header)) - 1;
	*callstack = backtrace();
	// call original __cxa_throw:
	static cxa_throw_type orig_cxa_throw = (cxa_throw_type)dlsym(RTLD_NEXT, "__cxa_throw");
	assert(orig_cxa_throw != nullptr);
	orig_cxa_throw(thrown_object, tinfo, dest);
	__builtin_unreachable();
}

}  // extern "C"

#endif  // TRACEBACK_HOOKS

////////////////////////////////////////////////////////////////////////////////


void
collect_callstack_sig_handler(int /*signum*/, siginfo_t* /*info*/, void* ptr)
{
	struct frameinfo {
		frameinfo* next;
		void* return_address;
	};

	ucontext_t* uc = static_cast<ucontext_t*>(ptr);
#if defined(__i386__)
	#if defined(__FreeBSD__)
		auto frame = uc ? ((const frameinfo*)uc->uc_mcontext.mc_ebp) : nullptr;
	#elif defined(__linux__)
		auto frame = uc ? ((const frameinfo*)uc->uc_mcontext.gregs[REG_EBP]) : nullptr;
	#elif defined(__APPLE__)
		auto frame = uc && uc->uc_mcontext ? ((const frameinfo*)uc->uc_mcontext->__ss.__ebp) : nullptr;
	#else
		#error Unsupported OS.
	#endif
#elif defined(__x86_64__)
	#if defined(__FreeBSD__)
		auto frame = uc ? ((const frameinfo*)uc->uc_mcontext.mc_rbp) : nullptr;
	#elif defined(__linux__)
		auto frame = uc ? ((const frameinfo*)uc->uc_mcontext.gregs[REG_RBP]) : nullptr;
	#elif defined(__APPLE__)
		auto frame = uc && uc->uc_mcontext ? ((const frameinfo*)uc->uc_mcontext->__ss.__rbp) : nullptr;
	#else
		#error Unsupported OS.
	#endif
#elif defined(__aarch64__) || defined(__arm64__)
	// On AArch64 the frame pointer is x29; the frame record it points at is
	// { saved x29, saved x30 (return address) }, matching `frameinfo`.
	#if defined(__FreeBSD__)
		auto frame = uc ? ((const frameinfo*)uc->uc_mcontext.mc_gpregs.gp_x[29]) : nullptr;
	#elif defined(__linux__)
		auto frame = uc ? ((const frameinfo*)uc->uc_mcontext.regs[29]) : nullptr;
	#elif defined(__APPLE__)
		auto frame = uc && uc->uc_mcontext ? ((const frameinfo*)uc->uc_mcontext->__ss.__fp) : nullptr;
	#else
		#error Unsupported OS.
	#endif
#else
	#error Unsupported architecture.
#endif
#ifdef __MACHINE_STACK_GROWS_UP
	#define BELOW >
#else
	#define BELOW <
#endif
	void* stack = &stack;
	auto return_address = frame && !(frame BELOW stack) ? frame->return_address : nullptr;
#undef BELOW

	auto pthread = pthread_self();
	for (std::size_t idx = 0; idx < pthreads.size() && idx < pthreads_cnt.load(std::memory_order_acquire); ++idx) {
		auto& thread_info = pthreads[idx];
		if (thread_info.pthread.load(std::memory_order_relaxed) == pthread) {
			void* buf[MAX_DUMP_FRAMES];
#if defined(TRACEBACK_HAVE_EXECINFO)
			std::size_t frames = static_cast<std::size_t>(::backtrace(buf, MAX_DUMP_FRAMES));
#else
			std::size_t frames = 0;
#endif
			void** callstack = buf;
			for (std::size_t n = 0; n < frames; ++n) {
				if (buf[n] == return_address) {
					callstack = &buf[n];
					frames -= n;
					break;
				}
			}
			for (std::size_t n = 0; n < frames; ++n) {
				thread_info.callstack[n].store(callstack[n], std::memory_order_relaxed);
			}
			thread_info.callstack_frames.store(frames, std::memory_order_release);
			thread_info.req.store(pthreads_req.load(std::memory_order_acquire), std::memory_order_release);
			return;
		}
	}
}


std::string
dump_callstacks()
{
	std::size_t req = pthreads_req.fetch_add(1) + 1;

	// request all threads to collect their callstack
	for (std::size_t idx = 0; idx < pthreads.size() && idx < pthreads_cnt.load(std::memory_order_acquire); ++idx) {
		auto& thread_info = pthreads[idx];
		auto pthread = thread_info.pthread.load(std::memory_order_relaxed);
		if (pthread) {
			thread_info.errnum.store(pthread_kill(pthread, SIGUSR2), std::memory_order_release);
		}
	}

	// try waiting for callstacks
	for (int w = 10; w >= 0; --w) {
		std::size_t ok = 0;
		for (std::size_t idx = 0; idx < pthreads.size() && idx < pthreads_cnt.load(std::memory_order_acquire); ++idx) {
			auto& thread_info = pthreads[idx];
			if (thread_info.req.load() >= req) {
				++ok;
			}
		}
		if (ok == pthreads_cnt.load(std::memory_order_acquire)) {
			break;
		}
		sched_yield();
	}

	// print tracebacks:
	// The first idx is main thread:
	//   skip 4 frames:  callstacks_snapshot -> setup_node_async_cb -> ev::base -> ev_invoke_pending
	std::size_t skip_snap = 4;
	//   skip 5 frames:  dump_callstacks -> signal_sig_impl -> signal_sig_async_cb -> ev::base -> ev_invoke_pending
	std::size_t skip_call = 5;

	std::size_t idx = 0;
	std::size_t active = 0;
	std::string ret;
	for (; idx < pthreads.size() && idx < pthreads_cnt.load(std::memory_order_acquire); ++idx) {
		auto& thread_info = pthreads[idx];
		auto pthread = thread_info.pthread.load(std::memory_order_relaxed);
		if (pthread) {
			auto errnum = thread_info.errnum.load(std::memory_order_acquire);

			auto snapshot_frames = thread_info.snapshot_frames.load(std::memory_order_acquire);
			void* snapshot[MAX_DUMP_FRAMES + 1];
			for (std::size_t n = 0; n < snapshot_frames; ++n) {
				snapshot[n + 1] = thread_info.snapshot[n].load(std::memory_order_relaxed);
			}
			snapshot[0] = &snapshot[snapshot_frames];

			auto callstack_frames = thread_info.callstack_frames.load(std::memory_order_acquire);
			void* callstack[MAX_DUMP_FRAMES + 1];
			for (std::size_t n = 0; n < callstack_frames; ++n) {
				callstack[n + 1] = thread_info.callstack[n].load(std::memory_order_relaxed);
			}
			callstack[0] = &callstack[callstack_frames];

			if (!snapshot_frames || !callstack_frames) {
				++active;
				ret.append(strings::format("        " + STEEL_BLUE + "<Thread {}: {}{}{}>\n", idx, thread_info.name, !snapshot_frames ? " " + DARK_STEEL_BLUE + "(no snapshot)" + STEEL_BLUE : " " + DARK_STEEL_BLUE + "(no callstack)" + STEEL_BLUE, errnum ? " " + RED + "(" + error::name(errnum) + ")" + STEEL_BLUE : ""));
				if (callstack_frames) {
					ret.append(strings::format(DEBUG_COL + "{}\n", strings::indent(traceback(thread_info.name, "", static_cast<int>(idx), callstack, static_cast<int>(skip_call)), ' ', 8, true)));
				}
			} else if (callstack[1 + skip_call] != snapshot[1 + skip_snap]) {
				++active;
				ret.append(strings::format("        " + STEEL_BLUE + "<Thread {}: {}{}{}>\n", idx, thread_info.name, callstack[1 + skip_call] == snapshot[1 + skip_snap] ? " " + DARK_STEEL_BLUE + "(idle)" + STEEL_BLUE : " " + DARK_ORANGE + "(active)" + STEEL_BLUE, errnum ? " " + RED + "(" + error::name(errnum) + ")" + STEEL_BLUE : ""));
				ret.append(strings::format(DEBUG_COL + "{}\n", strings::indent(traceback(thread_info.name, "", static_cast<int>(idx), callstack, static_cast<int>(skip_call)), ' ', 8, true)));
			}
		}
		skip_call = 0;
		skip_snap = 0;
	}
	return strings::format("    " + STEEL_BLUE + "<Threads {{total:{}, active:{}}}>\n", idx, active) + ret;
}


void
callstacks_snapshot()
{
	// Try to get a stable snapshot of callbacks for all threads.

	for (int t = 10; t >= 0; --t) {
		bool retry = true;
		auto start = std::chrono::steady_clock::now();
		auto end = start;
		while (retry && std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() < 100) {
			std::size_t req = pthreads_req.fetch_add(1) + 1;

			// request all threads to collect their callstack
			for (std::size_t idx = 0; idx < pthreads.size() && idx < pthreads_cnt.load(); ++idx) {
				auto& thread_info = pthreads[idx];
				auto pthread = thread_info.pthread.load(std::memory_order_relaxed);
				if (pthread) {
					auto snapshot_frames = thread_info.snapshot_frames.load(std::memory_order_acquire);
					auto callstack_frames = thread_info.callstack_frames.load(std::memory_order_acquire);
					if (!snapshot_frames || !callstack_frames || snapshot_frames != callstack_frames) {
						thread_info.errnum.store(pthread_kill(pthread, SIGUSR2), std::memory_order_release);
					}
				}
			}

			// try waiting for callstacks
			for (int w = 10; w >= 0; --w) {
				std::size_t ok = 0;
				for (std::size_t idx = 0; idx < pthreads.size() && idx < pthreads_cnt.load(); ++idx) {
					auto& thread_info = pthreads[idx];
					if (thread_info.req.load() >= req) {
						++ok;
					}
				}
				if (ok == pthreads_cnt.load()) {
					break;
				}
				sched_yield();
			}

			// save snapshots:
			retry = false;
			for (std::size_t idx = 0; idx < pthreads.size() && idx < pthreads_cnt.load(); ++idx) {
				auto& thread_info = pthreads[idx];
				auto pthread = thread_info.pthread.load(std::memory_order_relaxed);
				if (pthread) {
					auto snapshot_frames = thread_info.snapshot_frames.load(std::memory_order_acquire);
					auto callstack_frames = thread_info.callstack_frames.load(std::memory_order_acquire);
					if (!snapshot_frames || !callstack_frames || snapshot_frames != callstack_frames) {
						retry = true;
					} else {
						for (std::size_t n = 0; n < snapshot_frames; ++n) {
							if (thread_info.snapshot[n].load(std::memory_order_relaxed) != thread_info.callstack[n].load(std::memory_order_relaxed)) {
								retry = true;
								break;
							}
						}
					}
					if (retry) {
						for (std::size_t n = 0; n < callstack_frames; ++n) {
							thread_info.snapshot[n].store(thread_info.callstack[n].load(std::memory_order_acquire), std::memory_order_relaxed);
						}
						thread_info.snapshot_frames.store(callstack_frames, std::memory_order_release);
					}
				} else {
					retry = true;
				}
			}

			sched_yield();

			end = std::chrono::steady_clock::now();
		}

		if (t == 0) {
			if (retry) {
				// The library must not pull in a logger; the original logged an
				// L_WARNING here. Report on stderr and move on.
				std::fprintf(stderr, "traceback: cannot take a snapshot of callbacks\n");
			}
			break;
		}

		nanosleep(10000000);  // sleep for 10 milliseconds
	}
}


void
register_thread(pthread_t pthread, const char* name)
{
	auto idx = pthreads_cnt.fetch_add(1);
	if (idx < pthreads.size()) {
		pthreads[idx].name = name;
		pthreads[idx].pthread.store(pthread, std::memory_order_release);
	}
}

}  // namespace traceback
