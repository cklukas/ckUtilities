#include "ck/ai/system_prompt_manager.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <random>

namespace ck::ai {

namespace {
const std::vector<SystemPrompt> kDefaultPrompts = {
    {"friendly-assistant", "Friendly Assistant",
     "You are a friendly, knowledgeable assistant. Respond clearly and helpfully.",
     true, true}};

std::string generate_id(const std::string &name) {
  std::string id;
  id.reserve(name.size());
  for (char ch : name) {
    if (std::isalnum(static_cast<unsigned char>(ch)))
      id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    else if (ch == ' ' || ch == '-' || ch == '_')
      id.push_back('-');
  }
  if (id.empty())
    id = "prompt";
  return id;
}

SystemPrompt normalize_prompt(SystemPrompt prompt) {
  if (prompt.id.empty())
    prompt.id = generate_id(prompt.name);
  return prompt;
}

} // namespace

SystemPromptManager::SystemPromptManager() {
  const char *home = std::getenv("HOME");
  if (home)
    base_directory_ = std::filesystem::path(home) / ".local" / "share" / "cktools";
  else
    base_directory_ = std::filesystem::current_path();

  std::filesystem::create_directories(base_directory_ / "prompts");
  load();
}

SystemPromptManager::~SystemPromptManager() { save(); }

std::vector<SystemPrompt> SystemPromptManager::get_prompts() const {
  return prompts_;
}

std::optional<SystemPrompt> SystemPromptManager::get_active_prompt() const {
  auto it = std::find_if(prompts_.begin(), prompts_.end(),
                         [&](const SystemPrompt &prompt) {
                           return prompt.id == active_prompt_id_;
                         });
  if (it != prompts_.end())
    return *it;
  if (!prompts_.empty())
    return prompts_.front();
  return std::nullopt;
}

std::optional<SystemPrompt>
SystemPromptManager::get_prompt_by_id(const std::string &id) const {
  auto it = std::find_if(prompts_.begin(), prompts_.end(),
                         [&](const SystemPrompt &prompt) {
                           return prompt.id == id;
                         });
  if (it != prompts_.end())
    return *it;
  return std::nullopt;
}

bool SystemPromptManager::add_or_update_prompt(const SystemPrompt &prompt) {
  SystemPrompt normalized = normalize_prompt(prompt);
  auto it = std::find_if(prompts_.begin(), prompts_.end(),
                         [&](const SystemPrompt &existing) {
                           return existing.id == normalized.id;
                         });

  if (it == prompts_.end()) {
    prompts_.push_back(normalized);
  } else {
    bool was_active = it->id == active_prompt_id_;
    *it = normalized;
    if (was_active)
      active_prompt_id_ = normalized.id;
  }

  ensure_default_prompts();
  save();
  return true;
}

bool SystemPromptManager::delete_prompt(const std::string &id) {
  auto it = std::find_if(prompts_.begin(), prompts_.end(),
                         [&](const SystemPrompt &prompt) { return prompt.id == id; });
  if (it == prompts_.end())
    return false;
  if (it->is_default)
    return false;

  bool removing_active = it->id == active_prompt_id_;
  prompts_.erase(it);
  if (removing_active)
    active_prompt_id_.clear();

  ensure_default_prompts();
  save();
  return true;
}

bool SystemPromptManager::set_active_prompt(const std::string &id) {
  auto it = std::find_if(prompts_.begin(), prompts_.end(),
                         [&](const SystemPrompt &prompt) { return prompt.id == id; });
  if (it == prompts_.end())
    return false;

  active_prompt_id_ = id;
  for (auto &prompt : prompts_)
    prompt.is_active = (prompt.id == active_prompt_id_);

  save();
  return true;
}

bool SystemPromptManager::restore_default_prompt(const std::string &id) {
  auto promptIt = std::find_if(prompts_.begin(), prompts_.end(),
                               [&](const SystemPrompt &prompt) {
                                 return prompt.id == id;
                               });
  if (promptIt == prompts_.end() || !promptIt->is_default)
    return false;

  auto defaultsIt = std::find_if(kDefaultPrompts.begin(), kDefaultPrompts.end(),
                                 [&](const SystemPrompt &prompt) {
                                   return prompt.id == id;
                                 });
  if (defaultsIt == kDefaultPrompts.end())
    return false;

  bool was_active = promptIt->is_active;
  *promptIt = *defaultsIt;
  promptIt->is_active = was_active;
  if (was_active)
    active_prompt_id_ = promptIt->id;

  save();
  return true;
}

void SystemPromptManager::refresh() {
  load();
}

void SystemPromptManager::ensure_default_prompts() {
  for (const auto &defaults : kDefaultPrompts) {
    auto it = std::find_if(prompts_.begin(), prompts_.end(),
                           [&](const SystemPrompt &prompt) {
                             return prompt.id == defaults.id;
                           });
    if (it == prompts_.end()) {
      prompts_.push_back(defaults);
    }
  }

  if (active_prompt_id_.empty()) {
    auto it = std::find_if(prompts_.begin(), prompts_.end(),
                           [](const SystemPrompt &prompt) { return prompt.is_default; });
    if (it != prompts_.end())
      active_prompt_id_ = it->id;
    else if (!prompts_.empty())
      active_prompt_id_ = prompts_.front().id;
  }

  for (auto &prompt : prompts_)
    prompt.is_active = prompt.id == active_prompt_id_;
}

void SystemPromptManager::load() {
  prompts_.clear();
  active_prompt_id_.clear();

  auto path = storage_path();
  if (std::filesystem::exists(path)) {
    std::ifstream file(path);
    if (file.is_open()) {
      try {
        nlohmann::json doc;
        file >> doc;
        if (doc.contains("prompts")) {
          for (const auto &entry : doc["prompts"]) {
            SystemPrompt prompt;
            prompt.id = entry.value("id", "");
            prompt.name = entry.value("name", "");
            prompt.message = entry.value("message", "");
            prompt.is_default = entry.value("is_default", false);
            prompt = normalize_prompt(prompt);
            prompts_.push_back(prompt);
          }
        }
        active_prompt_id_ = doc.value("active_prompt_id", "");
      } catch (...) {
        prompts_.clear();
        active_prompt_id_.clear();
      }
    }
  }

  ensure_default_prompts();
  save();
}

void SystemPromptManager::save() const {
  nlohmann::json doc;
  doc["active_prompt_id"] = active_prompt_id_;
  nlohmann::json prompts_json = nlohmann::json::array();
  for (const auto &prompt : prompts_) {
    nlohmann::json entry;
    entry["id"] = prompt.id;
    entry["name"] = prompt.name;
    entry["message"] = prompt.message;
    entry["is_default"] = prompt.is_default;
    prompts_json.push_back(entry);
  }
  doc["prompts"] = prompts_json;

  auto path = storage_path();
  std::ofstream file(path);
  if (file.is_open())
    file << doc.dump(2);
}

std::filesystem::path SystemPromptManager::storage_path() const {
  return base_directory_ / "prompts" / "system_prompts.json";
}

} // namespace ck::ai
