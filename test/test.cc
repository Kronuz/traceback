// Smoke test for the traceback box. Uses CHECK (not assert) so it verifies even
// in NDEBUG/Release builds.

#include "traceback.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
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

	// ---- crash dumper smoke test ----------------------------------------
	// Single-threaded: register the main thread, install the SIGUSR2 handler the
	// dumper broadcasts to, then exercise the public entry points. The
	// multi-thread signal-broadcast path is hard to make deterministic in a unit
	// test, so this just confirms the entry points run and produce output without
	// crashing.

	// backtrace() returns a raw void** block (slot 0 is the sentinel/length).
	void** raw = traceback::backtrace();
	CHECK(raw != nullptr);
	std::size_t raw_frames = static_cast<void**>(*raw) - raw;
	CHECK(raw_frames > 0);

	// traceback() formats a raw callstack; route through the same describe().
	std::string raw_tb = traceback::traceback("smoke", __FILE__, __LINE__, raw, 0);
	CHECK(raw_tb.find("Traceback") != std::string::npos);
	std::free(raw);

	// Install the SIGUSR2 handler and register the main thread.
	struct sigaction sa;
	std::memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sa.sa_sigaction = traceback::collect_callstack_sig_handler;
	CHECK(sigaction(SIGUSR2, &sa, nullptr) == 0);

	traceback::register_thread(pthread_self(), "main");

	// callstacks_snapshot() takes a baseline; dump_callstacks() broadcasts the
	// signal and renders every registered thread. Both must return non-empty
	// formatted output and not crash.
	traceback::callstacks_snapshot();
	std::string dump = traceback::dump_callstacks();
	CHECK(!dump.empty());
	CHECK(dump.find("Threads") != std::string::npos);

	std::printf("traceback test: OK (%zu frames, dump %zu bytes)\n", frames.size(), dump.size());
	return 0;
}
