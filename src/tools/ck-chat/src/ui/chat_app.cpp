#include "chat_app.hpp"
#include "../commands.hpp"
#include "chat_window.hpp"
#include "ck/about_dialog.hpp"
#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"
#include "ck/app_info.hpp"
#include "ck/launcher.hpp"
#include "model_dialog.hpp"
#include "prompt_dialog.hpp"

#include <algorithm>
#include <cstdlib>

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

ChatApp::ChatApp(int, char **)
    : TProgInit(&ChatApp::initStatusLine, nullptr, &TApplication::initDeskTop),
      TApplication()
{
  config = ck::ai::ConfigLoader::load_or_default();
  runtimeConfig = runtime_from_config(config);

  conversationSettings_.max_context_tokens = runtimeConfig.context_window_tokens;
  conversationSettings_.summary_trigger_tokens = runtimeConfig.summary_trigger_tokens;
  conversationSettings_.max_response_tokens = runtimeConfig.max_output_tokens;

  if (auto prompt = promptManager_.get_active_prompt())
    systemPrompt_ = prompt->message;

  updateActiveModel();
  handlePromptManagerChange();

  openChatWindow();
  applyConversationSettingsToWindows();
}

void ChatApp::registerWindow(ChatWindow *window) { windows.push_back(window); }

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
  TApplication::idle();
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
                       *new TMenuItem("~N~ew Chat...", cmNewChat, kbCtrlN,
                                      hcNoContext, "Ctrl-N") +
                       *new TMenuItem("~C~lose Window", cmClose, kbAltF3,
                                      hcNoContext, "Alt-F3") +
                       newLine();
  if (ck::launcher::launchedFromCkLauncher())
    fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher,
                              kbCtrlL, hcNoContext, "Ctrl-L");
  fileMenu + *new TMenuItem("E~x~it", cmQuit, kbAltX, hcNoContext, "Alt-X");

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
      auto *item = new TMenuItem(menuText.c_str(), command, kbNoKey, hcNoContext);
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
      *new TMenuItem("Manage ~M~odels...", cmManageModels, kbF2,
                     hcNoContext, "F2");
  modelsMenu + *new TMenuItem("Manage ~P~rompts...", cmManagePrompts, kbF3,
                              hcNoContext, "F3");

  TMenuItem &menuChain =
      fileMenu + modelsMenu + *new TSubMenu("~W~indows", hcNoContext) +
      *new TMenuItem("~R~esize/Move", cmResize, kbCtrlF5, hcNoContext,
                     "Ctrl-F5") +
      *new TMenuItem("~Z~oom", cmZoom, kbF5, hcNoContext, "F5") +
      *new TMenuItem("~N~ext", cmNext, kbF6, hcNoContext, "F6") +
      *new TMenuItem("~C~lose", cmClose, kbAltF3, hcNoContext, "Alt-F3") +
      *new TMenuItem("~T~ile", cmTile, kbNoKey) +
      *new TMenuItem("C~a~scade", cmCascade, kbNoKey) +
      *new TSubMenu("~H~elp", hcNoContext) +
      *new TMenuItem("~A~bout", cmAbout, kbF1, hcNoContext, "F1");

  return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
}

TStatusLine *ChatApp::initStatusLine(TRect r)
{
  r.a.y = r.b.y - 1;

  auto *newItem = new TStatusItem("~Ctrl-N~ New Chat", kbCtrlN, cmNewChat);
  auto *closeItem = new TStatusItem("~Alt-F3~ Close", kbAltF3, cmClose);
  newItem->next = closeItem;
  TStatusItem *tail = closeItem;

  if (ck::launcher::launchedFromCkLauncher())
  {
    auto *returnItem =
        new TStatusItem("~Ctrl-L~ Return", kbCtrlL, cmReturnToLauncher);
    tail->next = returnItem;
    tail = returnItem;
  }

  auto *quitItem = new TStatusItem("~Alt-X~ Quit", kbAltX, cmQuit);
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
  if (menuDownloadedModels_.empty() ||
      modelIndex < 0 ||
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

void ChatApp::updateConversationSettings(std::size_t maxResponseTokens,
                                         std::size_t summaryThresholdTokens)
{
  if (maxResponseTokens == 0)
  {
    maxResponseTokens = runtimeConfig.max_output_tokens > 0
                             ? runtimeConfig.max_output_tokens
                             : 512;
  }

  conversationSettings_.max_context_tokens = runtimeConfig.context_window_tokens;
  conversationSettings_.max_response_tokens = maxResponseTokens;
  conversationSettings_.summary_trigger_tokens = summaryThresholdTokens;

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
  }

  refreshWindowTitles();
}

void ChatApp::handlePromptManagerChange()
{
  auto activePrompt = promptManager_.get_active_prompt();
  if (activePrompt)
    systemPrompt_ = activePrompt->message;

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

  if (!selected)
  {
    std::lock_guard<std::mutex> lock(llmMutex_);
    activeLlm_.reset();
    currentActiveModel_.reset();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(llmMutex_);
    if (currentActiveModel_ && activeLlm_ &&
        currentActiveModel_->id == selected->id)
      return;
  }

  auto newLlm = loadModel(*selected);
  std::lock_guard<std::mutex> lock(llmMutex_);
  if (newLlm)
  {
    activeLlm_ = std::move(newLlm);
    currentActiveModel_ = selected;
    conversationSettings_.max_context_tokens = runtimeConfig.context_window_tokens;
    applyConversationSettingsToWindows();
  }
  else
  {
    activeLlm_.reset();
    currentActiveModel_.reset();
  }
}
