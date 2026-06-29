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

// A small, modern C++ stack-trace box, extracted and modernized from Xapiand.
//
// It captures the current call stack and symbolizes it (demangled symbol +
// offset via dladdr/abi, plus file:line via `atos` on macOS), and formats a
// human-readable traceback. The reusable core only; the throw-site capture hack
// (a __cxa_throw + global-allocator interposition) and the signal-time all-thread
// crash dumper from the original are intentionally left out.
//
// Forward-compatible with C++23 <stacktrace>: when libc++/libstdc++ ship it, the
// capture/symbolize paths below can be swapped for std::stacktrace behind the
// same API (the `#if __cpp_lib_stacktrace` guard marks the seam). As of mid-2026
// neither Apple clang's nor Homebrew LLVM's libc++ ships it, so the manual path
// is what runs.

#pragma once

#include <cstddef>            // for std::size_t
#include <source_location>   // for std::source_location
#include <string>            // for std::string
#include <string_view>       // for std::string_view
#include <vector>            // for std::vector

namespace traceback {

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

}  // namespace traceback
