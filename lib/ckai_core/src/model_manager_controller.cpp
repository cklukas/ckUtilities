#include "ck/ai/model_manager_controller.hpp"

#include <iomanip>
#include <sstream>

namespace ck::ai {

ModelManagerController::ModelManagerController(ModelManager &modelManager)
    : modelManager_(modelManager), selectedAvailableIndex_(-1),
      selectedDownloadedIndex_(-1) {
  refreshModels();
}

bool ModelManagerController::downloadModel(const std::string &modelId) {
  auto model = getModelById(modelId);
  if (!model) {
    notifyError("Model not found: " + modelId);
    return false;
  }

  if (model->is_downloaded) {
    notifyError("Model is already downloaded: " + model->name);
    return false;
  }

  notifyStatus("Starting download of " + model->name + "...");

  try {
    // Use ModelManager's download functionality with real progress updates
    bool success = modelManager_.download_model(
        modelId, [this](const ModelDownloadProgress &progress) {
          if (progress.total_bytes > 0) {
            double percent = (static_cast<double>(progress.bytes_downloaded) /
                              progress.total_bytes) *
                             100.0;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << percent << "% ("
                << formatSizeInMB(progress.bytes_downloaded) << " / "
                << formatSizeInMB(progress.total_bytes) << ")";
            notifyStatus("Downloading... " + oss.str());
          } else {
            notifyStatus("Downloading... " +
                         formatSizeInMB(progress.bytes_downloaded) +
                         " received");
          }

          if (progress.is_complete) {
            notifyStatus("Download completed successfully!");
          }
        });

    if (success) {
      notifyStatus("Model downloaded: " + model->name);
      refreshModels(); // Refresh to update the downloaded list
      return true;
    } else {
      notifyError("Failed to download model: " + model->name);
      return false;
    }
  } catch (const std::exception &e) {
    notifyError("Download error: " + std::string(e.what()));
    return false;
  }
}

bool ModelManagerController::activateModel(const std::string &modelId) {
  if (!isModelDownloaded(modelId)) {
    notifyError("Model is not downloaded: " + modelId);
    return false;
  }

  if (modelManager_.activate_model(modelId)) {
    auto model = getModelById(modelId);
    std::string modelName = model ? model->name : modelId;
    notifyStatus("Model activated: " + modelName);
    notifyModelListUpdate();
    return true;
  } else {
    notifyError("Failed to activate model: " + modelId);
    return false;
  }
}

bool ModelManagerController::deactivateModel(const std::string &modelId) {
  if (!isModelDownloaded(modelId)) {
    notifyError("Model is not downloaded: " + modelId);
    return false;
  }

  if (modelManager_.deactivate_model(modelId)) {
    auto model = getModelById(modelId);
    std::string modelName = model ? model->name : modelId;
    notifyStatus("Model deactivated: " + modelName);
    notifyModelListUpdate();
    return true;
  } else {
    notifyError("Failed to deactivate model: " + modelId);
    return false;
  }
}

bool ModelManagerController::deleteModel(const std::string &modelId) {
  if (!isModelDownloaded(modelId)) {
    notifyError("Model is not downloaded: " + modelId);
    return false;
  }

  auto model = getModelById(modelId);
  std::string modelName = model ? model->name : modelId;

  if (modelManager_.delete_model(modelId)) {
    notifyStatus("Model deleted: " + modelName);
    clearSelection(); // Clear selection since model was deleted
    notifyModelListUpdate();
    return true;
  } else {
    notifyError("Failed to delete model: " + modelName);
    return false;
  }
}

void ModelManagerController::refreshModels() {
  try {
    modelManager_.refresh_model_list();
    cachedAvailableModels_ = modelManager_.get_available_models();
    cachedDownloadedModels_ = modelManager_.get_downloaded_models();
    notifyStatus("Model list refreshed");
    notifyModelListUpdate();
  } catch (const std::exception &e) {
    notifyError("Error refreshing models: " + std::string(e.what()));
  } catch (...) {
    notifyError("Unknown error refreshing models");
  }
}

std::vector<ModelInfo> ModelManagerController::getAvailableModels() const {
  return cachedAvailableModels_;
}

std::vector<ModelInfo> ModelManagerController::getDownloadedModels() const {
  return cachedDownloadedModels_;
}

std::vector<ModelInfo> ModelManagerController::getActiveModels() const {
  return modelManager_.get_active_models();
}

std::optional<ModelInfo>
ModelManagerController::getModelById(const std::string &id) const {
  return modelManager_.get_model_by_id(id);
}

std::optional<ModelInfo> ModelManagerController::getActiveModel() const {
  return modelManager_.get_active_model();
}

bool ModelManagerController::isModelDownloaded(
    const std::string &modelId) const {
  return modelManager_.is_model_downloaded(modelId);
}

bool ModelManagerController::isModelActive(const std::string &modelId) const {
  return modelManager_.is_model_active(modelId);
}

std::string
ModelManagerController::getModelDisplayName(const ModelInfo &model) const {
  return model.name;
}

std::string
ModelManagerController::getModelStatusText(const ModelInfo &model) const {
  std::string statusText = formatModelSize(model.size_bytes);

  if (model.is_active) {
    statusText += " [X]";
  } else if (model.is_downloaded) {
    statusText += " [ ]";
  }

  return statusText;
}

std::string ModelManagerController::formatModelSize(std::size_t bytes) const {
  std::ostringstream oss;

  if (bytes == 0) {
    oss << "Unknown";
  } else if (bytes < 1024) {
    oss << bytes << " B";
  } else if (bytes < 1024 * 1024) {
    oss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
  } else if (bytes < 1024ULL * 1024 * 1024) {
    oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0))
        << " MB";
  } else {
    oss << std::fixed << std::setprecision(1)
        << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
  }

  return oss.str();
}

void ModelManagerController::setSelectedAvailableModel(int index) {
  selectedAvailableIndex_ = index;
  selectedDownloadedIndex_ = -1; // Clear other selection
}

void ModelManagerController::setSelectedDownloadedModel(int index) {
  selectedDownloadedIndex_ = index;
  selectedAvailableIndex_ = -1; // Clear other selection
}

void ModelManagerController::clearSelection() {
  selectedAvailableIndex_ = -1;
  selectedDownloadedIndex_ = -1;
}

std::optional<ModelInfo>
ModelManagerController::getSelectedAvailableModel() const {
  if (selectedAvailableIndex_ >= 0 &&
      selectedAvailableIndex_ <
          static_cast<int>(cachedAvailableModels_.size())) {
    return cachedAvailableModels_[selectedAvailableIndex_];
  }
  return std::nullopt;
}

std::optional<ModelInfo>
ModelManagerController::getSelectedDownloadedModel() const {
  if (selectedDownloadedIndex_ >= 0 &&
      selectedDownloadedIndex_ <
          static_cast<int>(cachedDownloadedModels_.size())) {
    return cachedDownloadedModels_[selectedDownloadedIndex_];
  }
  return std::nullopt;
}

bool ModelManagerController::canActivateSelected() const {
  auto selected = getSelectedDownloadedModel();
  return selected && !selected->is_active;
}

bool ModelManagerController::canDeactivateSelected() const {
  auto selected = getSelectedDownloadedModel();
  return selected && selected->is_active;
}

bool ModelManagerController::canDeleteSelected() const {
  auto selected = getSelectedDownloadedModel();
  return selected.has_value();
}

bool ModelManagerController::canDownloadSelected() const {
  auto selected = getSelectedAvailableModel();
  return selected && !selected->is_downloaded;
}

bool ModelManagerController::downloadSelectedModel() {
  auto selected = getSelectedAvailableModel();
  if (!selected) {
    notifyError("Please select a model from the available list first");
    return false;
  }

  return downloadModel(selected->id);
}

bool ModelManagerController::activateSelectedModel() {
  auto selected = getSelectedDownloadedModel();
  if (!selected) {
    notifyError("Please select a model from the downloaded list first");
    return false;
  }

  return activateModel(selected->id);
}

bool ModelManagerController::deactivateSelectedModel() {
  auto selected = getSelectedDownloadedModel();
  if (!selected) {
    notifyError("Please select a model from the downloaded list first");
    return false;
  }

  return deactivateModel(selected->id);
}

bool ModelManagerController::deleteSelectedModel() {
  auto selected = getSelectedDownloadedModel();
  if (!selected) {
    notifyError("Please select a model from the downloaded list first");
    return false;
  }

  return deleteModel(selected->id);
}

void ModelManagerController::notifyStatus(const std::string &message) {
  if (statusCallback_) {
    statusCallback_(message);
  }
}

void ModelManagerController::notifyError(const std::string &error) {
  if (errorCallback_) {
    errorCallback_(error);
  }
}

void ModelManagerController::notifyModelListUpdate() {
  if (modelListUpdateCallback_) {
    modelListUpdateCallback_();
  }
}

std::string ModelManagerController::formatSizeInMB(std::size_t bytes) const {
  std::ostringstream oss;
  if (bytes < 1024 * 1024) {
    oss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
  } else if (bytes < 1024ULL * 1024 * 1024) {
    oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0))
        << " MB";
  } else {
    oss << std::fixed << std::setprecision(1)
        << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
  }
  return oss.str();
}

} // namespace ck::ai
