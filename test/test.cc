// Smoke test for the traceback box. Uses CHECK (not assert) so it verifies even
// in NDEBUG/Release builds.

#include "traceback.h"

#include <cstdio>
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

	std::printf("traceback test: OK (%zu frames)\n", frames.size());
	return 0;
}
