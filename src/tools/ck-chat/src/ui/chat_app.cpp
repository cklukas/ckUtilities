#include "chat_app.hpp"
#include "../commands.hpp"
#include "chat_options.hpp"
#include "chat_window.hpp"
#include "ck/about_dialog.hpp"
#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"
#include "ck/app_info.hpp"
#include "ck/launcher.hpp"
#include "ck/hotkeys.hpp"
#include "ck/ui/clock_view.hpp"
#include "ck/ui/window_menu.hpp"
#include "model_dialog.hpp"
#include "model_loading_dialog.hpp"
#include "prompt_dialog.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace
{
  const ck::appinfo::ToolInfo &tool_info()
  {
    return ck::appinfo::requireTool("ck-chat");
  }

  ck::ai::RuntimeConfig runtime_from_config(const ck::ai::Config &config)
  {
    ck::ai::RuntimeConfig runtime = config.runtime;
    if (runtime.model_path.empty())
      runtime.model_path = "stub-model.gguf";
    return runtime;
  }
} // namespace

ChatApp::ChatApp(int argc, char **argv)
    : TProgInit(&ChatApp::initStatusLine, nullptr, &TApplication::initDeskTop),
      ck::ui::ClockAwareApplication(),
      modelLoadingInProgress_(false),
      modelLoadingShouldStop_(false), modelLoadingStarted_(false)
{
  insertMenuClock();

  config = ck::ai::ConfigLoader::load_or_default();
  runtimeConfig = runtime_from_config(config);

  optionRegistry_ = std::make_shared<ck::config::OptionRegistry>("ck-chat");
  ck::chat::registerChatOptions(*optionRegistry_);
  optionRegistry_->loadDefaults();
  showThinking_ =
      optionRegistry_->getBool(ck::chat::kOptionShowThinking, false);
  showAnalysis_ =
      optionRegistry_->getBool(ck::chat::kOptionShowAnalysis, false);
  parseMarkdownLinks_ =
      optionRegistry_->getBool(ck::chat::kOptionParseMarkdownLinks, false);

  const std::string savedModelId =
      optionRegistry_->getString(ck::chat::kOptionActiveModelId, std::string());
  if (!savedModelId.empty() &&
      !modelManager_.is_model_active(savedModelId))
    modelManager_.activate_model(savedModelId);

  const std::string savedPromptId = optionRegistry_->getString(
      ck::chat::kOptionActivePromptId, std::string());
  if (!savedPromptId.empty())
    promptManager_.set_active_prompt(savedPromptId);

  if (argv && argc > 0 && argv[0])
  {
    try
    {
      binaryDir_ = std::filesystem::absolute(std::filesystem::path(argv[0]))
                       .parent_path();
    }
    catch (...)
    {
      binaryDir_.clear();
    }
  }
  if (binaryDir_.empty())
    binaryDir_ = std::filesystem::current_path();

  conversationSettings_.max_context_tokens =
      runtimeConfig.context_window_tokens;
  conversationSettings_.summary_trigger_tokens =
      runtimeConfig.summary_trigger_tokens;
  conversationSettings_.max_response_tokens = runtimeConfig.max_output_tokens;
  stopSequences_ = ck::chat::ChatSession::defaultStopSequences();

  if (auto prompt = promptManager_.get_active_prompt())
    systemPrompt_ = prompt->message;

  handlePromptManagerChange();

  openChatWindow();
  applyConversationSettingsToWindows();

  logPath_ = binaryDir_ / "chat.log";
  std::ofstream(logPath_, std::ios::trunc).close();
}

ChatApp::~ChatApp() { stopModelLoading(); }

void ChatApp::registerWindow(ChatWindow *window)
{
  if (!window)
    return;
  windows.push_back(window);
  window->setShowThinking(showThinking_);
  window->setShowAnalysis(showAnalysis_);
  window->setParseMarkdownLinks(parseMarkdownLinks_);
  window->setStopSequences(stopSequences_);
}

void ChatApp::unregisterWindow(ChatWindow *window)
{
  auto it = std::remove(windows.begin(), windows.end(), window);
  windows.erase(it, windows.end());
}

void ChatApp::openChatWindow()
{
  if (!deskTop)
    return;

  TRect bounds = deskTop->getExtent();
  bounds.grow(-2, -1);
  if (bounds.b.x <= bounds.a.x + 10 || bounds.b.y <= bounds.a.y + 5)
    bounds = TRect(0, 0, 70, 20);

  auto *window = new ChatWindow(*this, bounds, nextWindowNumber++);
  deskTop->insert(window);
  window->applyConversationSettings(conversationSettings_);
  window->refreshWindowTitle();
  window->select();
}

void ChatApp::handleEvent(TEvent &event)
{
  TApplication::handleEvent(event);
  if (event.what == evCommand)
  {
    switch (event.message.command)
    {
    case cmNewChat:
      openChatWindow();
      clearEvent(event);
      break;
    case cmReturnToLauncher:
      std::exit(ck::launcher::kReturnToLauncherExitCode);
      break;
    case cmAbout:
      showAboutDialog();
      clearEvent(event);
      break;
    case cmManageModels:
      showModelManagerDialog();
      clearEvent(event);
      break;
    case cmShowThinking:
      setShowThinking(true);
      clearEvent(event);
      break;
    case cmHideThinking:
      setShowThinking(false);
      clearEvent(event);
      break;
    case cmShowAnalysis:
      setShowAnalysis(true);
      clearEvent(event);
      break;
    case cmHideAnalysis:
      setShowAnalysis(false);
      clearEvent(event);
      break;
    case cmToggleParseMarkdownLinks:
      setParseMarkdownLinks(!parseMarkdownLinks_);
      clearEvent(event);
      break;
    case cmManagePrompts:
      showPromptManagerDialog();
      clearEvent(event);
      break;
    case cmSelectModel1:
    case cmSelectModel2:
    case cmSelectModel3:
    case cmSelectModel4:
    case cmSelectModel5:
    case cmSelectModel6:
    case cmSelectModel7:
    case cmSelectModel8:
    case cmSelectModel9:
    case cmSelectModel10:
      selectModel(event.message.command - cmSelectModel1);
      clearEvent(event);
      break;
    default:
      if (event.message.command >= cmSelectPromptBase &&
          event.message.command < cmSelectPromptBase + 10)
      {
        selectPrompt(event.message.command - cmSelectPromptBase);
        clearEvent(event);
        break;
      }
      if (event.message.command == cmNoOp)
      {
        clearEvent(event);
        break;
      }
      break;
    }
  }
}

void ChatApp::idle()
{
  ck::ui::ClockAwareApplication::idle();

  // Start model loading if not already started and deskTop is available
  if (!modelLoadingStarted_ && deskTop)
  {
    modelLoadingStarted_ = true;
    updateActiveModel();
  }

  for (auto *window : windows)
  {
    if (window)
      window->processPendingResponses();
  }
}

TMenuBar *ChatApp::initMenuBar(TRect r)
{
  r.b.y = r.a.y + 1;

  TSubMenu &fileMenu = *new TSubMenu("~F~ile", hcNoContext) +
                       *new TMenuItem("~N~ew Chat...", cmNewChat, kbNoKey,
                                      hcNoContext) +
                       *new TMenuItem("~C~lose Window", cmClose, kbNoKey,
                                      hcNoContext) +
                       newLine();
  if (ck::launcher::launchedFromCkLauncher())
    fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher,
                              kbNoKey, hcNoContext);
  fileMenu + *new TMenuItem("E~x~it", cmQuit, kbNoKey, hcNoContext);

  TSubMenu &editMenu = *new TSubMenu("~E~dit", hcNoContext) +
                       *new TMenuItem("Copy ~L~ast Response",
                                      cmCopyLastResponse, kbNoKey,
                                      hcNoContext) +
                       *new TMenuItem("Copy ~F~ull Conversation",
                                      cmCopyFullConversation, kbNoKey,
                                      hcNoContext);

  TSubMenu &modelsMenu = *new TSubMenu("~M~odels", hcNoContext);

  menuDownloadedModels_ = modelManager_.get_downloaded_models();
  auto activeInfo = activeModelInfo();

  if (menuDownloadedModels_.empty())
  {
    modelsMenu +
        *new TMenuItem("~N~o downloaded models", cmNoOp, kbNoKey, hcNoContext);
  }
  else
  {
    TMenuItem *defaultItem = nullptr;
    for (size_t i = 0; i < menuDownloadedModels_.size() && i < 10; ++i)
    {
      const auto &model = menuDownloadedModels_[i];
      std::string menuText = model.name;
      if (model.is_active)
        menuText += " [active]";

      ushort command = cmSelectModel1 + static_cast<ushort>(i);
      auto *item =
          new TMenuItem(menuText.c_str(), command, kbNoKey, hcNoContext);
      modelsMenu + *item;

      if (activeInfo && activeInfo->id == model.id)
        defaultItem = item;
    }

    if (defaultItem && modelsMenu.subMenu)
      modelsMenu.subMenu->deflt = defaultItem;
  }

  modelsMenu + newLine();

  menuPrompts_ = promptManager_.get_prompts();
  auto activePrompt = promptManager_.get_active_prompt();

  if (menuPrompts_.empty())
  {
    modelsMenu +
        *new TMenuItem("~N~o prompts defined", cmNoOp, kbNoKey, hcNoContext);
  }
  else
  {
    for (size_t i = 0; i < menuPrompts_.size() && i < 10; ++i)
    {
      const auto &prompt = menuPrompts_[i];
      std::string label = prompt.name;
      if (activePrompt && activePrompt->id == prompt.id)
        label += " [current]";

      ushort command = cmSelectPromptBase + static_cast<ushort>(i);
      auto *item = new TMenuItem(label.c_str(), command, kbNoKey, hcNoContext);
      modelsMenu + *item;
    }
  }

  modelsMenu + newLine() +
      *new TMenuItem("Manage ~M~odels...", cmManageModels, kbNoKey, hcNoContext);
  modelsMenu + *new TMenuItem("Manage ~P~rompts...", cmManagePrompts, kbNoKey,
                              hcNoContext);

  TSubMenu &viewMenu = *new TSubMenu("~V~iew", hcNoContext);
  if (showThinking_)
    viewMenu +
        *new TMenuItem("~H~ide Thinking", cmHideThinking, kbNoKey, hcNoContext);
  else
    viewMenu +
        *new TMenuItem("~S~how Thinking", cmShowThinking, kbNoKey, hcNoContext);
  if (showAnalysis_)
    viewMenu +
        *new TMenuItem("Hide ~A~nalysis", cmHideAnalysis, kbNoKey, hcNoContext);
  else
    viewMenu +
        *new TMenuItem("Show ~A~nalysis", cmShowAnalysis, kbNoKey, hcNoContext);

  std::string parseLabel =
      std::string(parseMarkdownLinks_ ? "[x] " : "[ ] ") + "Parse Markdown Links";
  viewMenu + *new TMenuItem(parseLabel.c_str(), cmToggleParseMarkdownLinks,
                            kbNoKey, hcNoContext);

  TSubMenu &windowMenu = ck::ui::createWindowMenu();

  TMenuItem &menuChain =
      fileMenu + editMenu + modelsMenu + viewMenu + windowMenu +
      *new TSubMenu("~H~elp", hcNoContext) +
      *new TMenuItem("~A~bout", cmAbout, kbNoKey, hcNoContext);

  ck::hotkeys::configureMenuTree(menuChain);
  return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
}

TStatusLine *ChatApp::initStatusLine(TRect r)
{
  r.a.y = r.b.y - 1;

  auto *newItem = new TStatusItem("New Chat", kbNoKey, cmNewChat);
  ck::hotkeys::configureStatusItem(*newItem, "New Chat");
  auto *closeItem = new TStatusItem("Close", kbNoKey, cmClose);
  ck::hotkeys::configureStatusItem(*closeItem, "Close");
  newItem->next = closeItem;
  TStatusItem *tail = closeItem;

  if (ck::launcher::launchedFromCkLauncher())
  {
    auto *returnItem =
        new TStatusItem("Return", kbNoKey, cmReturnToLauncher);
    ck::hotkeys::configureStatusItem(*returnItem, "Return");
    tail->next = returnItem;
    tail = returnItem;
  }

  auto *quitItem = new TStatusItem("Quit", kbNoKey, cmQuit);
  ck::hotkeys::configureStatusItem(*quitItem, "Quit");
  tail->next = quitItem;

  return new TStatusLine(r, *new TStatusDef(0, 0xFFFF, newItem));
}

void ChatApp::showAboutDialog()
{
  const auto &info = tool_info();
#ifdef CK_CHAT_VERSION
  ck::ui::showAboutDialog(info.executable, CK_CHAT_VERSION,
                          info.aboutDescription);
#else
  ck::ui::showAboutDialog(info.executable, "dev", info.aboutDescription);
#endif
}

void ChatApp::showModelManagerDialog()
{
  // Fixed dialog size based on content needs
  TRect bounds(5, 3, 105, 33);

  auto *dialog = new ModelDialog(bounds, modelManager_, this);
  if (dialog)
  {
    deskTop->insert(dialog);
    dialog->select();
  }
}

void ChatApp::refreshModelsMenu() { rebuildMenuBar(); }

void ChatApp::selectModel(int modelIndex)
{
  if (menuDownloadedModels_.empty() || modelIndex < 0 ||
      modelIndex >= static_cast<int>(menuDownloadedModels_.size()))
  {
    messageBox("Invalid model selection", mfError | mfOKButton);
    return;
  }

  const auto &model = menuDownloadedModels_[modelIndex];
  if (!model.is_downloaded)
  {
    messageBox("Model is not downloaded", mfError | mfOKButton);
    return;
  }

  if (!modelManager_.activate_model(model.id))
  {
    messageBox("Failed to activate model: " + model.name, mfError | mfOKButton);
    return;
  }

  handleModelManagerChange();
}

void ChatApp::handleModelManagerChange()
{
  updateActiveModel();
  rebuildMenuBar();
  refreshWindowTitles();
}

void ChatApp::selectPrompt(int promptIndex)
{
  if (menuPrompts_.empty() || promptIndex < 0 ||
      promptIndex >= static_cast<int>(menuPrompts_.size()))
  {
    messageBox("Invalid prompt selection", mfError | mfOKButton);
    return;
  }

  const auto &prompt = menuPrompts_[promptIndex];
  if (!promptManager_.set_active_prompt(prompt.id))
  {
    messageBox("Failed to activate prompt", mfError | mfOKButton);
    return;
  }

  handlePromptManagerChange();
}

void ChatApp::showPromptManagerDialog()
{
  TRect bounds(10, 4, 77, 23);
  auto *dialog = new PromptDialog(bounds, promptManager_, this);
  if (dialog)
  {
    deskTop->insert(dialog);
    dialog->select();
  }
}

void ChatApp::applyConversationSettingsToWindows()
{
  for (auto *window : windows)
  {
    if (window)
    {
      window->applyConversationSettings(conversationSettings_);
      window->refreshWindowTitle();
    }
  }
}

void ChatApp::refreshWindowTitles()
{
  for (auto *window : windows)
  {
    if (window)
      window->refreshWindowTitle();
  }
}

void ChatApp::setShowThinking(bool showThinking)
{
  if (showThinking_ == showThinking)
    return;
  showThinking_ = showThinking;
  persistBoolOption(ck::chat::kOptionShowThinking, showThinking_);
  applyThinkingVisibilityToWindows();
  rebuildMenuBar();
}

void ChatApp::setShowAnalysis(bool showAnalysis)
{
  if (showAnalysis_ == showAnalysis)
    return;
  showAnalysis_ = showAnalysis;
  persistBoolOption(ck::chat::kOptionShowAnalysis, showAnalysis_);
  applyAnalysisVisibilityToWindows();
  rebuildMenuBar();
}

void ChatApp::setParseMarkdownLinks(bool enabled)
{
  if (parseMarkdownLinks_ == enabled)
    return;
  parseMarkdownLinks_ = enabled;
  persistBoolOption(ck::chat::kOptionParseMarkdownLinks, parseMarkdownLinks_);
  applyParseMarkdownLinksToWindows();
  rebuildMenuBar();
}

void ChatApp::appendLog(const std::string &text)
{
  if (logPath_.empty())
    return;
  std::lock_guard<std::mutex> lock(llmMutex_);
  std::ofstream file(logPath_, std::ios::app);
  if (!file.is_open())
    return;
  file << text;
  if (!text.empty() && text.back() != '\n')
    file << '\n';
}

void ChatApp::persistBoolOption(const std::string &key, bool value)
{
  if (!optionRegistry_)
    return;
  ck::config::OptionValue desired(value);
  if (optionRegistry_->get(key) == desired)
    return;
  optionRegistry_->set(key, desired);
  optionRegistry_->saveDefaults();
}

void ChatApp::persistStringOption(const std::string &key,
                                  const std::string &value)
{
  if (!optionRegistry_)
    return;
  ck::config::OptionValue desired(value);
  if (optionRegistry_->get(key) == desired)
    return;
  optionRegistry_->set(key, desired);
  optionRegistry_->saveDefaults();
}

void ChatApp::applyThinkingVisibilityToWindows()
{
  for (auto *window : windows)
  {
    if (window)
      window->setShowThinking(showThinking_);
  }
}

void ChatApp::applyAnalysisVisibilityToWindows()
{
  for (auto *window : windows)
  {
    if (window)
      window->setShowAnalysis(showAnalysis_);
  }
}

void ChatApp::applyParseMarkdownLinksToWindows()
{
  for (auto *window : windows)
  {
    if (window)
      window->setParseMarkdownLinks(parseMarkdownLinks_);
  }
}

void ChatApp::applyStopSequencesToWindows()
{
  for (auto *window : windows)
  {
    if (window)
      window->setStopSequences(stopSequences_);
  }
}

void ChatApp::updateConversationSettings(std::size_t contextTokens,
                                         std::size_t maxResponseTokens,
                                         std::size_t summaryThresholdTokens)
{
  if (contextTokens == 0)
    contextTokens = runtimeConfig.context_window_tokens;

  if (maxResponseTokens == 0)
  {
    maxResponseTokens = runtimeConfig.max_output_tokens > 0
                            ? runtimeConfig.max_output_tokens
                            : 512;
  }

  if (maxResponseTokens > contextTokens)
    maxResponseTokens = contextTokens;

  if (summaryThresholdTokens == 0)
    summaryThresholdTokens = runtimeConfig.summary_trigger_tokens;

  if (summaryThresholdTokens > contextTokens)
    summaryThresholdTokens = contextTokens;

  conversationSettings_.max_context_tokens = contextTokens;
  conversationSettings_.max_response_tokens = maxResponseTokens;
  conversationSettings_.summary_trigger_tokens = summaryThresholdTokens;

  runtimeConfig.context_window_tokens = contextTokens;
  runtimeConfig.max_output_tokens = maxResponseTokens;
  runtimeConfig.summary_trigger_tokens = summaryThresholdTokens;
  config.runtime = runtimeConfig;

  applyConversationSettingsToWindows();
  ck::ai::ConfigLoader::save(config);
}

int ChatApp::gpuLayersForModel(const std::string &modelId) const
{
  int layers = runtimeConfig.gpu_layers;
  auto it = config.model_overrides.find(modelId);
  if (it != config.model_overrides.end() && it->second.gpu_layers != -9999)
    layers = it->second.gpu_layers;
  return layers;
}

int ChatApp::effectiveGpuLayers(const ck::ai::ModelInfo &model) const
{
  int requested = gpuLayersForModel(model.id);
  if (requested == -1)
    return autoGpuLayersForModel(model);
  return requested;
}

int ChatApp::autoGpuLayersForModel(const ck::ai::ModelInfo &model) const
{
#if defined(__APPLE__)
  std::size_t sizeGiB = model.size_bytes / (1024ull * 1024ull * 1024ull);
  if (sizeGiB <= 0)
    sizeGiB = 1;
  if (sizeGiB <= 6)
    return 9999; // fully offload small models
  if (sizeGiB <= 10)
    return 80;
  if (sizeGiB <= 14)
    return 60;
  if (sizeGiB <= 20)
    return 40;
  return 24;
#else
  (void)model;
  return 0;
#endif
}

ChatApp::TokenLimits ChatApp::resolveTokenLimitsForModelInfo(
    const std::optional<std::string> &modelId,
    std::optional<ck::ai::ModelInfo> modelInfo) const
{
  TokenLimits limits{};
  limits.context_tokens = runtimeConfig.context_window_tokens;
  limits.max_response_tokens = runtimeConfig.max_output_tokens;
  limits.summary_trigger_tokens = runtimeConfig.summary_trigger_tokens;

  if (!modelId)
    return limits;

  if (!modelInfo)
    modelInfo = modelManager_.get_model_by_id(*modelId);

  if (modelInfo)
  {
    if (modelInfo->default_context_window_tokens > 0)
      limits.context_tokens = modelInfo->default_context_window_tokens;
    if (modelInfo->default_max_output_tokens > 0)
      limits.max_response_tokens = modelInfo->default_max_output_tokens;
    if (modelInfo->default_summary_trigger_tokens > 0)
      limits.summary_trigger_tokens = modelInfo->default_summary_trigger_tokens;
  }

  auto overrideIt = config.model_overrides.find(*modelId);
  if (overrideIt != config.model_overrides.end())
  {
    const auto &overrideConfig = overrideIt->second;
    if (overrideConfig.context_window_tokens != 0)
      limits.context_tokens = overrideConfig.context_window_tokens;
    if (overrideConfig.max_output_tokens != 0)
      limits.max_response_tokens = overrideConfig.max_output_tokens;
    if (overrideConfig.summary_trigger_tokens != 0)
      limits.summary_trigger_tokens = overrideConfig.summary_trigger_tokens;
  }

  if (limits.max_response_tokens > limits.context_tokens)
    limits.max_response_tokens = limits.context_tokens;
  if (limits.summary_trigger_tokens > limits.context_tokens)
    limits.summary_trigger_tokens = limits.context_tokens;

  return limits;
}

ChatApp::TokenLimits
ChatApp::resolveTokenLimits(const std::optional<std::string> &modelId) const
{
  return resolveTokenLimitsForModelInfo(
      modelId, modelId ? modelManager_.get_model_by_id(*modelId)
                       : std::optional<ck::ai::ModelInfo>{});
}

std::vector<std::string> ChatApp::resolveStopSequencesForModel(
    const std::optional<std::string> &modelId,
    std::optional<ck::ai::ModelInfo> modelInfo) const
{
  std::vector<std::string> stops =
      ck::chat::ChatSession::defaultStopSequences();
  if (!modelId)
    return stops;

  if (!modelInfo)
    modelInfo = modelManager_.get_model_by_id(*modelId);

  if (modelInfo && !modelInfo->default_stop_sequences.empty())
  {
    stops.insert(stops.end(), modelInfo->default_stop_sequences.begin(),
                 modelInfo->default_stop_sequences.end());
  }

  stops.erase(
      std::remove_if(stops.begin(), stops.end(),
                     [](const std::string &value)
                     { return value.empty(); }),
      stops.end());
  std::sort(stops.begin(), stops.end());
  stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
  if (stops.empty())
    stops = ck::chat::ChatSession::defaultStopSequences();
  return stops;
}

void ChatApp::updateModelGpuLayers(const std::string &modelId, int gpuLayers)
{
  if (gpuLayers < -1)
    gpuLayers = -1;

  auto &overrideEntry = config.model_overrides[modelId];
  overrideEntry.gpu_layers = gpuLayers;

  std::shared_ptr<ck::ai::Llm> newLlm;
  if (currentActiveModel_ && currentActiveModel_->id == modelId)
  {
    newLlm = loadModel(*currentActiveModel_);
  }

  ck::ai::ConfigLoader::save(config);

  if (newLlm)
  {
    std::lock_guard<std::mutex> lock(llmMutex_);
    activeLlm_ = std::move(newLlm);
    conversationSettings_.max_context_tokens =
        runtimeConfig.context_window_tokens;
    conversationSettings_.max_response_tokens = runtimeConfig.max_output_tokens;
    conversationSettings_.summary_trigger_tokens =
        runtimeConfig.summary_trigger_tokens;
    applyConversationSettingsToWindows();
  }

  refreshWindowTitles();
}

void ChatApp::updateModelTokenSettings(const std::string &modelId,
                                       std::size_t contextTokens,
                                       std::size_t maxResponseTokens,
                                       std::size_t summaryThresholdTokens)
{
  if (contextTokens == 0)
    contextTokens = runtimeConfig.context_window_tokens;

  if (maxResponseTokens == 0)
    maxResponseTokens = runtimeConfig.max_output_tokens;
  if (maxResponseTokens > contextTokens)
    maxResponseTokens = contextTokens;

  if (summaryThresholdTokens == 0)
    summaryThresholdTokens = runtimeConfig.summary_trigger_tokens;
  if (summaryThresholdTokens > contextTokens)
    summaryThresholdTokens = contextTokens;

  auto &overrideEntry = config.model_overrides[modelId];
  overrideEntry.context_window_tokens = contextTokens;
  overrideEntry.max_output_tokens = maxResponseTokens;
  overrideEntry.summary_trigger_tokens = summaryThresholdTokens;

  std::shared_ptr<ck::ai::Llm> newLlm;
  if (currentActiveModel_ && currentActiveModel_->id == modelId)
  {
    newLlm = loadModel(*currentActiveModel_);
  }

  ck::ai::ConfigLoader::save(config);

  if (newLlm)
  {
    std::lock_guard<std::mutex> lock(llmMutex_);
    activeLlm_ = std::move(newLlm);
    conversationSettings_.max_context_tokens =
        runtimeConfig.context_window_tokens;
    conversationSettings_.max_response_tokens = runtimeConfig.max_output_tokens;
    conversationSettings_.summary_trigger_tokens =
        runtimeConfig.summary_trigger_tokens;
    applyConversationSettingsToWindows();
  }

  refreshWindowTitles();
}

void ChatApp::handlePromptManagerChange()
{
  auto activePrompt = promptManager_.get_active_prompt();
  if (activePrompt)
    systemPrompt_ = activePrompt->message;
  if (activePrompt)
    persistStringOption(ck::chat::kOptionActivePromptId, activePrompt->id);
  else
    persistStringOption(ck::chat::kOptionActivePromptId, std::string());

  {
    std::lock_guard<std::mutex> lock(llmMutex_);
    if (activeLlm_)
      activeLlm_->set_system_prompt(systemPrompt_);
  }

  for (auto *window : windows)
  {
    if (window)
      window->applySystemPrompt(systemPrompt_);
  }

  rebuildMenuBar();
}

std::shared_ptr<ck::ai::Llm> ChatApp::getActiveLlm()
{
  std::lock_guard<std::mutex> lock(llmMutex_);
  return activeLlm_;
}

std::optional<ck::ai::ModelInfo> ChatApp::activeModelInfo() const
{
  std::lock_guard<std::mutex> lock(llmMutex_);
  return currentActiveModel_;
}

void ChatApp::rebuildMenuBar()
{
  if (!deskTop)
    return;

  TRect bounds;
  if (TProgram::menuBar)
    bounds = TProgram::menuBar->getBounds();
  else
  {
    bounds = deskTop->getExtent();
    bounds.b.y = bounds.a.y + 1;
  }

  if (TProgram::menuBar)
  {
    TMenuBar *oldBar = TProgram::menuBar;
    remove(oldBar);
    TObject::destroy(oldBar);
  }

  TMenuBar *newBar = initMenuBar(bounds);
  if (newBar)
  {
    insert(newBar);
    TProgram::menuBar = newBar;
    newBar->drawView();
    promoteClocksToFront();
  }
}

std::shared_ptr<ck::ai::Llm>
ChatApp::loadModel(const ck::ai::ModelInfo &model)
{
  std::filesystem::path modelPath = model.local_path;
  if (modelPath.empty())
    modelPath = modelManager_.get_model_path(model.id);

  if (modelPath.empty() || !std::filesystem::exists(modelPath))
  {
    messageBox(("Model file not found: " + model.name).c_str(),
               mfError | mfOKButton);
    return nullptr;
  }

  ck::ai::RuntimeConfig newRuntime = runtimeConfig;
  newRuntime.model_path = modelPath.string();
  if (newRuntime.threads <= 0)
    newRuntime.threads = static_cast<int>(std::thread::hardware_concurrency());

  TokenLimits limits = resolveTokenLimitsForModelInfo(
      model.id, modelManager_.get_model_by_id(model.id));

  if (limits.context_tokens > 0)
    newRuntime.context_window_tokens = limits.context_tokens;
  if (limits.max_response_tokens > 0)
    newRuntime.max_output_tokens = limits.max_response_tokens;
  if (limits.summary_trigger_tokens > 0)
    newRuntime.summary_trigger_tokens = limits.summary_trigger_tokens;

  int requestedLayers = runtimeConfig.gpu_layers;
  auto overrideIt = config.model_overrides.find(model.id);
  if (overrideIt != config.model_overrides.end() &&
      overrideIt->second.gpu_layers != -9999)
    requestedLayers = overrideIt->second.gpu_layers;

  int effectiveLayers = requestedLayers;
  if (effectiveLayers == -1)
    effectiveLayers = autoGpuLayersForModel(model);

  if (effectiveLayers < 0)
    effectiveLayers = 0;

  newRuntime.gpu_layers = effectiveLayers;

  try
  {
    auto uniqueLlm = ck::ai::Llm::open(newRuntime.model_path, newRuntime);
    uniqueLlm->set_system_prompt(systemPrompt_);
    runtimeConfig.model_path = newRuntime.model_path;
    runtimeConfig.max_output_tokens = newRuntime.max_output_tokens;
    runtimeConfig.context_window_tokens = newRuntime.context_window_tokens;
    runtimeConfig.summary_trigger_tokens = newRuntime.summary_trigger_tokens;
    runtimeConfig.gpu_layers = requestedLayers;
    runtimeConfig.threads = newRuntime.threads;
    config.runtime = runtimeConfig;
    return std::shared_ptr<ck::ai::Llm>(std::move(uniqueLlm));
  }
  catch (const std::exception &e)
  {
    messageBox(("Failed to load model: " + std::string(e.what())).c_str(),
               mfError | mfOKButton);
    return nullptr;
  }
}

void ChatApp::updateActiveModel()
{
  auto activeModels = modelManager_.get_active_models();
  std::optional<ck::ai::ModelInfo> selected;
  if (!activeModels.empty())
    selected = activeModels.front();

  if (selected)
    persistStringOption(ck::chat::kOptionActiveModelId, selected->id);
  else
    persistStringOption(ck::chat::kOptionActiveModelId, std::string());

  if (!selected)
  {
    {
      std::lock_guard<std::mutex> lock(llmMutex_);
      activeLlm_.reset();
      currentActiveModel_.reset();
      stopSequences_ = ck::chat::ChatSession::defaultStopSequences();
    }
    applyStopSequencesToWindows();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(llmMutex_);
    if (currentActiveModel_ && activeLlm_ &&
        currentActiveModel_->id == selected->id)
      return;
  }

  // Stop any existing model loading
  stopModelLoading();

  // Load model in background with progress dialog
  loadModelInBackground();
}

void ChatApp::loadModelInBackground()
{
  auto activeModels = modelManager_.get_active_models();
  if (activeModels.empty())
    return;

  const auto &model = activeModels.front();
  currentLoadingModelName_ = model.name;

  // Show loading dialog - make it much smaller
  TRect bounds = deskTop->getExtent();
  bounds.a.x = bounds.a.x + (bounds.b.x - bounds.a.x) / 2 - 20;
  bounds.a.y = bounds.a.y + (bounds.b.y - bounds.a.y) / 2 - 4;
  bounds.b.x = bounds.a.x + 40;
  bounds.b.y = bounds.a.y + 9;

  auto *loadingDialog = ModelLoadingProgressDialog::create(bounds, model.name);
  deskTop->insert(loadingDialog);
  auto *progressDialog =
      static_cast<ModelLoadingProgressDialog *>(loadingDialog);
  progressDialog->updateProgress("Initializing model loading...");

  // Start background loading thread
  modelLoadingInProgress_ = true;
  modelLoadingShouldStop_ = false;

  modelLoadingThread_ = std::thread([this, progressDialog, model]()
                                    {
    try {
      progressDialog->updateProgress("Opening model file: " + model.name +
                                     "...");

      if (modelLoadingShouldStop_) {
        progressDialog->setComplete(false, "Loading cancelled");
        return;
      }

      auto newLlm = loadModel(model);

      if (modelLoadingShouldStop_) {
        progressDialog->setComplete(false, "Loading cancelled");
        return;
      }

      if (newLlm) {
        progressDialog->updateProgress("Loading " + model.name +
                                       " into memory...");

        std::vector<std::string> resolvedStops =
            resolveStopSequencesForModel(model.id, model);
        std::vector<std::string> defaultStops =
            ck::chat::ChatSession::defaultStopSequences();

        {
          std::lock_guard<std::mutex> lock(llmMutex_);
          activeLlm_ = std::move(newLlm);
          currentActiveModel_ = model;
          conversationSettings_.max_context_tokens =
              runtimeConfig.context_window_tokens;
          conversationSettings_.max_response_tokens =
              runtimeConfig.max_output_tokens;
          conversationSettings_.summary_trigger_tokens =
              runtimeConfig.summary_trigger_tokens;
          stopSequences_ = std::move(resolvedStops);
        }

        progressDialog->setComplete(true, model.name + " loaded successfully!");

        // Apply settings to windows
        applyConversationSettingsToWindows();
        applyStopSequencesToWindows();
      } else {
        progressDialog->setComplete(false, "Failed to load model");
      }
    } catch (const std::exception &e) {
      progressDialog->setComplete(false, "Error: " + std::string(e.what()));
    }

    modelLoadingInProgress_ = false; });
}

void ChatApp::stopModelLoading()
{
  modelLoadingShouldStop_ = true;

  if (modelLoadingThread_.joinable())
  {
    modelLoadingThread_.join();
  }

  modelLoadingInProgress_ = false;
}
