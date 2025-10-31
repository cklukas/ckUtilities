#include "ck/find/search_backend.hpp"
#include "ck/find/search_model.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <vector>

TEST(SearchBackend, BuildsDefaultFindCommand)
{
    auto spec = ck::find::makeDefaultSpecification();
    auto command = ck::find::buildFindCommand(spec);
    std::vector<std::string> expected = {"find", ".", "-P", "!", "-name", ".*", "!", "-path", "*/.*", "-print"};
    EXPECT_EQ(command, expected);
}

TEST(SearchBackend, HiddenFilterRespectsConfiguration)
{
    auto spec = ck::find::makeDefaultSpecification();
    spec.includeHidden = true;
    auto command = ck::find::buildFindCommand(spec);
    std::vector<std::string> hidden = {"!", "-name", ".*"};
    EXPECT_EQ(std::search(command.begin(), command.end(), hidden.begin(), hidden.end()), command.end());

    spec.includeHidden = false;
    auto filtered = ck::find::buildFindCommand(spec);
    EXPECT_NE(std::search(filtered.begin(), filtered.end(), hidden.begin(), hidden.end()), filtered.end());
}

TEST(SearchBackend, NormalisesSpecificationNames)
{
    EXPECT_EQ(ck::find::normaliseSpecificationName("  Example Spec  "), "Example Spec");
    EXPECT_EQ(ck::find::normaliseSpecificationName("\t\n"), "");
}
