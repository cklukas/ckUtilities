// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "file_metadata.hpp"
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>
namespace ck::files {
struct PaneFilter {
    bool show_hidden = true; // unfiltered storage lookup by default
    std::string display_substring;
    bool ascii_case_sensitive = false;
};
enum class PaneFilterMatch { Included, Excluded, Cancelled };
// Bounded compiled literal substring matcher. ASCII-only folding, no locale or
// normalization, and matching against escaped display text, never native paths.
// Linear-time KMP keeps cancellation bounded within long names/pattern matches.
class PaneFilterMatcher final {
  public:
    explicit PaneFilterMatcher(const PaneFilter &filter);
    PaneFilterMatch match(std::string_view display, const Observation<bool> &hidden, std::stop_token stop = {}) const;
    bool needs_display() const noexcept {
        return !pattern_.empty();
    }

  private:
    unsigned char fold(unsigned char value) const noexcept;
    bool show_hidden_, case_sensitive_;
    std::string pattern_;
    std::vector<std::size_t> prefix_;
};
} // namespace ck::files
