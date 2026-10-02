// SPDX-License-Identifier: GPL-3.0-or-later
//
// A shared library with no macha_subsystem_entry symbol: an unrelated library in
// plugin_path must be skipped quietly, not reported as a disabled subsystem.
namespace {
int unrelated_exported_value = 42;
}

extern "C" int macha_test_plugin_no_entry_symbol_marker() {
    return unrelated_exported_value;
}
