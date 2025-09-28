#pragma once

#include "../../../../include/ck/ai/config.hpp"
#include "../../../../include/ck/ai/model_manager.hpp"

#include "../tvision_include.hpp"

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

  const ck::ai::RuntimeConfig &runtime() const noexcept {
    return runtimeConfig;
  }
  ck::ai::ModelManager &modelManager() noexcept { return modelManager_; }

private:
  void openChatWindow();
  void showAboutDialog();
  void showModelManagerDialog();
  void selectModel(int modelIndex);

  std::vector<ChatWindow *> windows;
  int nextWindowNumber = 1;
  ck::ai::Config config;
  ck::ai::RuntimeConfig runtimeConfig;
  ck::ai::ModelManager modelManager_;
};
