// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string_view>

namespace macha {

// The mandatory entry point for every subsystem-owned thread body. An
// exception escaping a std::jthread/std::thread lambda does not reach any
// caller's try/catch -- it calls std::terminate() and aborts the whole
// process before main()'s own catch block is ever reachable. run_supervised
// is the one place that boundary is guarded: any exception thrown by `body`
// is caught, logged with `name` for context, and swallowed instead of
// escaping. `name` should identify the subsystem/thread (e.g. "ingest",
// "torrent", "fuse-namespace") so the log line says which subsystem stopped.
void run_supervised(std::string_view name, const std::function<void()>& body) noexcept;

} // namespace macha
