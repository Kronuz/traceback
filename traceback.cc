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

#include <atomic>            // for std::atomic
#include <cstdint>           // for uintptr_t
#include <cstdio>            // for std::snprintf
#include <cstdlib>           // for std::free
#include <cstring>           // for std::memset
#include <format>            // for std::format

#if defined(__has_include)
#  if __has_include(<execinfo.h>)
#    include <execinfo.h>    // for ::backtrace
#    define TRACEBACK_HAVE_EXECINFO 1
#  endif
#  if __has_include(<dlfcn.h>)
#    include <dlfcn.h>       // for dladdr
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

}  // namespace traceback
