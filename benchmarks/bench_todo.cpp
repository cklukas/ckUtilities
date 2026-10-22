// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Focused TODO example benchmarks. Wall-clock numbers are diagnostic outside
// the named reference machine; deterministic work, size, and redraw budgets
// below are hard failures on every machine.
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "ckbench.hpp"
#include "cvision/term/headless_terminal.hpp"

#include "memory_todo_repository.hpp"
#include "todo_app.hpp"
#include "todo_codec.hpp"

namespace {

using namespace ckv;
using namespace ck::todo;

AuditStamp benchmark_stamp() {
    return {IsoTimestamp{"2026-08-25T12:00:00Z"}, "benchmark"};
}

TodoWorkspace thousand_task_workspace() {
    const auto guided = TodoWorkspace::guided(benchmark_stamp());
    TodoWorkspace workspace = guided ? *guided.value : TodoWorkspace::empty();
    for (int index = 3; index < 1'000; ++index) {
        TaskDraft task;
        task.title = "Benchmark task " + std::to_string(index + 1);
        task.details = "Stable details for refresh and rendering measurements";
        task.priority = static_cast<Priority>((index % 4) + 1);
        if (!workspace.add_task(LaneId{static_cast<std::uint64_t>((index % 3) + 1)},
                                std::move(task), benchmark_stamp()))
            break;
    }
    return workspace;
}

}  // namespace

// Usage: todo_bench [--gate], as cvision_bench.
int main(int argc, char** argv) {
    const std::optional<ckbench::Mode> mode = ckbench::mode_from_arguments(argc, argv);
    if (!mode) {
        std::fprintf(stderr, "usage: todo_bench [--gate]\n");
        return 2;
    }
    const ckbench::Runner bench(*mode);
    constexpr std::size_t kTaskCount = 1'000;
    constexpr std::size_t kEncodedBytesBudget = 1024U * 1024U;
    constexpr std::size_t kFrameCellsBudget = 140U * 40U;
    const TodoWorkspace workspace = thousand_task_workspace();
    const auto encoded = encode_workspace(workspace);
    if (!encoded) {
        std::fprintf(stderr, "todo benchmark: workspace encoding failed\n");
        return 1;
    }
    bool budgets_hold = workspace.snapshot().tasks.size() == kTaskCount &&
                        encoded.value->size() <= kEncodedBytesBudget;

    std::size_t sink = 0;
    bench.run("todo_codec_encode_1000", 100, [&] {
        const auto result = encode_workspace(workspace);
        if (!result || result.value->size() > kEncodedBytesBudget) {
            budgets_hold = false;
            return;
        }
        sink += result.value->size();
    });
    bench.run("todo_codec_decode_1000", 100, [&] {
        const auto result = decode_workspace(*encoded.value);
        if (!result || result.value->snapshot().tasks.size() != kTaskCount) {
            budgets_hold = false;
            return;
        }
        sink += result.value->snapshot().tasks.size();
    });

    MemoryTodoRepository repository(workspace);
    bench.run("todo_repository_load_1000", 1'000, [&] {
        const auto result = repository.load();
        if (!result || result.value->workspace.snapshot().tasks.size() != kTaskCount) {
            budgets_hold = false;
            return;
        }
        sink += result.value->workspace.snapshot().tasks.size();
    });

    ckv::term::HeadlessTerminal terminal(Size{140, 40}, ckv::term::headless_no_graphics_profile());
    ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    FixedCalendarClock calendar(
        {IsoTimestamp{"2026-08-25T12:00:00Z"}, IsoDate{"2026-08-25"}, IsoTime{"14:30"}});
    TodoApp todo(app, repository, calendar, "benchmark",
                 {.workspace_description = "in-memory benchmark workspace"});
    app.step(0);

    bench.run("todo_refresh_1000_tasks", 50, [&] {
        todo.refresh_board();
        std::size_t visible_tasks = 0;
        for (const Lane& lane : todo.controller().workspace()->find_board(BoardId{1})->lanes)
            visible_tasks += lane.task_ids.size();
        if (todo.board_view()->lane_count() != 3U || visible_tasks != kTaskCount)
            budgets_hold = false;
        sink += todo.board_view()->lane_count();
    });
    bench.run("todo_steady_board_render", 250, [&] {
        app.root().invalidate();
        terminal.clear_written();
        app.step(monotonic.now_nanos());
        if (app.last_compose_cells_touched() > kFrameCellsBudget ||
            !terminal.written_bytes().empty())
            budgets_hold = false;
        sink += app.composed_surface().size().width;
    });

    bool in_second_lane = false;
    bench.run("todo_lane_move_commit", 100, [&] {
        const auto pending = todo.controller().begin_task_move(TaskId{1});
        const LaneId target = in_second_lane ? LaneId{1} : LaneId{2};
        if (!pending) {
            budgets_hold = false;
            return;
        }
        const auto staged = todo.controller().stage_task_move(*pending.value, target, std::nullopt);
        if (!staged) {
            budgets_hold = false;
            return;
        }
        const auto committed = todo.controller().commit_task_move(*staged.value);
        if (!committed || !committed.value->changed) {
            budgets_hold = false;
            return;
        }
        in_second_lane = !in_second_lane;
        sink += committed.value->changed ? 1U : 0U;
    });

    bench.run("todo_unchanged_revision_poll", 5'000, [&] {
        terminal.clear_written();
        todo.poll_repository();
        app.step(monotonic.now_nanos());
        if (!terminal.written_bytes().empty()) budgets_hold = false;
        sink += todo.controller().workspace() != nullptr ? 1U : 0U;
    });

    if (todo.controller().workspace()->lane_of(TaskId{1}) != LaneId{1}) budgets_hold = false;
    std::printf("budgets encoded=%zu/%zu bytes frame=%zu/%zu cells idle-output=0 bytes\n",
                encoded.value->size(), kEncodedBytesBudget, app.last_compose_cells_touched(),
                kFrameCellsBudget);
    std::printf("checksum %zu\n", sink);
    if (!budgets_hold)
        std::fputs("todo benchmark budget failure: a deterministic bound or operation contract regressed\n",
                   stderr);
    return budgets_hold && sink != 0 ? 0 : 1;
}
