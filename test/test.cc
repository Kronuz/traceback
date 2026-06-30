// Smoke test for the traceback box. Uses CHECK (not assert) so it verifies even
// in NDEBUG/Release builds.

#include "traceback.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <string>

#define CHECK(cond) do { \
	if (!(cond)) { \
		std::fprintf(stderr, "CHECK FAILED at line %d: %s\n", __LINE__, #cond); \
		return 1; \
	} \
} while (0)

__attribute__((noinline))
static std::string deep(int n)
{
	if (n > 0) {
		return deep(n - 1);
	}
	return traceback::format("test-header");
}

int main()
{
	// capture() returns frames of the current stack.
	auto frames = traceback::capture();
	CHECK(!frames.empty());

	// describe() never throws and always returns something.
	std::string d = traceback::describe(frames.front());
	CHECK(!d.empty());

	// format() builds a multi-line traceback with the header.
	auto tb = deep(3);
	CHECK(tb.find("Traceback") != std::string::npos);
	CHECK(tb.find("test-header") != std::string::npos);

	// the source_location overload builds a header from the call site.
	auto loc = traceback::format(std::source_location::current());
	CHECK(loc.find("Traceback") != std::string::npos);
	CHECK(loc.find("test.cc") != std::string::npos);

	// enable_atos toggles the flag (no-op off macOS / when atos unavailable).
	traceback::enable_atos(true);
	(void)traceback::describe(frames.front());   // must not crash
	traceback::enable_atos(false);

	// ---- raw layer (always available) -----------------------------------
	// backtrace()/traceback() are part of the toolkit but do not carry the heavy
	// registry, so they compile and run in the default (hooks-off) build.

	// backtrace() returns a raw void** block (slot 0 is the sentinel/length).
	void** raw = traceback::backtrace();
	CHECK(raw != nullptr);
	std::size_t raw_frames = static_cast<void**>(*raw) - raw;
	CHECK(raw_frames > 0);

	// traceback() formats a raw callstack; route through the same describe().
	std::string raw_tb = traceback::traceback("smoke", __FILE__, __LINE__, raw, 0);
	CHECK(raw_tb.find("Traceback") != std::string::npos);
	std::free(raw);

#if defined(TRACEBACK_HOOKS)
	// ---- crash dumper smoke test (TRACEBACK_HOOKS only) -----------------
	// Single-threaded: register the main thread via the RAII guard, install the
	// SIGUSR2 handler the dumper broadcasts to, then exercise the public entry
	// points. The multi-thread signal-broadcast path is hard to make
	// deterministic in a unit test, so this just confirms the entry points run
	// and produce output without crashing.

	// Install the SIGUSR2 handler the dumper drives.
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sa.sa_sigaction = traceback::collect_callstack_sig_handler;
	CHECK(sigaction(SIGUSR2, &sa, nullptr) == 0);

	// Register main through the RAII helper (constructor calls register_thread).
	traceback::thread_registration main_reg("main");

	// callstacks_snapshot() takes a baseline; dump_callstacks() broadcasts the
	// signal and renders every registered thread. Both must return non-empty
	// formatted output and not crash.
	traceback::callstacks_snapshot();
	std::string dump = traceback::dump_callstacks();
	CHECK(!dump.empty());
	CHECK(dump.find("Threads") != std::string::npos);

	// ---- slot reuse -----------------------------------------------------
	// A thread that registers then deregisters frees its slot; the next
	// registration reuses it instead of extending the registry's high-water
	// mark. The dump header reports total:N, where N is that high-water mark
	// (it counts every slot ever handed out, freed or live). Run a worker that
	// registers (RAII), parks until the main thread has read the mark, then
	// exits (RAII teardown deregisters). Do it twice: the first worker pushes the
	// mark up by one; the second must reuse the freed slot, so the mark does NOT
	// grow again.
	//
	// All dumping happens on the MAIN thread, and we snapshot the parked worker
	// first so dump_callstacks() sees it as idle and does not symbolize it. That
	// avoids describe() -> __cxa_demangle -> free() on a C++-mangled worker frame:
	// with TRACEBACK_HOOKS on, the process-wide malloc/free interposition makes
	// freeing a __cxa_demangle buffer through the override unsafe (the documented
	// landmine), which is orthogonal to the registry behavior under test here.
	auto count_total = [](const std::string& d) -> std::size_t {
		// dump header looks like: <Threads {total:N, active:M}>
		auto pos = d.find("total:");
		if (pos == std::string::npos) { return 0; }
		return std::strtoul(d.c_str() + pos + 6, nullptr, 10);
	};

	// total reported by a dump, taken after a fresh snapshot so every parked
	// thread is idle (unchanged since the snapshot) and nothing is symbolized.
	auto measured_total = [&count_total]() -> std::size_t {
		traceback::callstacks_snapshot();
		return count_total(traceback::dump_callstacks());
	};

	struct sync { std::atomic<bool> registered{false}; std::atomic<bool> release{false}; };
	auto worker = [](void* p) -> void* {
		auto* s = static_cast<sync*>(p);
		traceback::thread_registration reg("worker");
		s->registered.store(true, std::memory_order_release);
		while (!s->release.load(std::memory_order_acquire)) {
			sched_yield();
		}
		return nullptr;  // RAII teardown deregisters here
	};

	// Baseline high-water mark with just main registered.
	std::size_t base_total = measured_total();

	sync s1;
	pthread_t t1;
	CHECK(pthread_create(&t1, nullptr, worker, &s1) == 0);
	while (!s1.registered.load(std::memory_order_acquire)) { sched_yield(); }
	std::size_t total1 = measured_total();
	CHECK(total1 == base_total + 1);     // first worker extended the registry by one
	s1.release.store(true, std::memory_order_release);
	CHECK(pthread_join(t1, nullptr) == 0);

	sync s2;
	pthread_t t2;
	CHECK(pthread_create(&t2, nullptr, worker, &s2) == 0);
	while (!s2.registered.load(std::memory_order_acquire)) { sched_yield(); }
	std::size_t total2 = measured_total();
	CHECK(total2 == base_total + 1);     // second worker reused the freed slot: no growth
	s2.release.store(true, std::memory_order_release);
	CHECK(pthread_join(t2, nullptr) == 0);

	// And after both exited, the mark held steady (no slot leak under churn).
	CHECK(measured_total() == base_total + 1);

	std::printf("traceback test: OK (%zu frames, dump %zu bytes, slot reuse ok)\n",
		frames.size(), dump.size());
#else
	std::printf("traceback test: OK (%zu frames, box only; dumper gated out)\n",
		frames.size());
#endif
	return 0;
}
