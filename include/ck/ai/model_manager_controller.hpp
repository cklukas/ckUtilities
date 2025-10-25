#pragma once

#include "ck/ai/model_manager.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ck::ai {

/**
 * @brief Controller class that provides a clean interface between UI and
 * ModelManager business logic
 *
 * This class separates the business logic from UI-specific code, making it
 * easier to test and maintain the model management functionality independently
 * of the TurboVision UI.
 */
class ModelManagerController {
public:
  // Status update callback types
  using StatusCallback = std::function<void(const std::string &message)>;
  using ErrorCallback = std::function<void(const std::string &error)>;
  using ModelListUpdateCallback = std::function<void()>;

  explicit ModelManagerController(ModelManager &modelManager);

  // Callback registration
  void setStatusCallback(StatusCallback callback) {
    statusCallback_ = callback;
  }
  void setErrorCallback(ErrorCallback callback) { errorCallback_ = callback; }
  void setModelListUpdateCallback(ModelListUpdateCallback callback) {
    modelListUpdateCallback_ = callback;
  }

  // Model operations
  bool downloadModel(const std::string &modelId);
  bool activateModel(const std::string &modelId);
  bool deactivateModel(const std::string &modelId);
  bool deleteModel(const std::string &modelId);
  void refreshModels();

  // Model queries
  std::vector<ModelInfo> getAvailableModels() const;
  std::vector<ModelInfo> getDownloadedModels() const;
  std::vector<ModelInfo> getActiveModels() const;
  std::optional<ModelInfo> getModelById(const std::string &id) const;
  std::optional<ModelInfo> getActiveModel() const;

  // Model status
  bool isModelDownloaded(const std::string &modelId) const;
  bool isModelActive(const std::string &modelId) const;
  std::string getModelDisplayName(const ModelInfo &model) const;
  std::string getModelStatusText(const ModelInfo &model) const;
  std::string formatModelSize(std::size_t bytes) const;

  // Selection state management
  void setSelectedAvailableModel(int index);
  void setSelectedDownloadedModel(int index);
  void clearSelection();

  int getSelectedAvailableIndex() const { return selectedAvailableIndex_; }
  int getSelectedDownloadedIndex() const { return selectedDownloadedIndex_; }

  std::optional<ModelInfo> getSelectedAvailableModel() const;
  std::optional<ModelInfo> getSelectedDownloadedModel() const;

  // Validation
  bool canActivateSelected() const;
  bool canDeactivateSelected() const;
  bool canDeleteSelected() const;
  bool canDownloadSelected() const;

  // Actions on selected models
  bool downloadSelectedModel();
  bool activateSelectedModel();
  bool deactivateSelectedModel();
  bool deleteSelectedModel();

private:
  void reloadModelCaches();
  void notifyStatus(const std::string &message);
  void notifyError(const std::string &error);
  void notifyModelListUpdate();
  std::string formatSizeInMB(std::size_t bytes) const;

  ModelManager &modelManager_;

  // Callbacks
  StatusCallback statusCallback_;
  ErrorCallback errorCallback_;
  ModelListUpdateCallback modelListUpdateCallback_;

  // Selection state
  int selectedAvailableIndex_;
  int selectedDownloadedIndex_;
  std::vector<ModelInfo> cachedAvailableModels_;
  std::vector<ModelInfo> cachedDownloadedModels_;
};

} // namespace ck::ai
