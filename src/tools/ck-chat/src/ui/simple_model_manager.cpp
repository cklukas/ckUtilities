#include "simple_model_manager.hpp"
#include "../commands.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

SimpleModelManager::SimpleModelManager(ck::ai::ModelManager &modelManager)
    : modelManager_(modelManager) {
  refreshModelList();
}

void SimpleModelManager::showModelInfo() {
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

void SimpleModelManager::downloadFirstModel() {
  refreshModelList();

  if (availableModels_.empty()) {
    messageBox("No models available for download", mfError | mfOKButton);
    return;
  }

  const auto &model = availableModels_[0];
  std::string sizeStr;
  formatModelSize(model.size_bytes, sizeStr);

  std::string message =
      "Starting download of: " + model.name + "\nSize: " + sizeStr;
  messageBox(message.c_str(), mfInformation | mfOKButton);

  // Start download
  modelManager_.download_model(
      model.id, [](const ck::ai::ModelDownloadProgress &progress) {
        if (progress.is_complete) {
          messageBox("Download completed successfully!",
                     mfInformation | mfOKButton);
        }
      });
}

void SimpleModelManager::activateFirstModel() {
  refreshModelList();

  if (downloadedModels_.empty()) {
    messageBox("No downloaded models to activate", mfError | mfOKButton);
    return;
  }

  const auto &model = downloadedModels_[0];
  if (modelManager_.activate_model(model.id)) {
    messageBox("Model activated: " + model.name, mfInformation | mfOKButton);
  } else {
    messageBox("Failed to activate model: " + model.name, mfError | mfOKButton);
  }
}

void SimpleModelManager::deactivateFirstModel() {
  refreshModelList();

  if (downloadedModels_.empty()) {
    messageBox("No downloaded models to deactivate", mfError | mfOKButton);
    return;
  }

  const auto &model = downloadedModels_[0];
  if (modelManager_.deactivate_model(model.id)) {
    messageBox("Model deactivated: " + model.name, mfInformation | mfOKButton);
  } else {
    messageBox("Failed to deactivate model: " + model.name,
               mfError | mfOKButton);
  }
}

void SimpleModelManager::deleteFirstModel() {
  refreshModelList();

  if (downloadedModels_.empty()) {
    messageBox("No downloaded models to delete", mfError | mfOKButton);
    return;
  }

  const auto &model = downloadedModels_[0];
  if (modelManager_.delete_model(model.id)) {
    messageBox("Model deleted: " + model.name, mfInformation | mfOKButton);
  } else {
    messageBox("Failed to delete model: " + model.name, mfError | mfOKButton);
  }
}

void SimpleModelManager::refreshModels() {
  modelManager_.refresh_model_list();
  refreshModelList();
  messageBox("Model list refreshed", mfInformation | mfOKButton);
}

void SimpleModelManager::refreshModelList() {
  availableModels_ = modelManager_.get_available_models();
  downloadedModels_ = modelManager_.get_downloaded_models();
}

void SimpleModelManager::formatModelSize(std::size_t bytes,
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
