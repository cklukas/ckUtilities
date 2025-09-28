#include "interactive_model_manager.hpp"
#include "../commands.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

InteractiveModelManagerDialog::InteractiveModelManagerDialog(
    TRect bounds, ck::ai::ModelManager &modelManager)
    : TDialog(bounds, "Manage Models"), modelManager_(modelManager),
      selectedAvailableIndex_(-1), selectedDownloadedIndex_(-1) {
  setupControls();
  refreshModelList();
}

InteractiveModelManagerDialog::~InteractiveModelManagerDialog() {}

void InteractiveModelManagerDialog::setupControls() {
  // Create available models list
  TRect availableRect(2, 2, 40, 15);
  availableListBox_ = new TListBox(availableRect, 1, nullptr);
  insert(availableListBox_);

  TLabel *availableLabel =
      new TLabel(TRect(2, 1, 20, 2), "Available Models", availableListBox_);
  insert(availableLabel);

  // Create downloaded models list
  TRect downloadedRect(42, 2, 80, 15);
  downloadedListBox_ = new TListBox(downloadedRect, 1, nullptr);
  insert(downloadedListBox_);

  TLabel *downloadedLabel =
      new TLabel(TRect(42, 1, 25, 2), "Downloaded Models", downloadedListBox_);
  insert(downloadedLabel);

  // Create buttons
  downloadButton_ = new TButton(TRect(2, 16, 12, 18), "~D~ownload",
                                cmDownloadModel, bfDefault);
  insert(downloadButton_);

  activateButton_ = new TButton(TRect(14, 16, 22, 18), "~A~ctivate",
                                cmActivateModel, bfNormal);
  insert(activateButton_);

  deactivateButton_ = new TButton(TRect(24, 16, 34, 18), "~D~eactivate",
                                  cmDeactivateModel, bfNormal);
  insert(deactivateButton_);

  deleteButton_ =
      new TButton(TRect(36, 16, 42, 18), "~D~elete", cmDeleteModel, bfNormal);
  insert(deleteButton_);

  refreshButton_ = new TButton(TRect(44, 16, 52, 18), "~R~efresh",
                               cmRefreshModels, bfNormal);
  insert(refreshButton_);

  infoButton_ = new TButton(TRect(54, 16, 62, 18), "~I~nfo", cmAbout, bfNormal);
  insert(infoButton_);

  closeButton_ =
      new TButton(TRect(64, 16, 72, 18), "~C~lose", cmCancel, bfNormal);
  insert(closeButton_);

  // Status label
  statusLabel_ = new TLabel(TRect(2, 19, 80, 20), "Ready", nullptr);
  insert(statusLabel_);

  updateButtons();
}

void InteractiveModelManagerDialog::handleEvent(TEvent &event) {
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
    case cmAbout:
      showModelInfo();
      clearEvent(event);
      break;
    }
  } else if (event.what == evBroadcast) {
    if (event.message.command == cmListItemSelected) {
      if (event.message.infoPtr == availableListBox_) {
        selectedAvailableIndex_ = availableListBox_->focused;
        selectedDownloadedIndex_ = -1;
        updateButtons();
      } else if (event.message.infoPtr == downloadedListBox_) {
        selectedDownloadedIndex_ = downloadedListBox_->focused;
        selectedAvailableIndex_ = -1;
        updateButtons();
      }
    }
  }
}

void InteractiveModelManagerDialog::draw() { TDialog::draw(); }

void InteractiveModelManagerDialog::refreshModelList() {
  availableModels_ = modelManager_.get_available_models();
  downloadedModels_ = modelManager_.get_downloaded_models();
  updateModelList();
}

void InteractiveModelManagerDialog::updateModelList() {
  // Update available models list
  availableModelStrings_.clear();
  availableModelIds_.clear();

  for (const auto &model : availableModels_) {
    std::string status;
    formatModelStatus(model, status);

    std::string modelStr = model.name + " (" + status + ")";
    availableModelStrings_.push_back(modelStr);
    availableModelIds_.push_back(model.id);
  }

  // Update downloaded models list
  downloadedModelStrings_.clear();
  downloadedModelIds_.clear();

  for (const auto &model : downloadedModels_) {
    std::string status;
    formatModelStatus(model, status);

    std::string modelStr = model.name + " (" + status + ")";
    downloadedModelStrings_.push_back(modelStr);
    downloadedModelIds_.push_back(model.id);
  }

  // Update list boxes
  if (availableListBox_) {
    auto *collection = new TStringCollection(10, 5);
    for (const auto &str : availableModelStrings_) {
      collection->insert(new TString(str.c_str()));
    }
    availableListBox_->newList(collection);
  }

  if (downloadedListBox_) {
    auto *collection = new TStringCollection(10, 5);
    for (const auto &str : downloadedModelStrings_) {
      collection->insert(new TString(str.c_str()));
    }
    downloadedListBox_->newList(collection);
  }

  updateButtons();
}

void InteractiveModelManagerDialog::updateButtons() {
  bool hasAvailableSelection =
      (selectedAvailableIndex_ >= 0 &&
       selectedAvailableIndex_ < availableModelIds_.size());
  bool hasDownloadedSelection =
      (selectedDownloadedIndex_ >= 0 &&
       selectedDownloadedIndex_ < downloadedModelIds_.size());

  if (downloadButton_)
    downloadButton_->setState(sfDisabled, !hasAvailableSelection);

  if (activateButton_)
    activateButton_->setState(sfDisabled, !hasDownloadedSelection);

  if (deactivateButton_)
    deactivateButton_->setState(sfDisabled, !hasDownloadedSelection);

  if (deleteButton_)
    deleteButton_->setState(sfDisabled, !hasDownloadedSelection);
}

void InteractiveModelManagerDialog::downloadSelectedModel() {
  if (selectedAvailableIndex_ < 0 ||
      selectedAvailableIndex_ >= availableModelIds_.size())
    return;

  std::string modelId = availableModelIds_[selectedAvailableIndex_];
  showDownloadProgress(modelId);
}

void InteractiveModelManagerDialog::activateSelectedModel() {
  if (selectedDownloadedIndex_ < 0 ||
      selectedDownloadedIndex_ >= downloadedModelIds_.size())
    return;

  std::string modelId = downloadedModelIds_[selectedDownloadedIndex_];
  if (modelManager_.activate_model(modelId)) {
    messageBox("Model activated: " + modelId, mfInformation | mfOKButton);
    refreshModelList();
  } else {
    messageBox("Failed to activate model: " + modelId, mfError | mfOKButton);
  }
}

void InteractiveModelManagerDialog::deactivateSelectedModel() {
  if (selectedDownloadedIndex_ < 0 ||
      selectedDownloadedIndex_ >= downloadedModelIds_.size())
    return;

  std::string modelId = downloadedModelIds_[selectedDownloadedIndex_];
  if (modelManager_.deactivate_model(modelId)) {
    messageBox("Model deactivated: " + modelId, mfInformation | mfOKButton);
    refreshModelList();
  } else {
    messageBox("Failed to deactivate model: " + modelId, mfError | mfOKButton);
  }
}

void InteractiveModelManagerDialog::deleteSelectedModel() {
  if (selectedDownloadedIndex_ < 0 ||
      selectedDownloadedIndex_ >= downloadedModelIds_.size())
    return;

  std::string modelId = downloadedModelIds_[selectedDownloadedIndex_];
  if (modelManager_.delete_model(modelId)) {
    messageBox("Model deleted: " + modelId, mfInformation | mfOKButton);
    refreshModelList();
  } else {
    messageBox("Failed to delete model: " + modelId, mfError | mfOKButton);
  }
}

void InteractiveModelManagerDialog::refreshModels() {
  modelManager_.refresh_model_list();
  refreshModelList();
  messageBox("Model list refreshed", mfInformation | mfOKButton);
}

void InteractiveModelManagerDialog::showDownloadProgress(
    const std::string &modelId) {
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
          messageBox("Download completed successfully!",
                     mfInformation | mfOKButton);
        }
      });
}

void InteractiveModelManagerDialog::showModelInfo() {
  refreshModelList();

  std::string info = "Model Management Info:\n\n";
  info += "Available Models: " + std::to_string(availableModels_.size()) + "\n";
  info +=
      "Downloaded Models: " + std::to_string(downloadedModels_.size()) + "\n\n";

  if (!availableModels_.empty()) {
    info += "Available Models:\n";
    for (const auto &model : availableModels_) {
      std::string sizeStr;
      formatModelSize(model.size_bytes, sizeStr);
      info += "- " + model.name + " (" + sizeStr + ")\n";
    }
  }

  if (!downloadedModels_.empty()) {
    info += "\nDownloaded Models:\n";
    for (const auto &model : downloadedModels_) {
      std::string sizeStr;
      formatModelSize(model.size_bytes, sizeStr);
      info += "- " + model.name + " (" + sizeStr + ")";
      if (model.is_active)
        info += " [ACTIVE]";
      info += "\n";
    }
  }

  messageBox(info.c_str(), mfInformation | mfOKButton);
}

void InteractiveModelManagerDialog::formatModelSize(std::size_t bytes,
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

void InteractiveModelManagerDialog::formatModelStatus(
    const ck::ai::ModelInfo &model, std::string &result) const {
  std::string sizeStr;
  formatModelSize(model.size_bytes, sizeStr);

  result = sizeStr;
  if (model.is_downloaded)
    result += " [Downloaded]";
  if (model.is_active)
    result += " [Active]";
}

TDialog *
InteractiveModelManagerDialog::create(TRect bounds,
                                      ck::ai::ModelManager &modelManager) {
  return new InteractiveModelManagerDialog(bounds, modelManager);
}
