// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// http_download on WinHTTP (Windows). Derived from Microsoft's WinHTTP API
// reference: WinHttpOpen, WinHttpCrackUrl, WinHttpConnect,
// WinHttpOpenRequest, WinHttpSetTimeouts, WinHttpSendRequest,
// WinHttpReceiveResponse, WinHttpQueryHeaders, WinHttpQueryDataAvailable and
// WinHttpReadData. WinHTTP follows redirects (except HTTPS to HTTP) and
// verifies the certificate chain and host name by default.
#include "http_download.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <string>
#include <vector>

namespace ck::ai::detail {

namespace {

struct InternetCloser {
  void operator()(HINTERNET handle) const noexcept { WinHttpCloseHandle(handle); }
};
using Handle = std::unique_ptr<void, InternetCloser>;

std::wstring widen(const std::string &text) {
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
  if (count <= 0)
    return {};
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                      wide.data(), count);
  return wide;
}

// The system's text for a WinHTTP or Win32 error code.
std::string describe(DWORD code) {
  wchar_t *buffer = nullptr;
  const HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                      (winhttp ? FORMAT_MESSAGE_FROM_HMODULE : 0) | FORMAT_MESSAGE_FROM_SYSTEM;
  const DWORD length = FormatMessageW(flags, winhttp, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
  std::string text;
  if (length > 0 && buffer) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    text.resize(static_cast<std::size_t>(bytes > 0 ? bytes : 0));
    if (bytes > 0)
      WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), text.data(), bytes, nullptr, nullptr);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
  }
  if (buffer)
    LocalFree(buffer);
  return text.empty() ? "Windows HTTP error " + std::to_string(code) : text;
}

// Reads the failing call's error code first, before anything can replace it.
HttpDownloadResult failure(const char *what) {
  const DWORD code = GetLastError();
  HttpDownloadResult result;
  result.error = std::string(what) + ": " + describe(code);
  return result;
}

} // namespace

HttpDownloadResult http_download(const std::string &url, std::ostream &out,
                                 const HttpProgress &progress) {
  HttpDownloadResult result;
  std::wstring wide_url = widen(url);
  if (wide_url.empty()) {
    result.error = "Invalid download address";
    return result;
  }

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts))
    return failure("Invalid download address");
  if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) {
    result.error = "Unsupported download address";
    return result;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.lpszExtraInfo)
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  if (path.empty())
    path = L"/";

  const Handle session(WinHttpOpen(L"ck-utilities/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session)
    return failure("Failed to initialize the HTTP client");
  // Resolve, connect and send within 30 s; a response or data gap of 60 s fails.
  if (!WinHttpSetTimeouts(session.get(), 30000, 30000, 30000, 60000))
    return failure("Failed to configure the HTTP client");
  const Handle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
  if (!connection)
    return failure("Could not connect to the download server");
  const DWORD secure = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  const Handle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure));
  if (!request)
    return failure("Could not create the download request");
  if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr))
    return failure("Download request failed");

  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
    return failure("Download response was unreadable");
  if (status != 200) {
    result.error = "HTTP error: " + std::to_string(status);
    return result;
  }

  // Content-Length as text: the numeric query flag is 32-bit, models are larger.
  std::uint64_t total = 0;
  wchar_t length_text[32] = {};
  DWORD length_size = sizeof(length_text);
  if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                          length_text, &length_size, WINHTTP_NO_HEADER_INDEX))
    total = std::wcstoull(length_text, nullptr, 10);

  std::uint64_t received = 0;
  std::vector<char> buffer(1 << 16);
  if (progress && !progress(0, total)) {
    result.cancelled = true;
    result.error = "Download cancelled";
    return result;
  }
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available))
      return failure("Download interrupted");
    if (available == 0)
      break;
    while (available > 0) {
      const DWORD chunk = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
      DWORD read = 0;
      if (!WinHttpReadData(request.get(), buffer.data(), chunk, &read))
        return failure("Download interrupted");
      if (read == 0)
        break;
      out.write(buffer.data(), static_cast<std::streamsize>(read));
      if (!out) {
        result.error = "Failed to write the downloaded data";
        return result;
      }
      received += read;
      available -= read;
      if (progress && !progress(received, total)) {
        result.cancelled = true;
        result.error = "Download cancelled";
        return result;
      }
    }
  }
  out.flush();
  if (!out) {
    result.error = "Failed to write the downloaded data";
    return result;
  }
  if (total != 0 && received != total) {
    result.error = "Download ended early";
    return result;
  }
  result.ok = true;
  return result;
}

} // namespace ck::ai::detail
