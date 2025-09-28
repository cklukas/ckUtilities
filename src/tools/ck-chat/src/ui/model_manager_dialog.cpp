#include "model_manager_dialog.hpp"
#include "../commands.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

ModelManagerDialog::ModelManagerDialog(TRect bounds,
                                       ck::ai::ModelManager &modelManager)
    : TDialog(bounds, "Manage Models"), modelManager_(modelManager),
      selectedAvailableIndex_(-1), selectedDownloadedIndex_(-1) {
  setupControls();
  refreshModelList();
}

ModelManagerDialog::~ModelManagerDialog() {}

void ModelManagerDialog::setupControls() {
  // Create simple text display for now
  TLabel *infoLabel = new TLabel(TRect(2, 2, 70, 4),
                                 "Model Management - Basic Version", nullptr);
  insert(infoLabel);

  // Create buttons
  downloadButton_ =
      new TButton(TRect(2, 6, 12, 8), "~D~ownload", cmDownloadModel, bfDefault);
  insert(downloadButton_);

  activateButton_ =
      new TButton(TRect(14, 6, 22, 8), "~A~ctivate", cmActivateModel, bfNormal);
  insert(activateButton_);

  deactivateButton_ = new TButton(TRect(24, 6, 34, 8), "~D~eactivate",
                                  cmDeactivateModel, bfNormal);
  insert(deactivateButton_);

  deleteButton_ =
      new TButton(TRect(36, 6, 42, 8), "~D~elete", cmDeleteModel, bfNormal);
  insert(deleteButton_);

  refreshButton_ =
      new TButton(TRect(44, 6, 52, 8), "~R~efresh", cmRefreshModels, bfNormal);
  insert(refreshButton_);

  closeButton_ =
      new TButton(TRect(54, 6, 62, 8), "~C~lose", cmCancel, bfNormal);
  insert(closeButton_);

  // Status label
  statusLabel_ = new TLabel(TRect(2, 10, 70, 12), "Ready", nullptr);
  insert(statusLabel_);

  updateButtons();
}

void ModelManagerDialog::handleEvent(TEvent &event) {
  TDialog::handleEvent(event);

  if (event.what == evCommand) {
    switch (event.message.command) {
    case cmDownloadModel:
      downloadSelectedModel();
      clearEvent(event);
      break;
    case cmActivateModel:
      activateSelectedModel();
      clearEvent(event);
      break;
    case cmDeactivateModel:
      deactivateSelectedModel();
      clearEvent(event);
      break;
    case cmDeleteModel:
      deleteSelectedModel();
      clearEvent(event);
      break;
    case cmRefreshModels:
      refreshModels();
      clearEvent(event);
      break;
    }
  }
}

void ModelManagerDialog::draw() { TDialog::draw(); }

void ModelManagerDialog::refreshModelList() {
  availableModels_ = modelManager_.get_available_models();
  downloadedModels_ = modelManager_.get_downloaded_models();
  updateModelList();
}

void ModelManagerDialog::updateModelList() {
  // For now, just update the status with model counts
  std::string status =
      "Available: " + std::to_string(availableModels_.size()) +
      ", Downloaded: " + std::to_string(downloadedModels_.size());
  // Note: TLabel doesn't have setText, so we'll just show a simple message box
  // instead
  updateButtons();
}

void ModelManagerDialog::updateButtons() {
  // Enable/disable buttons based on available models
  bool hasAvailableModels = !availableModels_.empty();
  bool hasDownloadedModels = !downloadedModels_.empty();

  if (downloadButton_)
    downloadButton_->setState(sfDisabled, !hasAvailableModels);

  if (activateButton_)
    activateButton_->setState(sfDisabled, !hasDownloadedModels);

  if (deactivateButton_)
    deactivateButton_->setState(sfDisabled, !hasDownloadedModels);

  if (deleteButton_)
    deleteButton_->setState(sfDisabled, !hasDownloadedModels);
}

void ModelManagerDialog::downloadSelectedModel() {
  if (availableModels_.empty())
    return;

  // For now, download the first available model
  std::string modelId = availableModels_[0].id;
  showDownloadProgress(modelId);
}

void ModelManagerDialog::activateSelectedModel() {
  if (downloadedModels_.empty())
    return;

  // For now, activate the first downloaded model
  std::string modelId = downloadedModels_[0].id;
  if (modelManager_.activate_model(modelId)) {
    // Show success message
    messageBox("Model activated: " + modelId, mfInformation | mfOKButton);
    refreshModelList();
  } else {
    messageBox("Failed to activate model: " + modelId, mfError | mfOKButton);
  }
}

void ModelManagerDialog::deactivateSelectedModel() {
  if (downloadedModels_.empty())
    return;

  // For now, deactivate the first downloaded model
  std::string modelId = downloadedModels_[0].id;
  if (modelManager_.deactivate_model(modelId)) {
    messageBox("Model deactivated: " + modelId, mfInformation | mfOKButton);
    refreshModelList();
  } else {
    messageBox("Failed to deactivate model: " + modelId, mfError | mfOKButton);
  }
}

void ModelManagerDialog::deleteSelectedModel() {
  if (downloadedModels_.empty())
    return;

  // For now, delete the first downloaded model
  std::string modelId = downloadedModels_[0].id;
  if (modelManager_.delete_model(modelId)) {
    messageBox("Model deleted: " + modelId, mfInformation | mfOKButton);
    refreshModelList();
  } else {
    messageBox("Failed to delete model: " + modelId, mfError | mfOKButton);
  }
}

void ModelManagerDialog::refreshModels() {
  modelManager_.refresh_model_list();
  refreshModelList();
  messageBox("Model list refreshed", mfInformation | mfOKButton);
}

void ModelManagerDialog::showDownloadProgress(const std::string &modelId) {
  auto modelOpt = modelManager_.get_model_by_id(modelId);
  if (!modelOpt)
    return;

  // For now, just show a simple message
  messageBox("Starting download of: " + modelOpt->name,
             mfInformation | mfOKButton);

  // Start download in background (simplified)
  modelManager_.download_model(
      modelId, [](const ck::ai::ModelDownloadProgress &progress) {
        if (progress.is_complete) {
          // Download completed
        }
      });
}

void ModelManagerDialog::formatModelSize(std::size_t bytes,
                                         std::string &result) const {
  std::ostringstream oss;
  if (bytes < 1024)
    oss << bytes << " B";
  else if (bytes < 1024 * 1024)
    oss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
  else if (bytes < 1024 * 1024 * 1024)
    oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0))
        << " MB";
  else
    oss << std::fixed << std::setprecision(1)
        << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";

  result = oss.str();
}

void ModelManagerDialog::formatModelStatus(const ck::ai::ModelInfo &model,
                                           std::string &result) const {
  std::string sizeStr;
  formatModelSize(model.size_bytes, sizeStr);

  result = sizeStr;
  if (model.is_downloaded)
    result += " [Downloaded]";
  if (model.is_active)
    result += " [Active]";
}

TDialog *ModelManagerDialog::create(TRect bounds,
                                    ck::ai::ModelManager &modelManager) {
  return new ModelManagerDialog(bounds, modelManager);
}

// DownloadProgressDialog implementation

DownloadProgressDialog::DownloadProgressDialog(TRect bounds,
                                               const std::string &modelName)
    : TDialog(bounds, "Download Progress"), modelName_(modelName),
      downloadedBytes_(0), totalBytes_(0), isComplete_(false),
      isSuccess_(false) {
  setupControls();
}

DownloadProgressDialog::~DownloadProgressDialog() {}

void DownloadProgressDialog::setupControls() {
  modelNameLabel_ = new TLabel(TRect(2, 2, 30, 3),
                               ("Downloading: " + modelName_).c_str(), nullptr);
  insert(modelNameLabel_);

  progressLabel_ =
      new TLabel(TRect(2, 4, 30, 5), "Progress: 0% (0 / 0 bytes)", nullptr);
  insert(progressLabel_);

  statusLabel_ =
      new TLabel(TRect(2, 6, 30, 7), "Starting download...", nullptr);
  insert(statusLabel_);

  closeButton_ =
      new TButton(TRect(10, 8, 20, 10), "~C~lose", cmCancel, bfNormal);
  insert(closeButton_);
}

void DownloadProgressDialog::handleEvent(TEvent &event) {
  TDialog::handleEvent(event);
}

void DownloadProgressDialog::draw() { TDialog::draw(); }

void DownloadProgressDialog::updateProgress(std::size_t downloaded,
                                            std::size_t total) {
  downloadedBytes_ = downloaded;
  totalBytes_ = total;
  updateProgressDisplay();
}

void DownloadProgressDialog::setComplete(bool success,
                                         const std::string &message) {
  isComplete_ = true;
  isSuccess_ = success;
  statusMessage_ = message;
  updateProgressDisplay();
}

TDialog *DownloadProgressDialog::create(TRect bounds,
                                        const std::string &modelName) {
  return new DownloadProgressDialog(bounds, modelName);
}

void DownloadProgressDialog::updateProgressDisplay() {
  // For now, just show a simple message
  if (isComplete_) {
    messageBox("Download completed: " + statusMessage_,
               mfInformation | mfOKButton);
  } else {
    // Progress update - could be enhanced later
  }
}