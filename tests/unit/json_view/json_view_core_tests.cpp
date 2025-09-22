#include <gtest/gtest.h>

#include "json_view_core.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace
{

json makeSampleJson()
{
    return json::parse(R"({"name":"sample","numbers":[1,2,3],"nested":{"flag":true}})");
}

const Node *findChildByKey(const Node *parent, const std::string &key)
{
    for (const auto &child : parent->children)
    {
        if (child->key == key)
            return child.get();
    }
    return nullptr;
}

} // namespace

TEST(JsonViewCore, BuildsTreeWithVisibleNodes)
{
    json data = makeSampleJson();
    auto root = buildTree(&data, "", nullptr, true);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->children.size(), 3u);

    std::vector<const Node *> visible;
    collectVisible(root.get(), visible);
    ASSERT_GE(visible.size(), 4u);
    const Node *numbers = findChildByKey(root.get(), "numbers");
    ASSERT_NE(numbers, nullptr);
    ASSERT_FALSE(numbers->children.empty());
    EXPECT_EQ(numbers->children.front()->key, "[0]");

    std::string prefix = buildPrefix(numbers->children.back().get());
    EXPECT_FALSE(prefix.empty());
    EXPECT_NE(prefix.find("└"), std::string::npos);
}

TEST(JsonViewCore, ShortensLongPaths)
{
    std::string path = "/very/long/path/segment/file.json";
    std::string shortened = shortenPath(path, 16);
    EXPECT_LE(getDisplayWidth(shortened), 16);
    EXPECT_NE(shortened.find("..."), std::string::npos);
}

TEST(JsonViewCore, FormatsFileSizes)
{
    EXPECT_EQ(formatFileSize(512), "512 Bytes");
    EXPECT_EQ(formatFileSize(1536), "1.5 KB");
    EXPECT_EQ(formatFileSize(5ull * 1024ull * 1024ull), "5.0 MB");
}

TEST(JsonViewCore, ParsesSpecialFloatingPointLiterals)
{
    const std::string input = R"({"value": NaN, "inf": Infinity, "neg": -Infinity, "arr": [NaN]})";
    json parsed = parseJsonWithSpecialNumbers(input);

    ASSERT_TRUE(parsed.is_object());
    EXPECT_TRUE(std::isnan(parsed.at("value").get<double>()));
    EXPECT_TRUE(std::isinf(parsed.at("inf").get<double>()));
    EXPECT_TRUE(std::isinf(parsed.at("neg").get<double>()));
    EXPECT_LT(parsed.at("neg").get<double>(), 0);

    ASSERT_TRUE(parsed.at("arr").is_array());
    EXPECT_TRUE(std::isnan(parsed.at("arr")[0].get<double>()));
}
