#pragma once

#include "../../../../include/ck/ai/model_manager.hpp"

#include "../tvision_include.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ModelManagerDialog : public TDialog {
public:
  ModelManagerDialog(TRect bounds, ck::ai::ModelManager &modelManager);
  ~ModelManagerDialog();

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
  void startBackgroundDownload(const std::string &modelId);
  void stopBackgroundDownload();
  void formatModelSize(std::size_t bytes, std::string &result) const;
  void formatModelStatus(const ck::ai::ModelInfo &model,
                         std::string &result) const;
  void setStatusText(const std::string &text);
  void updateListBox(TListBox *listBox, const std::vector<std::string> &items);
  std::string formatBytes(std::size_t bytes) const;
  void queueStatusUpdate(const std::string &text, bool requestRefresh = false);
  void queueButtonsUpdate();
  void applyPendingUpdates();

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
  TButton *cancelButton_;
  TButton *closeButton_;
  TStaticText *statusLabel_;

  std::string statusText_;

  // Model list data
  std::vector<std::string> availableModelStrings_;
  std::vector<std::string> downloadedModelStrings_;
  std::vector<std::string> availableModelIds_;
  std::vector<std::string> downloadedModelIds_;

  std::atomic<bool> downloadInProgress_;
  std::atomic<bool> pendingUpdates_;
  std::mutex pendingMutex_;
  std::string pendingStatusText_;
  bool pendingStatusUpdate_ = false;
  bool pendingRefresh_ = false;
  bool pendingButtonsUpdate_ = false;
  std::thread downloadThread_;
  std::string currentDownloadModelId_;
  std::atomic<bool> downloadShouldStop_;

  int selectedAvailableIndex_;
  int selectedDownloadedIndex_;
};