#pragma once

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <map>

using json = nlohmann::json;

struct Node
{
    const json *value = nullptr;
    Node *parent = nullptr;
    std::vector<std::unique_ptr<Node>> children;
    std::string key;
    bool expanded = false;
    bool isDummyRoot = false;
    bool isLastChild = false;
};

struct SearchState
{
    std::string term;
    bool searchKeys = true;
    bool searchValues = false;
    std::vector<const Node *> matches;
    int currentIndex = 0;
};

extern std::map<std::string, size_t> fileSizes;

int getDisplayWidth(const std::string &str);
std::unique_ptr<Node> buildTree(const json *j, const std::string &key, Node *parent, bool dummy);
void collectVisible(const Node *node, std::vector<const Node *> &out);
std::string buildPrefix(const Node *node);
std::string shortenPath(const std::string &path, int maxWidth);
std::string getTypeIcon(const Node *node);
std::string getContentLabel(const Node *node, int maxWidth = 80);
std::string getContentLabelWithSearch(const Node *node, const SearchState &search, int maxWidth = 80);
void expandAll(Node *node);
void collapseAll(Node *node, bool keepRoot);
void expandToLevel(Node *node, int targetLevel, int currentLevel = 0);
void expandPath(Node *node);
void searchTree(const Node *node, const std::string &term, bool searchKeys, bool searchValues, std::vector<const Node *> &out);
bool osc52Likely();
std::string getClipboardStatusMessage();
void copyToClipboard(const std::string &text);
json reconstructJson(const Node *node);
std::string formatFileSize(size_t size);
void printFormattedJson(const json &j, int indent = 0);
json parseJsonWithSpecialNumbers(const std::string &contents);

