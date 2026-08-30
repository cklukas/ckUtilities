#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ck/find/search_model.hpp"

namespace ck::vision
{

// A saved action value identifies one fixed native template; it is never a
// shell command line. The plan exposes the direct argv form used for both the
// preview and the sandboxed process invocation.
struct FindCustomCommandPlan
{
    bool allowed = false;
    std::string reason;
    std::filesystem::path working_directory;
    std::vector<std::string> executable_argv;
};

FindCustomCommandPlan planFindCustomCommand(const ck::find::SearchSpecification &specification);

struct FindCustomCommandCapability
{
    bool available = false;
    std::string reason;
    // Includes the sandbox launcher and profile as direct argv values. The
    // final `<matched-path>` token is replaced by one literal search result.
    std::vector<std::string> argv_preview;
};

struct FindCustomCommandOutcome
{
    bool invoked = false;
    bool cancelled = false;
    bool timed_out = false;
    bool output_truncated = false;
    int exit_code = -1;
    std::string output;
    std::string message;
};

// Process execution is deliberately isolated behind this narrow application
// service. The worker owns search traversal while this service owns platform
// policy, sandboxing, direct argv construction, timeout, and bounded output.
class FindCustomCommandExecutor
{
public:
    virtual ~FindCustomCommandExecutor() = default;

    virtual FindCustomCommandCapability capability(const ck::find::SearchSpecification &specification) const = 0;
    virtual FindCustomCommandOutcome execute(const FindCustomCommandPlan &plan,
                                             const std::filesystem::path &matched_path,
                                             const std::atomic_bool &cancellation) = 0;
};

// The production factory returns a macOS Seatbelt (`sandbox-exec`) adapter on
// supported hosts. Other hosts return an explicit unavailable implementation;
// there is no weaker fallback execution path.
std::unique_ptr<FindCustomCommandExecutor> makePlatformFindCustomCommandExecutor();

} // namespace ck::vision
