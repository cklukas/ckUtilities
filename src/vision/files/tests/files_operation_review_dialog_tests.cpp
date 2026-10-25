// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_review_dialog.hpp"

#include "ck/vision/suite_shell.hpp"
#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/testing/cktest.hpp>
#include <cvision/widgets/button.hpp>
#include <cvision/widgets/combo_box.hpp>
#include <cvision/widgets/input_line.hpp>

#include <array>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace ck::files;
using namespace ck::vision;

template <class T>
T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("operation review dialog fixture unavailable");
    return std::move(value).take_value();
}

FrozenOperationItem item() {
    return {ObjectIdentity{AdapterScope{}, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}}, ObjectKind::RegularFile,
        SourceTargetTopology::Disjoint, std::nullopt};
}

OperationReviewModel::Pointer model_with_conflict() {
    return take(OperationReviewModel::create(take(OperationPlan::create(FileOperation::Copy, {item()}, {},
        {{0, DestinationConflictKind::ExistingFile}}))));
}

template <class T>
void find_views(ckv::ui::View &root, std::vector<T *> &found) {
    if (auto *view = dynamic_cast<T *>(&root))
        found.push_back(view);
    for (const auto &child : root.children())
        find_views(*child, found);
}
} // namespace

CK_TEST(files_operation_review_dialog_escape_returns_plan_bound_cancel) {
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34});
    ckv::ui::Application application(terminal, clock);
    ck::vision::SuiteShell shell(application, {.application_name = "Files"});
    std::optional<ConsentReceipt> receipt;
    FilesOperationReviewDialog dialog(application, shell.desktop(), shell.roles(), model_with_conflict(),
        NameEncoding::PosixBytes, [&](Observation<ConsentReceipt> result) {
            if (result.value())
                receipt = std::move(result).take_value();
        });
    application.step(clock.now_nanos());
    CK_CHECK(!dialog.completed());
    CK_CHECK(application.dispatch(ckv::KeyEvent{{ckv::Key::Escape, ckv::Modifier::None, {}}}));
    application.step(clock.now_nanos());
    application.step(clock.now_nanos());
    CK_CHECK(dialog.completed() && receipt && receipt->action == ConsentAction::Cancel && receipt->plan.is_valid());
}

CK_TEST(files_operation_review_dialog_binds_reviewed_keep_both_native_name) {
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34});
    ckv::ui::Application application(terminal, clock);
    ck::vision::SuiteShell shell(application, {.application_name = "Files"});
    std::optional<ConsentReceipt> receipt;
    FilesOperationReviewDialog dialog(application, shell.desktop(), shell.roles(), model_with_conflict(),
        NameEncoding::PosixBytes, [&](Observation<ConsentReceipt> result) {
            if (result.value())
                receipt = std::move(result).take_value();
        });
    application.step(clock.now_nanos());
    std::vector<ckv::widgets::ComboBox *> combos;
    std::vector<ckv::widgets::InputLine *> inputs;
    std::vector<ckv::widgets::Button *> buttons;
    find_views(application.root(), combos);
    find_views(application.root(), inputs);
    find_views(application.root(), buttons);
    CK_CHECK(combos.size() == 1 && inputs.size() >= 1);
    ckv::widgets::InputLine *name = nullptr;
    for (auto *input : inputs) {
        bool inside_combo = false;
        for (auto *parent = input->parent(); parent; parent = parent->parent())
            inside_combo = inside_combo || dynamic_cast<ckv::widgets::ComboBox *>(parent) != nullptr;
        if (!inside_combo)
            name = input;
    }
    ckv::widgets::Button *apply = nullptr;
    for (auto *button : buttons)
        if (button->text() == "&Apply decision")
            apply = button;
    CK_CHECK(apply != nullptr);
    CK_CHECK(name != nullptr);
    name->set_text("copy");
    combos.front()->set_selected_index(3); // Cancel, choose destination, skip, Keep Both.
    const auto bounds = apply->absolute_bounds();
    CK_CHECK(application.dispatch(ckv::MouseEvent{.action = ckv::MouseAction::Down,
        .button = ckv::MouseButton::Left, .cell = {bounds.x, bounds.y}}));
    CK_CHECK(application.dispatch(ckv::MouseEvent{.action = ckv::MouseAction::Up,
        .button = ckv::MouseButton::Left, .cell = {bounds.x, bounds.y}}));
    application.step(clock.now_nanos());
    application.step(clock.now_nanos());
    CK_CHECK(dialog.completed() && receipt && receipt->action == ConsentAction::KeepBoth &&
        receipt->keep_both_names.size() == 1 && receipt->keep_both_names.front().display_name() == "copy");
}
