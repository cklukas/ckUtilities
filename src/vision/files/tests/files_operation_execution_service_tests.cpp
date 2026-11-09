#include "files_operation_execution_service.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <filesystem>
#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;
using namespace ck::vision;

NativeAbsolutePath address(const std::filesystem::path& path) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value())
        throw std::runtime_error("execution service fixture address unavailable");
    return std::move(value).take_value();
}
}

CK_TEST(operation_execution_service_rejects_missing_worker_handoffs_without_starting_native_work) {
    const ck::testing::ScratchDirectory scratch("files-operation-execution-service",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesOperationExecutionService service;
    FilesOperationExecutionRequest request{
        FileOperation::Copy,
        nullptr,
        nullptr,
        {},
        {},
        {},
        {},
        {},
        address(scratch.path()),
        {},
        {},
        {}};
    const auto submitted = service.request(std::move(request));
    CK_CHECK(submitted.state == FilesOperationExecutionSubmission::Invalid);
    CK_CHECK(!submitted.ticket.valid() && !service.working());
}

#if defined(__APPLE__)
#include "files_native_fixture_snapshot.hpp"
#include <chrono>
#include <fstream>
#include <thread>
CK_TEST(operation_execution_service_retains_first_mac_refusal_when_last_marked_item_is_unstarted) {
    const ck::testing::ScratchDirectory scratch("files-operation-first-failure", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    for (const auto* component : {"source", "target", "source-index", "target-index", "journals"}) {
        const auto path = scratch.path() / component;
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    }
    for (const auto* component : {"a.bin", "b.bin"}) {
        std::ofstream output(scratch.path() / "source" / component, std::ios::binary);
        output << component;
        CK_CHECK(output.good());
    }
    FilesPaneService source(native_files_pane_backend(address(scratch.path() / "source-index")));
    FilesPaneService target(native_files_pane_backend(address(scratch.path() / "target-index")));
    struct Drain final {
        FilesPaneService& source;
        FilesPaneService& target;
        ~Drain() { source.request_stop(); target.request_stop(); source.join(); target.join(); }
    } drain{source, target};
    const auto wait_pane = [](FilesPaneService& service) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto reply = service.take()) return std::move(*reply);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("native execution fixture pane did not drain");
    };
    const auto source_navigation = source.navigate(address(scratch.path() / "source"));
    const auto target_navigation = target.navigate(address(scratch.path() / "target"));
    CK_CHECK(source_navigation.state == FilesPaneSubmission::Accepted && target_navigation.state == FilesPaneSubmission::Accepted);
    const auto initial_source = wait_pane(source);
    const auto initial_target = wait_pane(target);
    CK_CHECK(initial_source.state == FilesPaneReplyState::Ready && initial_target.state == FilesPaneReplyState::Ready);
    const auto marking = source.change(source_navigation.ticket, initial_source.stamp,
        FilesPaneChangeMarks{IndexedMarkVisible{true}}, {0, 10});
    CK_CHECK(marking.state == FilesPaneSubmission::Accepted);
    const auto marked = wait_pane(source);
    CK_CHECK(marked.state == FilesPaneReplyState::Ready);
    const auto source_before = ck::testing::observe_files_fixture_tree(scratch.path() / "source");
    const auto target_before = ck::testing::observe_files_fixture_tree(scratch.path() / "target");
    FilesOperationExecutionService execution;
    FilesOperationExecutionRequest request{FileOperation::Copy, &source, &target,
        source_navigation.ticket, target_navigation.ticket, marked.stamp, initial_target.stamp,
        {}, address(scratch.path() / "journals"), {}, {}, {}};
    request.journal_id.bytes[0] = std::byte{0x6a};
    request.source_limits.access = EntryAccess::ContentRead;
    request.policy.allow_unhandled_metadata_loss = true;
    request.open_journal = [](const NativeAbsolutePath& root, const NativeName& name, bool create_new) {
        return open_native_operation_journal_file(root, name, create_new);
    };
    const auto submitted = execution.request(std::move(request));
    CK_CHECK(submitted.state == FilesOperationExecutionSubmission::Accepted);
    std::optional<FilesOperationExecutionReply> reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!reply && std::chrono::steady_clock::now() < deadline) {
        reply = execution.take();
        if (!reply) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    execution.request_stop(); execution.join();
    CK_CHECK(reply && reply->state == FilesOperationExecutionState::NeedsReconciliation);
    if (!reply) throw std::runtime_error("native execution fixture result missing");
    CK_CHECK(reply->result.items.size() == 2);
    if (reply->result.items.size() != 2) throw std::runtime_error("native execution fixture item shape changed");
    CK_CHECK(reply->result.items.front().outcome == OperationItemOutcome::NeedsReconciliation);
    CK_CHECK(reply->result.items.back().outcome == OperationItemOutcome::Pending && !reply->result.items.back().failure);
    CK_CHECK(reply->error && reply->result.items.front().failure &&
             reply->error->error == reply->result.items.front().failure->error);
    CK_CHECK(reply->error && reply->error->state == ObservationState::Unsupported);
    CK_CHECK(reply->error && reply->error->error.domain == ErrorDomain::Adapter);
    CK_CHECK(reply->error && reply->error->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));
    CK_CHECK(reply->error && reply->error->error.operation == "create_staging_file.fd_publication_unqualified");
    CK_CHECK(ck::testing::observe_files_fixture_tree(scratch.path() / "source") == source_before);
    CK_CHECK(ck::testing::observe_files_fixture_tree(scratch.path() / "target") == target_before);
    CK_CHECK(!execution.take());
}
#endif
