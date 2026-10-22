// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.hpp"
#include "cvision/core/utf8.hpp"
#include <charconv>
#include <map>
#include <variant>

namespace ck::sysmon {
namespace {
struct Json {
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    std::variant<std::monostate, std::string, std::uint64_t, Object, Array> value;
};
class Reader {
public:
    explicit Reader(std::string_view input) : input_(input) {}
    std::optional<Json> read(int depth = 0) {
        if (depth > 16 || ++nodes_ > 4096) return {};
        space();
        if (at_ == input_.size()) return {};
        if (input_[at_] == '"') { const auto s = string(); if (!s) return {}; return Json{*s}; }
        if (input_[at_] == '{' || input_[at_] == '[') {
            const bool object = input_[at_++] == '{';
            Json::Object members; Json::Array items;
            space();
            if (!take(object ? '}' : ']')) {
                do {
                    std::optional<std::string> key;
                    if (object) { key = string(); if (!key || !take(':')) return {}; }
                    auto child = read(depth + 1); if (!child) return {};
                    if (object) { if (!members.emplace(*key, std::move(*child)).second) return {}; }
                    else items.push_back(std::move(*child));
                } while (take(','));
                if (!take(object ? '}' : ']')) return {};
            }
            return object ? Json{std::move(members)} : Json{std::move(items)};
        }
        for (const std::string_view literal : {"null", "true", "false"}) {
            if (input_.substr(at_).starts_with(literal)) { at_ += literal.size(); return Json{}; }
        }
        const auto begin = at_;
        if (input_[at_] == '-') ++at_;
        if (at_ == input_.size() || input_[at_] < '0' || input_[at_] > '9') return {};
        if (input_[at_] == '0') ++at_;
        else while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') ++at_;
        bool integer = input_[begin] != '-';
        if (at_ < input_.size() && input_[at_] == '.') {
            integer = false; ++at_; const auto digits = at_;
            while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') ++at_;
            if (at_ == digits) return {};
        }
        if (at_ < input_.size() && (input_[at_] == 'e' || input_[at_] == 'E')) {
            integer = false; ++at_;
            if (at_ < input_.size() && (input_[at_] == '+' || input_[at_] == '-')) ++at_;
            const auto digits = at_;
            while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9') ++at_;
            if (at_ == digits) return {};
        }
        std::uint64_t number = 0;
        if (integer && std::from_chars(input_.data() + begin, input_.data() + at_, number).ec == std::errc{}) return Json{number};
        return Json{};
    }
    bool done() { space(); return at_ == input_.size(); }
private:
    void space() { while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\t' || input_[at_] == '\n' || input_[at_] == '\r')) ++at_; }
    bool take(char c) { space(); if (at_ == input_.size() || input_[at_] != c) return false; ++at_; return true; }
    std::optional<char32_t> hex() {
        if (input_.size() - at_ < 4) return {};
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[at_++];
            const unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16) return {};
            value = value * 16 + digit;
        }
        return static_cast<char32_t>(value);
    }
    std::optional<std::string> string() {
        if (!take('"')) return {};
        std::string out;
        while (at_ < input_.size()) {
            const char c = input_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 32) return {};
            if (c != '\\') out += c;
            else {
                if (at_ == input_.size()) return {};
                const char escaped = input_[at_++];
                if (escaped == 'u') {
                    auto code = hex(); if (!code) return {};
                    if (*code >= 0xd800 && *code <= 0xdbff) {
                        if (input_.substr(at_, 2) != "\\u") return {};
                        at_ += 2; const auto low = hex();
                        if (!low || *low < 0xdc00 || *low > 0xdfff) return {};
                        *code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
                    } else if (*code >= 0xdc00 && *code <= 0xdfff) return {};
                    ckv::utf8::encode(*code, out);
                } else {
                    const std::string_view codes = "\"\\/bfnrt", values = "\"\\/\b\f\n\r\t";
                    const auto index = codes.find(escaped); if (index == codes.npos) return {};
                    out += values[index];
                }
            }
        }
        return {};
    }
    std::string_view input_; std::size_t at_ = 0, nodes_ = 0;
};
}
std::optional<std::vector<ModelInfo>> parse_ollama(std::string_view json) {
    if (json.size() > 32768 || !ckv::utf8::is_valid(json)) return {};
    Reader reader(json); const auto root = reader.read();
    if (!root || !reader.done()) return {};
    const auto* object = std::get_if<Json::Object>(&root->value);
    if (!object) return {};
    const auto found = object->find("models"); if (found == object->end()) return {};
    const auto* models = std::get_if<Json::Array>(&found->second.value);
    if (!models || models->size() > 32) return {};
    std::vector<ModelInfo> result;
    for (const auto& model : *models) {
        const auto* fields = std::get_if<Json::Object>(&model.value); if (!fields) return {};
        const auto name = fields->find("name"); if (name == fields->end()) return {};
        const auto* text = std::get_if<std::string>(&name->second.value);
        if (!text || text->empty() || text->size() > 256) return {};
        ModelInfo info{*text, {}};
        const auto vram = fields->find("size_vram");
        if (vram != fields->end()) {
            const auto* size = std::get_if<std::uint64_t>(&vram->second.value); if (!size) return {};
            info.vram = *size;
        }
        result.push_back(std::move(info));
    }
    return result;
}
} // namespace ck::sysmon
