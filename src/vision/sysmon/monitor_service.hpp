// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "monitor.hpp"
#include "cvision/ui/application.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace ck::sysmon {
class MonitorService {
public:
    using Handler = std::function<void(std::string, Sample)>;
    MonitorService(ckv::ui::Application& app, MonitorSource& source, std::weak_ptr<void> lifetime, Handler handler);
    ~MonitorService();
    MonitorService(const MonitorService&) = delete;
    MonitorService& operator=(const MonitorService&) = delete;
    void request(std::string endpoint);
    void wait_idle();
    void wait_idle(const std::string& endpoint);
private:
    void work(bool local);
    ckv::ui::Application& app_;
    MonitorSource& source_;
    std::weak_ptr<void> lifetime_;
    Handler handler_;
    std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::deque<std::string> pending_;
    std::set<std::string> active_;
    bool stopping_ = false;
    // A dedicated local worker and two remote workers. Remote timeouts cannot
    // steal the local refresh slot or queue unbounded repeated requests.
    std::vector<std::thread> workers_;
};
} // namespace ck::sysmon
