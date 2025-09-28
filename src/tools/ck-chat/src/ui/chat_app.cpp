#include "chat_app.hpp"
#include "../commands.hpp"
#include "chat_window.hpp"
#include "ck/about_dialog.hpp"
#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"
#include "ck/app_info.hpp"
#include "ck/launcher.hpp"
#include "model_dialog.hpp"

#include <algorithm>
#include <cstdlib>

namespace {
const ck::appinfo::ToolInfo &tool_info() {
  return ck::appinfo::requireTool("ck-chat");
}

ck::ai::RuntimeConfig runtime_from_config(const ck::ai::Config &config) {
  ck::ai::RuntimeConfig runtime = config.runtime;
  if (runtime.model_path.empty())
    runtime.model_path = "stub-model.gguf";
  return runtime;
}
} // namespace

ChatApp::ChatApp(int, char **)
    : TProgInit(&ChatApp::initStatusLine, nullptr, &TApplication::initDeskTop),
      TApplication() {
  config = ck::ai::ConfigLoader::load_or_default();
  runtimeConfig = runtime_from_config(config);

  // Set up the menu bar manually since we need access to modelManager_
  if (deskTop) {
    TRect menuRect = deskTop->getExtent();
    menuRect.b.y = menuRect.a.y + 1;
    TMenuBar *menuBar = initMenuBar(menuRect);
    if (menuBar) {
      insert(menuBar);
    }
  }

  openChatWindow();
}

void ChatApp::registerWindow(ChatWindow *window) { windows.push_back(window); }

void ChatApp::unregisterWindow(ChatWindow *window) {
  auto it = std::remove(windows.begin(), windows.end(), window);
  windows.erase(it, windows.end());
}

void ChatApp::openChatWindow() {
  if (!deskTop)
    return;

  TRect bounds = deskTop->getExtent();
  bounds.grow(-2, -1);
  if (bounds.b.x <= bounds.a.x + 10 || bounds.b.y <= bounds.a.y + 5)
    bounds = TRect(0, 0, 70, 20);

  auto *window = new ChatWindow(*this, bounds, nextWindowNumber++);
  deskTop->insert(window);
  window->select();
}

void ChatApp::handleEvent(TEvent &event) {
  TApplication::handleEvent(event);
  if (event.what == evCommand) {
    switch (event.message.command) {
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
    case cmNoOp: // "No active models" - do nothing
      clearEvent(event);
      break;
    default:
      break;
    }
  }
}

void ChatApp::idle() {
  TApplication::idle();
  for (auto *window : windows) {
    if (window)
      window->processPendingResponses();
  }
}

TMenuBar *ChatApp::initMenuBar(TRect r) {
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

  auto downloadedModels = modelManager_.get_downloaded_models();
  menuDownloadedModels_ = downloadedModels;
  auto activeInfo = activeModelInfo();

  if (downloadedModels.empty()) {
    modelsMenu +
        *new TMenuItem("~N~o downloaded models", cmNoOp, kbNoKey, hcNoContext);
  } else {
    TMenuItem *defaultItem = nullptr;
    for (size_t i = 0; i < downloadedModels.size() && i < 10; ++i) {
      const auto &model = downloadedModels[i];
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

  modelsMenu + newLine() +
      *new TMenuItem("~M~anage models...", cmManageModels, kbNoKey,
                     hcNoContext);

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

TStatusLine *ChatApp::initStatusLine(TRect r) {
  r.a.y = r.b.y - 1;

  auto *newItem = new TStatusItem("~Ctrl-N~ New Chat", kbCtrlN, cmNewChat);
  auto *closeItem = new TStatusItem("~Alt-F3~ Close", kbAltF3, cmClose);
  newItem->next = closeItem;
  TStatusItem *tail = closeItem;

  if (ck::launcher::launchedFromCkLauncher()) {
    auto *returnItem =
        new TStatusItem("~Ctrl-L~ Return", kbCtrlL, cmReturnToLauncher);
    tail->next = returnItem;
    tail = returnItem;
  }

  auto *quitItem = new TStatusItem("~Alt-X~ Quit", kbAltX, cmQuit);
  tail->next = quitItem;

  return new TStatusLine(r, *new TStatusDef(0, 0xFFFF, newItem));
}

void ChatApp::showAboutDialog() {
  const auto &info = tool_info();
#ifdef CK_CHAT_VERSION
  ck::ui::showAboutDialog(info.executable, CK_CHAT_VERSION,
                          info.aboutDescription);
#else
  ck::ui::showAboutDialog(info.executable, "dev", info.aboutDescription);
#endif
}

void ChatApp::showModelManagerDialog() {
  // Fixed dialog size based on content needs
  TRect bounds(5, 3, 105, 28);

  auto *dialog = new ModelDialog(bounds, modelManager_, this);
  if (dialog) {
    deskTop->insert(dialog);
    dialog->select();
  }
}

void ChatApp::refreshModelsMenu() { rebuildMenuBar(); }

void ChatApp::selectModel(int modelIndex) {
  if (menuDownloadedModels_.empty() ||
      modelIndex < 0 ||
      modelIndex >= static_cast<int>(menuDownloadedModels_.size())) {
    messageBox("Invalid model selection", mfError | mfOKButton);
    return;
  }

  const auto &model = menuDownloadedModels_[modelIndex];
  if (!model.is_downloaded) {
    messageBox("Model is not downloaded", mfError | mfOKButton);
    return;
  }

  if (!modelManager_.activate_model(model.id)) {
    messageBox("Failed to activate model: " + model.name, mfError | mfOKButton);
    return;
  }

  handleModelManagerChange();
}

void ChatApp::handleModelManagerChange() {
  updateActiveModel();
  rebuildMenuBar();
}

std::shared_ptr<ck::ai::Llm> ChatApp::getActiveLlm() {
  std::lock_guard<std::mutex> lock(llmMutex_);
  return activeLlm_;
}

std::optional<ck::ai::ModelInfo> ChatApp::activeModelInfo() const {
  std::lock_guard<std::mutex> lock(llmMutex_);
  return currentActiveModel_;
}

void ChatApp::rebuildMenuBar() {
  if (!deskTop)
    return;

  TRect bounds;
  if (TProgram::menuBar)
    bounds = TProgram::menuBar->getBounds();
  else {
    bounds = deskTop->getExtent();
    bounds.b.y = bounds.a.y + 1;
  }

  if (TProgram::menuBar) {
    TMenuBar *oldBar = TProgram::menuBar;
    remove(oldBar);
    TObject::destroy(oldBar);
  }

  TMenuBar *newBar = initMenuBar(bounds);
  if (newBar) {
    insert(newBar);
    TProgram::menuBar = newBar;
    newBar->drawView();
  }
}

std::shared_ptr<ck::ai::Llm>
ChatApp::loadModel(const ck::ai::ModelInfo &model) {
  std::filesystem::path modelPath = model.local_path;
  if (modelPath.empty())
    modelPath = modelManager_.get_model_path(model.id);

  if (modelPath.empty() || !std::filesystem::exists(modelPath)) {
    messageBox(("Model file not found: " + model.name).c_str(),
               mfError | mfOKButton);
    return nullptr;
  }

  ck::ai::RuntimeConfig newRuntime = runtimeConfig;
  newRuntime.model_path = modelPath.string();
  if (newRuntime.threads <= 0)
    newRuntime.threads = static_cast<int>(std::thread::hardware_concurrency());

  try {
    auto uniqueLlm = ck::ai::Llm::open(newRuntime.model_path, newRuntime);
    uniqueLlm->set_system_prompt(systemPrompt_);
    runtimeConfig = newRuntime;
    return std::shared_ptr<ck::ai::Llm>(std::move(uniqueLlm));
  } catch (const std::exception &e) {
    messageBox(("Failed to load model: " + std::string(e.what())).c_str(),
               mfError | mfOKButton);
    return nullptr;
  }
}

void ChatApp::updateActiveModel() {
  auto activeModels = modelManager_.get_active_models();
  std::optional<ck::ai::ModelInfo> selected;
  if (!activeModels.empty())
    selected = activeModels.front();

  if (!selected) {
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
  if (newLlm) {
    activeLlm_ = std::move(newLlm);
    currentActiveModel_ = selected;
  } else {
    activeLlm_.reset();
    currentActiveModel_.reset();
  }
}
