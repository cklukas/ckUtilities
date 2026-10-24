// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_app.hpp"
#include <atomic>
#include <stdexcept>
namespace ck::vision {
struct FilesAppWake::State : std::enable_shared_from_this<State> {
    ckv::ui::Application &application;
    std::atomic<bool> armed{false}, queued{false};
    std::function<void()> handler; // UI-thread only
    explicit State(ckv::ui::Application &app) : application(app) {}
    void request() {
        if (!armed.load(std::memory_order_acquire) || queued.exchange(true, std::memory_order_acq_rel))
            return;
        try {
            application.post([weak = weak_from_this()] {
                if (auto state = weak.lock()) {
                    state->queued.store(false, std::memory_order_release);
                    if (state->armed.load(std::memory_order_acquire) && state->handler)
                        state->handler();
                }
            });
        } catch (...) {
            queued.store(false, std::memory_order_release);
            throw;
        }
    }
};
FilesAppWake::FilesAppWake(ckv::ui::Application &application) : state_(std::make_shared<State>(application)) {}
FilesAppWake::~FilesAppWake() { unbind(); }
std::function<void()> FilesAppWake::callback() const {
    return [weak = std::weak_ptr<State>(state_)] {
        if (auto state = weak.lock())
            state->request();
    };
}
void FilesAppWake::request() const { state_->request(); }
void FilesAppWake::bind(std::function<void()> handler) {
    if (!handler || state_->armed.load())
        throw std::logic_error("file UI wake already bound or empty");
    state_->handler = std::move(handler);
    state_->armed.store(true, std::memory_order_release);
}
void FilesAppWake::unbind() noexcept {
    state_->armed.store(false, std::memory_order_release);
    state_->handler = {};
}
} // namespace ck::vision
