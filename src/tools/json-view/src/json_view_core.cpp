// Core functionality for json-view shared between different frontends
#include "json_view_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <deque>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>
#include <wchar.h>

std::map<std::string, size_t> fileSizes;

// Calculate the display width of a UTF-8 string (handles Unicode properly)
int getDisplayWidth(const std::string &str)
{
    // Convert UTF-8 string to wide character string
    size_t len = str.length();
    wchar_t *wstr = new wchar_t[len + 1];

    // Convert UTF-8 to wide characters
    size_t result = mbstowcs(wstr, str.c_str(), len);
    if (result == (size_t)-1)
    {
        // Conversion failed, fall back to byte length
        delete[] wstr;
        return str.length();
    }
    wstr[result] = L'\0';

    // Get display width
    int width = wcswidth(wstr, result);
    delete[] wstr;

    // wcswidth returns -1 for unprintable characters, fall back to byte length
    return (width >= 0) ? width : str.length();
}

// Build a tree of Node objects mirroring the structure of a JSON
// document.  Each Node stores a pointer to the original JSON value.
std::unique_ptr<Node> buildTree(const json *j, const std::string &key,
                                       Node *parent, bool dummy)
{
    auto node = std::make_unique<Node>();
    node->value = j;
    node->parent = parent;
    node->key = key;
    node->isDummyRoot = dummy;
    // Root nodes are expanded by default so the top‑level structure is visible.
    node->expanded = dummy;
    // Recursively create children for objects and arrays.  Primitive
    // values have no children.
    if (j->is_object())
    {
        node->children.reserve(j->size());
        for (auto it = j->begin(); it != j->end(); ++it)
        {
            const std::string &childKey = it.key();
            const json &childVal = it.value();
            auto child = buildTree(&childVal, childKey, node.get(), false);
            node->children.push_back(std::move(child));
        }
    }
    else if (j->is_array())
    {
        node->children.reserve(j->size());
        int idx = 0;
        for (auto it = j->begin(); it != j->end(); ++it, ++idx)
        {
            std::string childKey = "[" + std::to_string(idx) + "]";
            auto child = buildTree(&(*it), childKey, node.get(), false);
            node->children.push_back(std::move(child));
        }
    }
    // Mark last child in the list.  This information is used to draw
    // the tree branches correctly.
    for (size_t i = 0; i < node->children.size(); ++i)
    {
        node->children[i]->isLastChild = (i == node->children.size() - 1);
    }
    return node;
}

// Recursively collect all nodes that are currently visible.  A node is
// visible if it is a root or its parent is expanded.
void collectVisible(const Node *node, std::vector<const Node *> &out)
{
    out.push_back(node);
    if (node->expanded)
    {
        for (const auto &child : node->children)
        {
            collectVisible(child.get(), out);
        }
    }
}

// Build the tree prefix for a node.  This string contains the
// vertical bar and branch characters needed to draw a proper tree.
std::string buildPrefix(const Node *node)
{
    std::string prefix;
    const Node *cur = node;
    while (cur->parent != nullptr)
    {
        const Node *parent = cur->parent;
        // skip the dummy root (it has no parent) so its isLastChild flag
        // doesn’t affect vertical lines for deeper levels
        if (parent->parent != nullptr)
        {
            if (!parent->isLastChild)
            {
                prefix = std::string("│   ") + prefix;
            }
            else
            {
                prefix = std::string("    ") + prefix;
            }
        }
        cur = parent;
    }
    if (node->parent != nullptr)
    {
        prefix += node->isLastChild ? "└── " : "├── ";
    }
    return prefix;
}

// Shorten a file path to fit within maxWidth by replacing middle parts with "..."
std::string shortenPath(const std::string &path, int maxWidth)
{
    if (getDisplayWidth(path) <= maxWidth)
    {
        return path;
    }

    // Find the filename (last component)
    size_t lastSlash = path.find_last_of('/');
    if (lastSlash == std::string::npos)
    {
        // No slash found, just truncate the string
        return path.substr(0, maxWidth - 3) + "...";
    }

    std::string filename = path.substr(lastSlash + 1);
    std::string directory = path.substr(0, lastSlash);

    // If filename alone is too long, truncate it
    if (getDisplayWidth(filename) > maxWidth - 4)
    { // -4 for ".../"
        return ".../" + filename.substr(0, maxWidth - 7) + "...";
    }

    // Calculate remaining space for directory
    int remainingSpace = maxWidth - getDisplayWidth(filename) - 1; // -1 for the slash

    if (getDisplayWidth(directory) <= remainingSpace)
    {
        return path; // Original fits
    }

    // Need to shorten directory part
    // Strategy: keep start and end of path, replace middle with "..."
    int prefixLen = std::max(1, remainingSpace / 3);
    int suffixLen = std::max(1, remainingSpace - prefixLen - 3); // -3 for "..."

    if (prefixLen + suffixLen + 3 >= getDisplayWidth(directory))
    {
        return path; // Would not actually shorten
    }

    std::string prefix = directory.substr(0, prefixLen);
    std::string suffix = directory.substr(directory.length() - suffixLen);

    return prefix + "..." + suffix + "/" + filename;
}

// Get the type icon for a node
std::string getTypeIcon(const Node *node)
{
    const json *v = node->value;
    if (node->isDummyRoot)
    {
        return ""; // No icons for dummy roots
    }

    if (v->is_string())
        return "℀ ";
    else if (v->is_boolean())
        return v->get<bool>() ? "☒ " : "☐ ";
    else if (v->is_number())
        return "⅑ ";
    else if (v->is_null())
        return "⊘ ";
    else if (v->is_object() && v->empty())
        return "⁞ "; // Only for empty dictionaries
    else if (v->is_array())
        return ""; // No icon for arrays
    else if (v->is_object())
        return ""; // No icon for non-empty objects
    return "";
}

// Get the content without type icon
std::string getContentLabel(const Node *node, int maxWidth)
{
    const json *v = node->value;
    if (node->isDummyRoot)
    {
        std::string type;
        if (v->is_object())
        {
            size_t count = v->size();
            type = "📦 dictionary, " + std::to_string(count) + (count == 1 ? " key" : " keys");
        }
        else if (v->is_array())
        {
            size_t count = v->size();
            type = "🗂️ list, " + std::to_string(count) + (count == 1 ? " item" : " items");
        }
        else if (v->is_string())
            type = "℀ string";
        else if (v->is_number())
            type = "⅑ number";
        else if (v->is_boolean())
            type = "☒ boolean";
        else if (v->is_null())
            type = "⊘ null";
        else
            type = "📄 value";

        // Add file size information from stored file sizes
        auto it = fileSizes.find(node->key);
        if (it != fileSizes.end())
        {
            std::string fileSizeStr = formatFileSize(it->second);
            type += ", " + fileSizeStr;
        }

        // Shorten the filename for display
        std::string shortKey = shortenPath(node->key, maxWidth - getDisplayWidth(type) - 4); // -4 for " ()"
        return shortKey + " (" + type + ")";
    }

    // For objects and arrays, no icons for expandable items
    if (v->is_object())
    {
        size_t count = v->size();
        return node->key + " (dictionary, " + std::to_string(count) + (count == 1 ? " key)" : " keys)");
    }
    else if (v->is_array())
    {
        size_t count = v->size();
        std::string baseLabel = node->key + " (list, " + std::to_string(count) + (count == 1 ? " item)" : " items)");

        // Array previews are rendered directly in drawLine; return base label only
        return baseLabel;
    }
    else if (v->is_string())
    {
        // Escape quotes in string for readability
        std::string s = v->get<std::string>();
        // Replace control characters with escapes
        std::string out;
        for (char c : s)
        {
            if (c == '\\')
                out += "\\\\";
            else if (c == '\"')
                out += "\\\"";
            else if (c == '\n')
                out += "\\n";
            else if (c == '\r')
                out += "\\r";
            else if (c == '\t')
                out += "\\t";
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                out += buf;
            }
            else
                out += c;
        }
        return node->key + ": \"" + out + "\"";
    }
    else if (v->is_boolean())
    {
        return node->key + ": " + std::string(v->get<bool>() ? "true" : "false");
    }
    else if (v->is_number())
    {
        return node->key + ": " + v->dump();
    }
    else if (v->is_null())
    {
        return node->key + ": null";
    }
    else
    {
        // Fallback for other types
        return node->key;
    }
}

// Get the content label with search match information
std::string getContentLabelWithSearch(const Node *node, const SearchState &search, int maxWidth)
{
    // For non-dummy root nodes, just use the original function
    if (!node->isDummyRoot)
    {
        return getContentLabel(node, maxWidth);
    }

    // For dummy root nodes, add search match count if search is active
    std::string baseLabel = getContentLabel(node, maxWidth);

    if (!search.term.empty() && !search.matches.empty())
    {
        // Count matches that belong to this dummy root's subtree
        int matchCount = 0;
        for (const Node *match : search.matches)
        {
            // Walk up the tree to find the root
            const Node *current = match;
            while (current->parent != nullptr)
            {
                current = current->parent;
            }
            if (current == node)
            {
                matchCount++;
            }
        }

        if (matchCount > 0)
        {
            // Find the closing parenthesis in the base label
            size_t closeParenPos = baseLabel.rfind(')');
            if (closeParenPos != std::string::npos)
            {
                // Insert the match count before the closing parenthesis
                std::string matchInfo = ", 🔍 " + std::to_string(matchCount) + (matchCount == 1 ? " match" : " matches");
                baseLabel.insert(closeParenPos, matchInfo);
            }
        }
    }

    return baseLabel;
}

// Generate a preview of array values for inline display
// (arrayPreview removed; previews are rendered directly in drawLine)

// Expand every branch of the given node.  Primitive leaves remain
// unchanged.  This function recurses through all descendants.
void expandAll(Node *node)
{
    node->expanded = true;
    for (auto &child : node->children)
    {
        expandAll(child.get());
    }
}

// Collapse every branch of the given node.  When keepRoot is true the
// top‑level node remains expanded so the structure of the document is
// still visible.
void collapseAll(Node *node, bool keepRoot)
{
    if (!node->isDummyRoot || !keepRoot)
    {
        node->expanded = false;
    }
    for (auto &child : node->children)
    {
        collapseAll(child.get(), false);
    }
}

// Expand nodes up to a specific nesting level.
// Level 0 means collapse all, level 1 means show only first level, etc.
void expandToLevel(Node *node, int targetLevel, int currentLevel)
{
    // Special handling for level 0 - collapse everything
    if (targetLevel == 0)
    {
        // Collapse everything, including root nodes
        node->expanded = false;
        for (auto &child : node->children)
        {
            collapseAll(child.get(), false);
        }
        return;
    }

    // For dummy roots, don't change their expansion state, just process children
    if (node->isDummyRoot)
    {
        node->expanded = true; // Always expand dummy roots when target > 0
        for (auto &child : node->children)
        {
            expandToLevel(child.get(), targetLevel, 1); // Children of dummy root are at level 1
        }
        return;
    }

    // For regular nodes:
    if (currentLevel < targetLevel)
    {
        // We haven't reached the target depth yet - expand and recurse
        node->expanded = true;
        for (auto &child : node->children)
        {
            expandToLevel(child.get(), targetLevel, currentLevel + 1);
        }
    }
    else
    {
        // We're at or beyond the target depth - collapse
        node->expanded = false;
        for (auto &child : node->children)
        {
            collapseAll(child.get(), false);
        }
    }
}

// Recursively search for nodes matching the given search term.  Both
// keys and values can be searched.  The search term and the
// candidates are compared in lowercase to achieve case‑insensitive
// matching.  When searching values, primitive values are converted to
// their JSON string representation.
void searchTree(const Node *node, const std::string &term,
                       bool searchKeys, bool searchValues,
                       std::vector<const Node *> &out)
{
    // Convert the node's key and value to lowercase strings
    if (!term.empty())
    {
        bool matched = false;
        if (searchKeys)
        {
            std::string keyLower = node->key;
            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
            if (keyLower.find(term) != std::string::npos)
            {
                matched = true;
            }
        }
        if (searchValues)
        {
            // Convert value to a displayable string
            std::string val;
            const json *v = node->value;
            if (v->is_string())
                val = v->get<std::string>();
            else if (v->is_boolean())
                val = (v->get<bool>() ? "true" : "false");
            else if (v->is_number())
                val = v->dump();
            else if (v->is_null())
                val = "null";
            else if (v->is_object())
                val = "dictionary";
            else if (v->is_array())
                val = "list";
            else
                val = v->dump();
            std::string valLower = val;
            std::transform(valLower.begin(), valLower.end(), valLower.begin(), ::tolower);
            if (valLower.find(term) != std::string::npos)
            {
                matched = true;
            }
        }
        if (matched)
        {
            out.push_back(node);
        }
    }
    // Continue search through children
    for (const auto &child : node->children)
    {
        searchTree(child.get(), term, searchKeys, searchValues, out);
    }
}

// Expand all ancestors of the given node so that it becomes visible.
void expandPath(Node *node)
{
    Node *cur = node->parent;
    while (cur)
    {
        cur->expanded = true;
        cur = cur->parent;
    }
}

// Base64 encoding for OSC 52 clipboard support
static std::string base64Encode(const std::string &input)
{
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string encoded;
    int val = 0, valb = -6;
    for (unsigned char c : input)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (encoded.size() % 4)
        encoded.push_back('=');
    return encoded;
}

// Check if an executable exists in PATH
// Check if OSC 52 clipboard sequences are likely to be supported
bool osc52Likely()
{
    // Check for explicit opt-out
    const char *noOsc52 = std::getenv("NO_OSC52");
    if (noOsc52 && *noOsc52)
        return false;

    const char *term = std::getenv("TERM");
    if (!term)
        return false;

    std::string termStr(term);

    // Terminals that definitely don't support OSC 52
    if (termStr == "dumb" || termStr == "linux")
        return false;

    // Common terminals/muxers that support OSC 52 (including tmux!)
    return termStr.find("xterm") != std::string::npos ||
           termStr.find("tmux") != std::string::npos ||
           termStr.find("screen") != std::string::npos ||
           termStr.find("rxvt") != std::string::npos ||
           termStr.find("alacritty") != std::string::npos ||
           termStr.find("foot") != std::string::npos ||
           termStr.find("kitty") != std::string::npos ||
           termStr.find("wezterm") != std::string::npos;
}

// Check if the current terminal supports clipboard operations via OSC 52
// Get a descriptive message about clipboard support status
std::string getClipboardStatusMessage()
{
    if (osc52Likely())
    {
        return "JSON copied to clipboard!";
    }
    if (std::getenv("TMUX") && !osc52Likely())
    {
        return "Clipboard not supported - tmux needs OSC 52 configuration";
    }
    return "Clipboard not supported by this terminal";
}

// Copy text to clipboard using OSC 52 escape sequence
void copyToClipboard(const std::string &text)
{
    if (!osc52Likely())
        return;

    // Limit payload size to prevent issues with muxers/terminals
    constexpr size_t maxOsc52Payload = 100000;
    std::string encoded = base64Encode(text);

    if (encoded.size() <= maxOsc52Payload)
    {
        // Try to write to /dev/tty first, fall back to stdout
        FILE *out = std::fopen("/dev/tty", "w");
        if (!out && isatty(fileno(stdout)))
        {
            out = stdout;
        }

        if (out)
        {
            // Use BEL terminator instead of ST for better compatibility
            std::fprintf(out, "\033]52;c;%s\a", encoded.c_str());
            std::fflush(out);
            if (out != stdout)
            {
                std::fclose(out);
            }
        }
    }
}

// Reconstruct JSON from a node and its children
json reconstructJson(const Node *node)
{
    if (node->isDummyRoot)
    {
        // For dummy root, return the actual JSON value
        return *node->value;
    }

    // Return the JSON value of this node
    return *node->value;
}

// Format file size in human-readable units
std::string formatFileSize(size_t size)
{
    const char *units[] = {"Bytes", "KB", "MB", "GB", "TB"};
    const size_t unitCount = sizeof(units) / sizeof(units[0]);

    double fileSize = static_cast<double>(size);
    size_t unitIndex = 0;

    while (fileSize >= 1024.0 && unitIndex < unitCount - 1)
    {
        fileSize /= 1024.0;
        unitIndex++;
    }

    // Format with appropriate precision
    std::ostringstream oss;
    if (unitIndex == 0)
    {
        // For bytes, show exact count
        oss << static_cast<size_t>(fileSize) << " " << units[unitIndex];
    }
    else if (fileSize < 10.0)
    {
        // For smaller values, show one decimal place
        oss.precision(1);
        oss << std::fixed << fileSize << " " << units[unitIndex];
    }
    else
    {
        // For larger values, show whole numbers
        oss << static_cast<int>(fileSize + 0.5) << " " << units[unitIndex];
    }

    return oss.str();
}

// Pretty-print JSON preserving NaN/Infinity literals
void printFormattedJson(const json &j, int indent)
{
    std::string pad(indent, ' ');
    switch (j.type())
    {
    case json::value_t::object:
        if (j.empty())
        {
            std::cout << "{}";
            return;
        }
        std::cout << "{\n";
        for (auto it = j.cbegin(); it != j.cend(); ++it)
        {
            std::cout << std::string(indent + 2, ' ')
                      << json(it.key()).dump() << ": ";
            printFormattedJson(it.value(), indent + 2);
            if (std::next(it) != j.cend())
                std::cout << ",";
            std::cout << "\n";
        }
        std::cout << pad << "}";
        break;
    case json::value_t::array:
        if (j.empty())
        {
            std::cout << "[]";
            return;
        }
        std::cout << "[\n";
        for (size_t i = 0; i < j.size(); ++i)
        {
            std::cout << std::string(indent + 2, ' ');
            printFormattedJson(j[i], indent + 2);
            if (i + 1 < j.size())
                std::cout << ",";
            std::cout << "\n";
        }
        std::cout << pad << "]";
        break;
    case json::value_t::string:
        std::cout << j.dump();
        break;
    case json::value_t::boolean:
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
        std::cout << j.dump();
        break;
    case json::value_t::number_float:
    {
        double d = j.get<double>();
        if (std::isnan(d))
            std::cout << "NaN";
        else if (std::isinf(d))
            std::cout << (d > 0 ? "Infinity" : "-Infinity");
        else
            std::cout << j.dump();
        break;
    }
    case json::value_t::null:
        std::cout << "null";
        break;
    default:
        std::cout << j.dump();
        break;
    }
}

// Replace placeholder strings with special floating-point values
static void replaceSpecialStrings(json &j)
{
    if (j.is_string())
    {
        auto &s = j.get_ref<json::string_t &>();
        if (s == "__JSON_VIEW_NaN__")
            j = std::numeric_limits<double>::quiet_NaN();
        else if (s == "__JSON_VIEW_INF__")
            j = std::numeric_limits<double>::infinity();
        else if (s == "__JSON_VIEW_NEG_INF__")
            j = -std::numeric_limits<double>::infinity();
    }
    else if (j.is_object())
    {
        for (auto &el : j.items())
            replaceSpecialStrings(el.value());
    }
    else if (j.is_array())
    {
        for (auto &el : j)
            replaceSpecialStrings(el);
    }
}

// Parse JSON while preserving NaN/Infinity literals by using placeholders
json parseJsonWithSpecialNumbers(const std::string &contents)
{
    std::string processed;
    processed.reserve(contents.size());
    bool inString = false;
    for (size_t i = 0; i < contents.size();)
    {
        char c = contents[i];
        if (inString)
        {
            processed.push_back(c);
            if (c == '\\')
            {
                ++i;
                if (i < contents.size())
                    processed.push_back(contents[i]);
                ++i;
            }
            else if (c == '"')
            {
                inString = false;
                ++i;
            }
            else
            {
                ++i;
            }
        }
        else
        {
            if (c == '"')
            {
                inString = true;
                processed.push_back(c);
                ++i;
            }
            else if (contents.compare(i, 3, "NaN") == 0)
            {
                processed += "\"__JSON_VIEW_NaN__\"";
                i += 3;
            }
            else if (contents.compare(i, 8, "Infinity") == 0)
            {
                processed += "\"__JSON_VIEW_INF__\"";
                i += 8;
            }
            else if (contents.compare(i, 9, "-Infinity") == 0)
            {
                processed += "\"__JSON_VIEW_NEG_INF__\"";
                i += 9;
            }
            else
            {
                processed.push_back(c);
                ++i;
            }
        }
    }

    json j = json::parse(processed);
    replaceSpecialStrings(j);
    return j;
}
