#pragma once

#include "ck/find/search_model.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ck::find
{

struct SavedSpecification
{
    std::string name;
    std::string slug;
    std::filesystem::path path;
};

struct SearchExecutionOptions
{
    bool includeActions = true;
    bool captureMatches = false;
    bool filterContent = true;
    // Zero keeps the historical unbounded capture behavior.  Interactive
    // callers should set a finite value and use SearchExecutionResult::matchCount
    // for the complete count.
    std::size_t maxCapturedMatches = 0;

    // The search core deliberately owns no worker or UI lifetime.  A caller
    // can provide this inexpensive polling probe to stop a long traversal at
    // deterministic traversal boundaries.
    std::function<bool()> cancellation_requested;

    // Called for every accepted path on the executing thread.  UI adapters
    // must marshal this callback before updating a view.
    std::function<void(const std::filesystem::path &)> on_match;
};

struct SearchExecutionResult
{
    int exitCode = 0;
    bool cancelled = false;
    std::size_t matchCount = 0;
    // Application adapters that apply an explicitly authorized file action
    // report its outcome separately from matching and traversal errors.
    std::size_t deletedCount = 0;
    std::size_t failedDeletionCount = 0;
    std::vector<std::filesystem::path> matches;
    std::vector<std::string> command;
};

std::filesystem::path specificationStorageDirectory();

std::vector<SavedSpecification> listSavedSpecifications();
std::optional<SearchSpecification> loadSpecification(const std::string &nameOrSlug);
bool saveSpecification(const SearchSpecification &spec);
bool saveSpecification(const SearchSpecification &spec, const std::string &name);
bool removeSpecification(const std::string &nameOrSlug);

std::string normaliseSpecificationName(const std::string &name);

std::vector<std::string> buildFindCommand(const SearchSpecification &spec, bool includeActions = true);
SearchExecutionResult executeSpecification(const SearchSpecification &spec,
                                           const SearchExecutionOptions &options = {},
                                           std::ostream *forwardStdout = nullptr,
                                           std::ostream *forwardStderr = nullptr);

} // namespace ck::find
