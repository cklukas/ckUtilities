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
