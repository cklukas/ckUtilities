// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor_service.hpp"
#include <algorithm>
#include <exception>
#include <utility>

namespace ck::sysmon {
MonitorService::MonitorService(ckv::ui::Application& app, MonitorSource& source, std::weak_ptr<void> lifetime, Handler handler)
    : app_(app), source_(source), lifetime_(std::move(lifetime)), handler_(std::move(handler)) {
    try {
        workers_.emplace_back([this] { work(true); });
        for (int i = 0; i < 2; ++i) workers_.emplace_back([this] { work(false); });
    } catch (...) {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        ready_.notify_all();
        for (auto& worker : workers_) worker.join();
        throw;
    }
}
MonitorService::~MonitorService() {
    { std::lock_guard lock(mutex_); stopping_ = true; pending_.clear(); }
    ready_.notify_all();
    for (auto& worker : workers_) worker.join();
}
void MonitorService::request(std::string endpoint) {
    { std::lock_guard lock(mutex_);
      if (stopping_ || active_.contains(endpoint)) return;
      active_.insert(endpoint); pending_.push_back(std::move(endpoint)); }
    ready_.notify_all();
}
void MonitorService::wait_idle() {
    std::unique_lock lock(mutex_); idle_.wait(lock, [this] { return active_.empty(); });
}
void MonitorService::wait_idle(const std::string& endpoint) {
    std::unique_lock lock(mutex_); idle_.wait(lock, [this, &endpoint] { return !active_.contains(endpoint); });
}
void MonitorService::work(bool local) {
    for (;;) {
        std::string endpoint;
        {
            std::unique_lock lock(mutex_);
            const auto matches = [local](const auto& host) { return host.empty() == local; };
            ready_.wait(lock, [&] { return stopping_ || std::any_of(pending_.begin(), pending_.end(), matches); });
            if (stopping_) return;
            auto job = std::find_if(pending_.begin(), pending_.end(), matches);
            endpoint = std::move(*job); pending_.erase(job);
        }
        Sample sample;
        try { sample = source_.sample(endpoint); }
        catch (const std::exception& error) { sample.diagnostic = error.what(); }
        catch (...) { sample.diagnostic = "Metric collection failed"; }
        app_.post([lifetime = lifetime_, handler = handler_, host = endpoint, result = std::move(sample)]() mutable {
            if (!lifetime.expired()) handler(std::move(host), std::move(result));
        });
        { std::lock_guard lock(mutex_); active_.erase(endpoint); }
        idle_.notify_all();
    }
}
} // namespace ck::sysmon
