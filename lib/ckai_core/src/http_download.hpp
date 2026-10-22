// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One HTTPS GET into a stream, for model downloads. libcurl implements it on
// macOS and Linux (http_download_curl.cpp), WinHTTP on Windows
// (http_download_winhttp.cpp), so Windows needs no third-party HTTP stack.
#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>

namespace ck::ai::detail {

// Called as data arrives: bytes received so far, and the total size or 0
// when the server did not announce one. Returning false cancels the transfer.
using HttpProgress = std::function<bool(std::uint64_t received, std::uint64_t total)>;

struct HttpDownloadResult {
  bool ok = false;         // HTTP 200 and every byte written
  bool cancelled = false;  // the progress callback asked to stop
  std::string error;       // why it failed, for the reader; empty on success
};

// Follows redirects, verifies the server certificate and host name, gives up
// after 30 s without a connection or 60 s without data, and sends the
// "ck-utilities" user agent. Bytes go to `out` as they arrive.
HttpDownloadResult http_download(const std::string &url, std::ostream &out,
                                 const HttpProgress &progress);

} // namespace ck::ai::detail
