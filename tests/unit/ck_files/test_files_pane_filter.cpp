// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_filter.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
namespace {
using namespace ck::files;
std::string folded(std::string value) {
    for (char &c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return value;
}
} // namespace
CK_TEST(pane_filter_literal_case_and_hidden_uncertainty_are_explicit) {
    const auto hidden = Observation<bool>::available(true), visible = Observation<bool>::available(false);
    for (bool show : {false, true}) {
        PaneFilterMatcher m({show, "Ab", false});
        CK_CHECK(m.match("zABz", visible) == PaneFilterMatch::Included);
        CK_CHECK(m.match("zABz", hidden) == (show ? PaneFilterMatch::Included : PaneFilterMatch::Excluded));
        CK_CHECK(m.match("zABz", {}) == PaneFilterMatch::Included);
        CK_CHECK(m.match("ab", Observation<bool>::unavailable(ObservationState::Denied,
                                                              {ErrorDomain::Posix, 13, "hidden"})) ==
                 PaneFilterMatch::Included);
    }
    CK_CHECK(PaneFilterMatcher({true, "Ab", true}).match("ab", {}) == PaneFilterMatch::Excluded);
    CK_CHECK(PaneFilterMatcher({false, "", false}).match("", {}) == PaneFilterMatch::Included);
}
CK_TEST(pane_filter_matches_independent_binary_substring_oracle) {
    const std::string alphabet("AaBb\\\0\xFF", 7);
    std::uint32_t rng = 12345;
    const auto make = [&](std::size_t n) {
        std::string s;
        for (std::size_t i = 0; i < n; ++i) {
            rng = rng * 1664525U + 1013904223U;
            s += alphabet[rng % alphabet.size()];
        }
        return s;
    };
    for (int i = 0; i < 4000; ++i) {
        auto text = make(static_cast<std::size_t>(i % 41));
        auto pattern = make(static_cast<std::size_t>(i % 9));
        if (i % 3 == 0 && !text.empty())
            pattern = text.substr(text.size() / 2, 3);
        for (bool sensitive : {false, true}) {
            const bool expected =
                (sensitive ? text : folded(text)).find(sensitive ? pattern : folded(pattern)) != std::string::npos;
            CK_CHECK(PaneFilterMatcher({true, pattern, sensitive}).match(text, {}) ==
                     (expected ? PaneFilterMatch::Included : PaneFilterMatch::Excluded));
        }
    }
}
CK_TEST(pane_filter_long_overlapping_prefixes_have_bounded_compiled_storage) {
    const auto pattern = std::string(4095, 'A') + 'b';
    const auto text = std::string(500000, 'A') + 'B';
    CK_CHECK(PaneFilterMatcher({true, pattern, false}).match(text, {}) == PaneFilterMatch::Included);
    CK_CHECK(PaneFilterMatcher({true, pattern, true}).match(text, {}) == PaneFilterMatch::Excluded);
    bool threw = false;
    try {
        PaneFilterMatcher invalid({true, std::string(4097, 'x'), false});
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CK_CHECK(threw);
}
CK_TEST(pane_filter_stop_is_distinct_from_inclusion_or_exclusion) {
    std::stop_source stop;
    stop.request_stop();
    for (const auto &pattern : std::vector<std::string>{"", "match", "absent"}) {
        PaneFilterMatcher m({false, pattern, false});
        CK_CHECK(m.match("match", Observation<bool>::available(true), stop.get_token()) == PaneFilterMatch::Cancelled);
        CK_CHECK(m.match("match", {}, stop.get_token()) == PaneFilterMatch::Cancelled);
    }
}
