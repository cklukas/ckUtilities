#pragma once

#include "../../../../include/ck/ai/config.hpp"
#include "../../../../include/ck/ai/llm.hpp"
#include "../../../../include/ck/ai/model_manager.hpp"
#include "../../../../include/ck/ai/system_prompt_manager.hpp"
#include "../../../../include/ck/options.hpp"

#include "../chat_session.hpp"
#include "../tvision_include.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class ChatWindow;

class ChatApp : public TApplication {
public:
  ChatApp(int argc, char **argv);
  ~ChatApp();

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
  const ck::chat::ChatSession::ConversationSettings &
  conversationSettings() const noexcept {
    return conversationSettings_;
  }
  int gpuLayersForModel(const std::string &modelId) const;
  int effectiveGpuLayers(const ck::ai::ModelInfo &model) const;
  void updateModelGpuLayers(const std::string &modelId, int gpuLayers);
  void updateConversationSettings(std::size_t contextTokens,
                                  std::size_t maxResponseTokens,
                                  std::size_t summaryThresholdTokens);
  void updateModelTokenSettings(const std::string &modelId,
                                std::size_t contextTokens,
                                std::size_t maxResponseTokens,
                                std::size_t summaryThresholdTokens);
  struct TokenLimits {
    std::size_t context_tokens = 0;
    std::size_t max_response_tokens = 0;
    std::size_t summary_trigger_tokens = 0;
  };
  TokenLimits
  resolveTokenLimits(const std::optional<std::string> &modelId) const;
  void refreshWindowTitles();
  bool showThinking() const noexcept { return showThinking_; }
  void setShowThinking(bool showThinking);
  bool showAnalysis() const noexcept { return showAnalysis_; }
  void setShowAnalysis(bool showAnalysis);
  bool parseMarkdownLinks() const noexcept { return parseMarkdownLinks_; }
  void setParseMarkdownLinks(bool enabled);
  const std::vector<std::string> &stopSequences() const noexcept {
    return stopSequences_;
  }
  void appendLog(const std::string &text);

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
  void applyConversationSettingsToWindows();
  int autoGpuLayersForModel(const ck::ai::ModelInfo &model) const;
  TokenLimits resolveTokenLimitsForModelInfo(
      const std::optional<std::string> &modelId,
      std::optional<ck::ai::ModelInfo> modelInfo) const;
  void applyThinkingVisibilityToWindows();
  void applyAnalysisVisibilityToWindows();
  void applyParseMarkdownLinksToWindows();
  void applyStopSequencesToWindows();
  std::vector<std::string> resolveStopSequencesForModel(
      const std::optional<std::string> &modelId,
      std::optional<ck::ai::ModelInfo> modelInfo) const;
  void loadModelInBackground();
  void stopModelLoading();

  std::vector<ChatWindow *> windows;
  int nextWindowNumber = 1;
  ck::ai::Config config;
  ck::ai::RuntimeConfig runtimeConfig;
  ck::ai::ModelManager modelManager_;
  ck::ai::SystemPromptManager promptManager_;
  std::string systemPrompt_ = "You are a friendly, knowledgeable assistant. "
                              "Respond clearly and helpfully.";
  ck::chat::ChatSession::ConversationSettings conversationSettings_{};

  std::vector<ck::ai::ModelInfo> menuDownloadedModels_;
  std::vector<ck::ai::SystemPrompt> menuPrompts_;
  std::optional<ck::ai::ModelInfo> currentActiveModel_;
  mutable std::mutex llmMutex_;
  std::shared_ptr<ck::ai::Llm> activeLlm_;
  bool showThinking_ = false;
  bool showAnalysis_ = false;
  bool parseMarkdownLinks_ = false;
  std::vector<std::string> stopSequences_;
  std::filesystem::path logPath_;
  std::filesystem::path binaryDir_;

  std::shared_ptr<ck::config::OptionRegistry> optionRegistry_;

  // Background model loading
  std::thread modelLoadingThread_;
  std::atomic<bool> modelLoadingInProgress_;
  std::atomic<bool> modelLoadingShouldStop_;
  std::atomic<bool> modelLoadingStarted_;
  std::string currentLoadingModelName_;
};
