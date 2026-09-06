// SPDX-License-Identifier: GPL-3.0-or-later
//
// A .so/.dylib with the right extension but no macha_subsystem_entry symbol
// at all, used only by
// foundations/test_subsystem_supervisor_silently_skips_a_non_plugin_library
// to prove that a plugin_path directory holding an unrelated library (e.g.
// /usr/bin/ld.so on a real install where plugin_path defaults to a shared
// system bindir) is skipped quietly, not reported as a disabled subsystem.
namespace {
int unrelated_exported_value = 42;
}

extern "C" int macha_test_plugin_no_entry_symbol_marker() {
    return unrelated_exported_value;
}
