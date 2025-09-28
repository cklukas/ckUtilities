#pragma once

#include "../../../../include/ck/ai/config.hpp"
#include "../../../../include/ck/ai/model_manager.hpp"
#include "../../../../include/ck/ai/system_prompt_manager.hpp"
#include "../../../../include/ck/ai/llm.hpp"

#include "../tvision_include.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class ChatWindow;

class ChatApp : public TApplication {
public:
  ChatApp(int argc, char **argv);

  virtual void handleEvent(TEvent &event) override;
  virtual void idle() override;

  TMenuBar *initMenuBar(TRect r);
  static TStatusLine *initStatusLine(TRect r);

  void registerWindow(ChatWindow *window);
  void unregisterWindow(ChatWindow *window);
  void refreshModelsMenu();
  void handleModelManagerChange();
  void handlePromptManagerChange();

  std::shared_ptr<ck::ai::Llm> getActiveLlm();
  const std::string &systemPrompt() const noexcept { return systemPrompt_; }
  std::optional<ck::ai::ModelInfo> activeModelInfo() const;

  const ck::ai::RuntimeConfig &runtime() const noexcept {
    return runtimeConfig;
  }
  ck::ai::ModelManager &modelManager() noexcept { return modelManager_; }

private:
  void openChatWindow();
  void showAboutDialog();
  void showModelManagerDialog();
  void selectModel(int modelIndex);
  void updateActiveModel();
  void rebuildMenuBar();
  std::shared_ptr<ck::ai::Llm> loadModel(const ck::ai::ModelInfo &model);
  void selectPrompt(int promptIndex);
  void showPromptManagerDialog();

  std::vector<ChatWindow *> windows;
  int nextWindowNumber = 1;
  ck::ai::Config config;
  ck::ai::RuntimeConfig runtimeConfig;
  ck::ai::ModelManager modelManager_;
  ck::ai::SystemPromptManager promptManager_;
  std::string systemPrompt_ =
      "You are a friendly, knowledgeable assistant. Respond clearly and helpfully.";

  std::vector<ck::ai::ModelInfo> menuDownloadedModels_;
  std::vector<ck::ai::SystemPrompt> menuPrompts_;
  std::optional<ck::ai::ModelInfo> currentActiveModel_;
  mutable std::mutex llmMutex_;
  std::shared_ptr<ck::ai::Llm> activeLlm_;
};
