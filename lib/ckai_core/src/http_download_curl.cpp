// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// http_download on libcurl (macOS, Linux).
#include "http_download.hpp"

#include <curl/curl.h>

#include <memory>
#include <mutex>

namespace ck::ai::detail {

namespace {

struct Transfer {
  std::ostream *out = nullptr;
  const HttpProgress *progress = nullptr;
  bool cancelled = false;
};

size_t write_body(char *data, size_t size, size_t count, void *user) {
  auto *transfer = static_cast<Transfer *>(user);
  const size_t bytes = size * count;
  transfer->out->write(data, static_cast<std::streamsize>(bytes));
  // A short count makes libcurl stop with CURLE_WRITE_ERROR.
  return *transfer->out ? bytes : 0;
}

int report_progress(void *user, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
  auto *transfer = static_cast<Transfer *>(user);
  if (*transfer->progress &&
      !(*transfer->progress)(static_cast<std::uint64_t>(received), static_cast<std::uint64_t>(total))) {
    transfer->cancelled = true;
    return 1;
  }
  return 0;
}

void initialise_once() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace

HttpDownloadResult http_download(const std::string &url, std::ostream &out,
                                 const HttpProgress &progress) {
  initialise_once();
  HttpDownloadResult result;
  const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) {
    result.error = "Failed to initialize the HTTP client";
    return result;
  }
  Transfer transfer{&out, &progress, false};
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_body);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, report_progress);
  curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 0L);
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1000L);
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "ck-utilities/1.0");
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

  const CURLcode code = curl_easy_perform(curl.get());
  long status = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
  out.flush();

  if (transfer.cancelled) {
    result.cancelled = true;
    result.error = "Download cancelled";
  } else if (code != CURLE_OK) {
    const char *text = curl_easy_strerror(code);
    result.error = text ? text : "unknown HTTP client error";
  } else if (status != 200) {
    result.error = "HTTP error: " + std::to_string(status);
  } else if (!out) {
    result.error = "Failed to write the downloaded data";
  } else {
    result.ok = true;
  }
  return result;
}

} // namespace ck::ai::detail
