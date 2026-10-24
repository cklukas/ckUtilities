// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_filter.hpp"
#include <stdexcept>
namespace ck::files {
PaneFilterMatcher::PaneFilterMatcher(const PaneFilter &filter)
    : show_hidden_(filter.show_hidden), case_sensitive_(filter.ascii_case_sensitive) {
    if (filter.display_substring.size() > 4096)
        throw std::invalid_argument("pane filter exceeds limit");
    pattern_ = filter.display_substring;
    for (char &c : pattern_)
        c = static_cast<char>(fold(static_cast<unsigned char>(c)));
    prefix_.resize(pattern_.size());
    for (std::size_t i = 1, j = 0; i < pattern_.size(); ++i) {
        while (j && pattern_[i] != pattern_[j])
            j = prefix_[j - 1];
        if (pattern_[i] == pattern_[j])
            ++j;
        prefix_[i] = j;
    }
}
unsigned char PaneFilterMatcher::fold(unsigned char value) const noexcept {
    return !case_sensitive_ && value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}
PaneFilterMatch PaneFilterMatcher::match(std::string_view display, const Observation<bool> &hidden,
                                         std::stop_token stop) const {
    if (stop.stop_requested())
        return PaneFilterMatch::Cancelled;
    if (!show_hidden_ && hidden.value() && *hidden.value())
        return PaneFilterMatch::Excluded;
    if (pattern_.empty())
        return PaneFilterMatch::Included;
    for (std::size_t i = 0, j = 0; i < display.size(); ++i) {
        if ((i & 255) == 0 && stop.stop_requested())
            return PaneFilterMatch::Cancelled;
        const auto c = fold(static_cast<unsigned char>(display[i]));
        while (j && c != static_cast<unsigned char>(pattern_[j]))
            j = prefix_[j - 1];
        if (c == static_cast<unsigned char>(pattern_[j]))
            ++j;
        if (j == pattern_.size())
            return stop.stop_requested() ? PaneFilterMatch::Cancelled : PaneFilterMatch::Included;
    }
    return stop.stop_requested() ? PaneFilterMatch::Cancelled : PaneFilterMatch::Excluded;
}
} // namespace ck::files
