#pragma once

#include "../../../../include/ck/ai/model_manager.hpp"

#include "../tvision_include.hpp"

#include <memory>
#include <string>
#include <vector>

class InteractiveModelManagerDialog : public TDialog {
public:
  InteractiveModelManagerDialog(TRect bounds,
                                ck::ai::ModelManager &modelManager);
  ~InteractiveModelManagerDialog();

  virtual void handleEvent(TEvent &event) override;
  virtual void draw() override;

  static TDialog *create(TRect bounds, ck::ai::ModelManager &modelManager);

private:
  void setupControls();
  void refreshModelList();
  void updateModelList();
  void updateButtons();
  void downloadSelectedModel();
  void activateSelectedModel();
  void deactivateSelectedModel();
  void deleteSelectedModel();
  void refreshModels();
  void showDownloadProgress(const std::string &modelId);
  void formatModelSize(std::size_t bytes, std::string &result) const;
  void formatModelStatus(const ck::ai::ModelInfo &model,
                         std::string &result) const;
  void showModelInfo();

  ck::ai::ModelManager &modelManager_;
  std::vector<ck::ai::ModelInfo> availableModels_;
  std::vector<ck::ai::ModelInfo> downloadedModels_;

  // UI Controls
  TListBox *availableListBox_;
  TListBox *downloadedListBox_;
  TButton *downloadButton_;
  TButton *activateButton_;
  TButton *deactivateButton_;
  TButton *deleteButton_;
  TButton *refreshButton_;
  TButton *infoButton_;
  TButton *closeButton_;
  TLabel *statusLabel_;

  // Model list data
  std::vector<std::string> availableModelStrings_;
  std::vector<std::string> downloadedModelStrings_;
  std::vector<std::string> availableModelIds_;
  std::vector<std::string> downloadedModelIds_;

  int selectedAvailableIndex_;
  int selectedDownloadedIndex_;
};
