// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_settings.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

#include "files_settings_allocation_bound_fixture.hpp"
using namespace settings_bound_fixture;

int main() {
    using namespace ck::vision;
    ckv::MemoryFileSystem files;
    FilesSettingsStore store(files, "/settings/value");
    if (!store.load().diagnostic.empty() || !store.save({}).empty()) return 1;
    const auto before = files.read_file("/settings/value");
    FilesPersistentSettings oversized;
    oversized.views[0].display_substring.assign(2 * cap, 'x');
    reject_large_allocation = true;
    bool control = false;
    try { auto *value = ::operator new(cap + 1); ::operator delete(value); }
    catch (const std::bad_alloc &) { control = true; }
    rejected = 0;
    bool threw = false;
    std::string diagnostic;
    try { diagnostic = store.save(oversized); }
    catch (...) { threw = true; }
    reject_large_allocation = false;
    const auto after = files.read_file("/settings/value");
    if (!control || threw || rejected || diagnostic.empty() || !before || !after ||
        before->contents != after->contents || before->fingerprint != after->fingerprint) {
        std::fprintf(stderr, "settings preflight failed: control=%d threw=%d large_allocations=%u\n",
                     control, threw, rejected);
        return 1;
    }
    if (!store.save({}).empty()) return 1; // Rejection must not poison the CAS revision.
    std::puts("PASS direct settings save rejects before any over-cap allocation; old bytes/revision preserved");
    return 0;
}
