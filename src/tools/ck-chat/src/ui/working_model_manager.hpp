#pragma once

#include "../../../../include/ck/ai/model_manager.hpp"

#include "../tvision_include.hpp"

#include <memory>
#include <string>
#include <vector>

class WorkingModelManagerDialog : public TDialog {
public:
  WorkingModelManagerDialog(TRect bounds, ck::ai::ModelManager &modelManager);
  ~WorkingModelManagerDialog();

  virtual void handleEvent(TEvent &event) override;
  virtual void draw() override;

  static TDialog *create(TRect bounds, ck::ai::ModelManager &modelManager);

private:
  void setupControls();
  void refreshModelList();
  void updateButtons();
  void downloadSelectedModel();
  void activateSelectedModel();
  void deactivateSelectedModel();
  void deleteSelectedModel();
  void refreshModels();
  void showModelInfo();
  void formatModelSize(std::size_t bytes, std::string &result) const;

  ck::ai::ModelManager &modelManager_;
  std::vector<ck::ai::ModelInfo> availableModels_;
  std::vector<ck::ai::ModelInfo> downloadedModels_;

  // UI Controls
  TButton *downloadButton_;
  TButton *activateButton_;
  TButton *deactivateButton_;
  TButton *deleteButton_;
  TButton *refreshButton_;
  TButton *infoButton_;
  TButton *closeButton_;
  TLabel *statusLabel_;

  int selectedModelIndex_;
};
