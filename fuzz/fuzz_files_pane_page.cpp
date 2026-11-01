// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "fuzz_common.hpp"
#include "pane_page.hpp"

namespace {

using namespace ck::files;

std::string next_name(const std::uint8_t *data, std::size_t size, std::size_t &offset,
                      std::size_t ordinal) {
    const auto remaining = size - offset;
    std::size_t length = 0;
    if (remaining != 0) {
        const auto requested = static_cast<std::size_t>(data[offset++]) % 96;
        length = std::min(requested, size - offset);
    }
    std::string bytes(reinterpret_cast<const char *>(data + offset), length);
    offset += length;
    if (const auto name = NativeName::from_posix(bytes); name && bytes != "." && bytes != "..")
        return bytes;
    return "fuzz-entry-" + std::to_string(ordinal);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0)
        return 0;

    PaneModel pane;
    std::size_t offset = 0;
    const auto requested_count = static_cast<std::size_t>(data[offset++]) % 128;
    const auto count = std::min(requested_count, size - offset);
    std::vector<PaneEntry> entries;
    entries.reserve(count);
    for (std::size_t index = 0; index < count && offset <= size; ++index) {
        auto name = next_name(data, size, offset, index);
        const auto hidden = offset < size && (data[offset++] & 1U) != 0;
        entries.emplace_back(*NativeName::from_posix(std::move(name)), BasicMetadata{},
                             Observation<bool>::available(hidden));
    }
    const auto request = pane.begin_listing(PaneListingPurpose::Navigate);
    if (pane.publish(request, entries) != PanePublish::Applied)
        return 0;

    if (offset < size) {
        const auto row = pane.visible_size() == 0 ? 0 : data[offset++] % pane.visible_size();
        if (const auto *target = pane.visible_row(row)) {
            (void)pane.focus(target->id);
            pane.set_mark(target->id, true);
        }
    }
    if (offset < size) {
        auto view = pane.view();
        const auto requested_length = static_cast<std::size_t>(data[offset++]) % 64;
        const auto length = std::min(requested_length, size - offset);
        view.display_substring.assign(reinterpret_cast<const char *>(data + offset), length);
        offset += length;
        pane.set_view(view);
    }

    const auto first = offset < size ? static_cast<std::size_t>(data[offset++]) : 0;
    const auto rows = 1 + (offset < size ? static_cast<std::size_t>(data[offset++]) : 0);
    const auto text_bytes = 1 + (offset < size
        ? static_cast<std::size_t>(data[offset++]) * 64U
        : 0U);
    const auto captured = capture_pane_page(pane, first, {rows, text_bytes});
    if (captured.state != PanePageState::Ready)
        return 0;

    ck::fuzz::require(captured.page != nullptr);
    ck::fuzz::require(captured.page->rows().size() <= rows);
    ck::fuzz::require(captured.page->text_bytes() <= text_bytes);
    for (const auto &row : captured.page->rows())
        ck::fuzz::require(!ck::fuzz::contains_terminal_control(row.display_name));
    return 0;
}
