#pragma once

#include "ck/find/search_model.hpp"

#include <filesystem>
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
};

struct SearchExecutionResult
{
    int exitCode = 0;
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
