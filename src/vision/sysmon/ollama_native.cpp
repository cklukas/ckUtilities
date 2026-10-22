// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_monitor.hpp"
#include <chrono>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace ck::sysmon {
namespace {
std::string get_models() {
    std::string response;
#if defined(_WIN32)
    const auto session = ::WinHttpOpen(L"ck-sysmon/1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
    if (!session) return {};
    ::WinHttpSetTimeouts(session, 500, 500, 500, 500);
    const auto connection = ::WinHttpConnect(session, L"127.0.0.1", 11434, 0);
    const auto request = connection ? ::WinHttpOpenRequest(connection, L"GET", L"/api/ps", nullptr, nullptr, nullptr, 0) : nullptr;
    if (request && ::WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) && ::WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr) && status == 200) {
            char buffer[4096]; DWORD count = 0;
            while (::WinHttpReadData(request, buffer, sizeof(buffer), &count) && count) {
                response.append(buffer, count);
                if (response.size() > 32768) { response.clear(); break; }
            }
        }
    }
    if (request) ::WinHttpCloseHandle(request);
    if (connection) ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
#else
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket < 0) return {};
    timeval timeout{0, 500000};
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(__APPLE__)
    const int no_sigpipe = 1; ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(11434); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        constexpr std::string_view request = "GET /api/ps HTTP/1.0\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        if (::send(socket, request.data(), request.size(), flags) == static_cast<ssize_t>(request.size())) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            char buffer[4096];
            while (std::chrono::steady_clock::now() < deadline) {
                const auto count = ::recv(socket, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                response.append(buffer, static_cast<std::size_t>(count));
                if (response.size() > 36864) { response.clear(); break; }
            }
        }
    }
    ::close(socket);
    const auto header = response.find("\r\n\r\n");
    if (header == response.npos || (!response.starts_with("HTTP/1.0 200 ") && !response.starts_with("HTTP/1.1 200 "))) return {};
    response.erase(0, header + 4);
#endif
    return response;
}
}
void add_ollama(Sample& sample) {
    const auto response = get_models();
    if (response.empty()) return;
    auto models = parse_ollama(response);
    if (!models) { sample.ollama_status = "invalid API response"; return; }
    sample.ollama_status = "running";
    sample.models = std::move(*models);
}
} // namespace ck::sysmon
