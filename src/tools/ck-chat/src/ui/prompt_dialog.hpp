#pragma once

#include "../../../../include/ck/ai/system_prompt_manager.hpp"

#include "../tvision_include.hpp"

#include <memory>
#include <string>
#include <optional>
#include <vector>

class ChatApp;

class PromptDialog : public TDialog {
public:
  PromptDialog(TRect bounds, ck::ai::SystemPromptManager &manager, ChatApp *app = nullptr);
  ~PromptDialog();

  virtual void handleEvent(TEvent &event) override;

  static TDialog *create(TRect bounds, ck::ai::SystemPromptManager &manager);

private:
  void close() override;
  void setupControls();
  void refreshList();
  void updateButtons();
  void setStatus(const std::string &message);
  void addPrompt();
  void editPrompt();
  void deletePrompt();
  void activatePrompt();
  void updateSelectionStatus();
  std::string buildSelectionSummary(const ck::ai::SystemPrompt &prompt) const;
  int selectedIndex() const;
  std::optional<ck::ai::SystemPrompt> selectedPrompt() const;

  ck::ai::SystemPromptManager &manager_;
  ChatApp *chatApp_;

  TListBox *listBox_;
  TButton *addButton_;
  TButton *editButton_;
  TButton *deleteButton_;
  TButton *activateButton_;
  TButton *closeButton_;
  TLabel *statusLabel_;
  std::string statusText_;

  std::vector<ck::ai::SystemPrompt> prompts_;
  std::vector<int> indexMap_;
  int lastSelectedIndex_ = -1;
  bool closing_ = false;
};
