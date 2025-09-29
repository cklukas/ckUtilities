#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ck::ai {

struct SystemPrompt {
  std::string id;
  std::string name;
  std::string message;
  bool is_default = false;
  bool is_active = false;
};

class SystemPromptManager {
public:
  SystemPromptManager();
  ~SystemPromptManager();

  std::vector<SystemPrompt> get_prompts() const;
  std::optional<SystemPrompt> get_active_prompt() const;
  std::optional<SystemPrompt> get_prompt_by_id(const std::string &id) const;

  bool add_or_update_prompt(const SystemPrompt &prompt);
  bool delete_prompt(const std::string &id);
  bool set_active_prompt(const std::string &id);
  bool restore_default_prompt(const std::string &id);
  bool is_default_prompt_modified(const std::string &id) const;

  void refresh();

private:
  void ensure_default_prompts();
  void load();
  void save() const;
  std::filesystem::path storage_path() const;

  std::vector<SystemPrompt> prompts_;
  std::string active_prompt_id_;
  std::filesystem::path base_directory_;
};

} // namespace ck::ai
