// The opt-in hooks: the crash dumper (thread registry, SIGUSR2 handler, all-thread
// dump) and, unless a sanitizer owns the allocator, the throw-site tracker.
#include "traceback.h"

#include <atomic>
#include <cassert>
#include <csignal>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>

static std::atomic<bool> stop{false};
static std::atomic<bool> ready{false};

static void* worker(void*) {
	traceback::thread_registration reg("hooks-worker");
	ready = true;
	while (!stop) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return nullptr;
}

static void test_dumper() {
	struct sigaction sa{};
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sa.sa_sigaction = traceback::collect_callstack_sig_handler;
	assert(sigaction(SIGUSR2, &sa, nullptr) == 0);

	traceback::register_thread(pthread_self(), "hooks-main");
	pthread_t thread;
	assert(pthread_create(&thread, nullptr, worker, nullptr) == 0);
	while (!ready) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	traceback::callstacks_snapshot();
	std::string dump = traceback::dump_callstacks();
	// Both threads are registered; the sleeping worker is idle since the snapshot, so
	// only the main thread (which is dumping) prints a traceback.
	assert(dump.find("total:2") != std::string::npos);
	assert(dump.find("hooks-main") != std::string::npos);
	assert(dump.find("Traceback") != std::string::npos);
	stop = true;
	pthread_join(thread, nullptr);
	std::printf("dumper OK: both threads registered, the active one traced\n");
}

#if defined(TRACEBACK_THROW_HOOKS)
[[gnu::noinline]] static void thrower() {
	throw std::runtime_error("hooked");
}

static void test_throw_hooks() {
	try {
		thrower();
	} catch (...) {
		auto eptr = std::current_exception();
		void** callstack = traceback::exception_callstack(eptr);
		assert(callstack != nullptr);
		std::string text = traceback::traceback("thrower", __FILE__, __LINE__, callstack, 0);
		assert(!text.empty());
	}
	std::printf("throw hooks OK: the exception carries its throw-site callstack\n");
}
#endif

int main() {
	test_dumper();
#if defined(TRACEBACK_THROW_HOOKS)
	test_throw_hooks();
#endif
	std::printf("all traceback hook tests passed\n");
}
