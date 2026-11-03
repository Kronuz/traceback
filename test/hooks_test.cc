// The opt-in hooks: the crash dumper (thread registry, SIGUSR2 handler, all-thread
// dump) and, unless a sanitizer owns the allocator, the throw-site tracker.
#include "traceback.h"

#include <atomic>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>
#if defined(__FreeBSD__)
#include <ucontext.h>
#else
#include <sys/ucontext.h>     // <ucontext.h> needs _XOPEN_SOURCE on macOS
#endif

// Not CHECK(): the checks have side effects (pthread_create, sigaction), and a Release build drops assert.
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #cond); std::abort(); } } while (0)

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
	CHECK(sigaction(SIGUSR2, &sa, nullptr) == 0);

	traceback::register_thread(pthread_self(), "hooks-main");
	pthread_t thread;
	CHECK(pthread_create(&thread, nullptr, worker, nullptr) == 0);
	while (!ready) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	traceback::callstacks_snapshot();
	std::string dump = traceback::dump_callstacks();
	// Both threads are registered; the sleeping worker is idle since the snapshot, so
	// only the main thread (which is dumping) prints a traceback.
	CHECK(dump.find("total:2") != std::string::npos);
	CHECK(dump.find("hooks-main") != std::string::npos);
	CHECK(dump.find("Traceback") != std::string::npos);
	stop = true;
	pthread_join(thread, nullptr);
	std::printf("dumper OK: both threads registered, the active one traced\n");
}

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
// Code built without frame pointers keeps anything in rbp (x29): a thread interrupted there must not be
// followed through it. The handler runs on a registered thread with a context whose frame register is junk.
static void test_junk_frame_register() {
	static const unsigned long long junk[] = {
		0x7fffffffffffffffULL,   // misaligned
		0x7ffffffff000ULL,       // aligned, above any stack, unmapped
		0xfffffffffffffff0ULL,   // aligned, and its second word wraps the address space
	};
	traceback::register_thread(pthread_self(), "hooks-junk");
	for (auto value : junk) {
		ucontext_t uc;
		CHECK(getcontext(&uc) == 0);
#if defined(__x86_64__)
		uc.uc_mcontext.gregs[REG_RBP] = static_cast<greg_t>(value);
#else
		uc.uc_mcontext.regs[29] = value;
#endif
		traceback::collect_callstack_sig_handler(SIGUSR2, nullptr, &uc);
	}
	std::printf("junk frame register OK: the handler does not follow it\n");
}
#endif

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
		CHECK(callstack != nullptr);
		std::string text = traceback::traceback("thrower", __FILE__, __LINE__, callstack, 0);
		CHECK(!text.empty());
	}
	std::printf("throw hooks OK: the exception carries its throw-site callstack\n");
}
#endif

int main() {
	test_dumper();
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
	test_junk_frame_register();
#endif
#if defined(TRACEBACK_THROW_HOOKS)
	test_throw_hooks();
#endif
	std::printf("all traceback hook tests passed\n");
}
