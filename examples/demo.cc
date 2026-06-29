// A runnable tour of traceback.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/traceback_demo
//
// The one idea worth taking away: traceback turns the live call stack into a
// readable box. capture() grabs the raw return addresses, describe() resolves
// one address to a demangled "symbol + offset", and format() stitches a whole
// stack into a labelled traceback (most recent call first). This demo walks a
// few nested calls so real frames show up, prints the box, peeks at the raw
// addresses underneath it, and shows the macOS `atos` file:line opt-in.
#include <cstdio>
#include <string>
#include <vector>

#include "traceback.h"   // capture / describe / format / enable_atos

static void rule(const char* title) {
	std::printf("\n\033[1m── %s ──\033[0m\n", title);
}

// Three nested, non-inlined calls so the traceback has distinct frames to show.
// __attribute__((noinline)) keeps the optimizer from folding them into main, and
// the trailing concatenation after each call keeps it from turning `return f()`
// into a tail call (a jump), which would drop the frame off the live stack. So
// outer/middle/inner each stay as a real frame when format() walks the stack.
__attribute__((noinline))
static std::string inner() {
	// format() with a source_location header: the box gets a "file:line at func"
	// line built from the call site, then the live stack below it.
	std::string box = traceback::format(std::source_location::current());
	return box + "";
}

__attribute__((noinline))
static std::string middle() {
	std::string box = inner();
	return box + "";
}

__attribute__((noinline))
static std::string outer() {
	std::string box = middle();
	return box + "";
}

int main() {
	std::printf("traceback demo  (atos file:line is %s by default)\n",
		traceback::atos_enabled() ? "on" : "off");

	// --- 1. the traceback box from a few nested calls ------------------------
	rule("format(): the live stack, most recent call first");
	// outer() -> middle() -> inner() -> format(). The frames for outer/middle/
	// inner should appear in order under the header, each as "symbol + offset".
	std::string box = outer();
	std::fputs(box.c_str(), stdout);
	std::putc('\n', stdout);
	std::puts("  (the header is the inner() call site; the frames below are the live stack)");

	// --- 2. format() with a plain header string ------------------------------
	rule("format(header): same stack, your own header");
	// The header is free text instead of a source_location. skip=2 by default
	// drops format() and this frame's prologue so the box starts at real code.
	std::string named = traceback::format("snapshot from main");
	std::fputs(named.c_str(), stdout);
	std::putc('\n', stdout);

	// --- 3. capture() + describe(): the addresses under the box --------------
	rule("capture() + describe(): one frame at a time");
	// capture() is the raw layer: a vector of return addresses, innermost first.
	// describe() resolves one of them the same way format() does internally.
	std::vector<void*> frames = traceback::capture();
	std::printf("  capture() returned %zu raw return addresses\n", frames.size());
	std::size_t show = frames.size() < 4 ? frames.size() : 4;
	for (std::size_t i = 0; i < show; ++i) {
		std::printf("  %#018zx  %s\n",
			reinterpret_cast<std::uintptr_t>(frames[i]),
			traceback::describe(frames[i]).c_str());
	}

	// describe() never throws and never returns empty: a bogus address that
	// resolves to nothing still comes back as "[unknown symbol]", so it is safe
	// to call straight from an error path.
	std::printf("  describe(bogus) -> %s\n",
		traceback::describe(reinterpret_cast<void*>(0x1)).c_str());

	// --- 4. macOS atos: opt into file:line -----------------------------------
	rule("enable_atos(): file:line on macOS (opt-in)");
	// Off by default because it forks an `atos` child to read the symbol table.
	// Turn it on and the same describe() call enriches frames with source
	// locations where atos can resolve them. A no-op (stays off) off macOS, and
	// frames that atos can't place still fall back to "symbol + offset".
	traceback::enable_atos(true);
	std::printf("  atos_enabled() -> %s\n", traceback::atos_enabled() ? "true" : "false");
	if (traceback::atos_enabled()) {
		std::string atos_box = outer();
		std::fputs(atos_box.c_str(), stdout);
		std::putc('\n', stdout);
		std::puts("  (frames that atos resolves now carry file:line; others stay symbol + offset)");
	} else {
		std::puts("  (not macOS, or atos unavailable: frames stay symbol + offset)");
	}
	traceback::enable_atos(false);

	std::puts("\ndone.");
	return 0;
}
