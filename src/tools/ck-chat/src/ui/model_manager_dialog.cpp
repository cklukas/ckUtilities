#include "model_manager_dialog.hpp"
#include "../commands.hpp"

#include <tvision/tview.h>

#include <algorithm>
#include <atomic>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

ModelManagerDialog::ModelManagerDialog(TRect bounds,
                                       ck::ai::ModelManager &modelManager)
    : TDialog(bounds, "Manage Models"), modelManager_(modelManager),
      availableListBox_(nullptr), downloadedListBox_(nullptr),
      downloadButton_(nullptr), activateButton_(nullptr),
      deactivateButton_(nullptr), deleteButton_(nullptr),
      refreshButton_(nullptr), cancelButton_(nullptr), closeButton_(nullptr),
      statusLabel_(nullptr), statusText_("Ready"), downloadInProgress_(false),
      pendingUpdates_(false), currentDownloadModelId_(""),
      downloadShouldStop_(false), selectedAvailableIndex_(-1),
      selectedDownloadedIndex_(-1) {
  setupControls();
  refreshModelList();
}

ModelManagerDialog::~ModelManagerDialog() {
  // Stop any ongoing download
  stopBackgroundDownload();
}

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

  // Cancel download button (initially hidden)
  cancelButton_ =
      new TButton(TRect(54, 6, 64, 8), "~C~ancel", cmCancelDownload, bfNormal);
  insert(cancelButton_);

  closeButton_ =
      new TButton(TRect(66, 6, 74, 8), "~C~lose", cmCancel, bfNormal);
  insert(closeButton_);

  // Status label
  statusLabel_ = new TStaticText(TRect(2, 10, 70, 12), statusText_.c_str());
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
    case cmCancelDownload:
      stopBackgroundDownload();
      clearEvent(event);
      break;
    }
  }
}

void ModelManagerDialog::draw() {
  if (pendingUpdates_.load(std::memory_order_acquire))
    applyPendingUpdates();
  TDialog::draw();
}

void ModelManagerDialog::refreshModelList() {
  availableModels_ = modelManager_.get_available_models();
  downloadedModels_ = modelManager_.get_downloaded_models();
  updateModelList();
}

void ModelManagerDialog::updateModelList() {
  availableModelStrings_.clear();
  availableModelIds_.clear();
  downloadedModelStrings_.clear();
  downloadedModelIds_.clear();

  for (const auto &model : availableModels_) {
    std::string status;
    formatModelStatus(model, status);
    availableModelStrings_.push_back(model.name + " (" + status + ")");
    availableModelIds_.push_back(model.id);
  }

  for (const auto &model : downloadedModels_) {
    std::string status;
    formatModelStatus(model, status);
    downloadedModelStrings_.push_back(model.name + " (" + status + ")");
    downloadedModelIds_.push_back(model.id);
  }

  updateListBox(availableListBox_, availableModelStrings_);
  updateListBox(downloadedListBox_, downloadedModelStrings_);

  updateButtons();
}

void ModelManagerDialog::updateButtons() {
  bool hasAvailableModels = !availableModels_.empty();
  bool hasDownloadedModels = !downloadedModels_.empty();
  bool downloading = downloadInProgress_.load();

  if (downloadButton_)
    downloadButton_->setState(sfDisabled, !hasAvailableModels || downloading);

  if (activateButton_)
    activateButton_->setState(sfDisabled, !hasDownloadedModels || downloading);

  if (deactivateButton_)
    deactivateButton_->setState(sfDisabled,
                                !hasDownloadedModels || downloading);

  if (deleteButton_)
    deleteButton_->setState(sfDisabled, !hasDownloadedModels || downloading);

  if (refreshButton_)
    refreshButton_->setState(sfDisabled, downloading);

  if (cancelButton_) {
    cancelButton_->setState(sfDisabled, !downloading);
    cancelButton_->setState(sfVisible, downloading);
  }
}

void ModelManagerDialog::downloadSelectedModel() {
  if (availableModels_.empty() || downloadInProgress_)
    return;

  std::string modelId = availableModels_[0].id;
  showDownloadProgress(modelId);
}

void ModelManagerDialog::activateSelectedModel() {
  if (downloadedModels_.empty() || downloadInProgress_)
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
  if (downloadedModels_.empty() || downloadInProgress_)
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
  if (downloadedModels_.empty() || downloadInProgress_)
    return;

  std::string modelId = downloadedModels_[0].id;
  if (modelManager_.delete_model(modelId)) {
    queueStatusUpdate("Model deleted: " + modelId, true);
    queueButtonsUpdate();
  } else {
    queueStatusUpdate("Failed to delete model: " + modelId);
    TProgram::application->insertIdleHandler([this]() {
      messageBox("Failed to delete model", mfError | mfOKButton);
      return false;
    });
  }
}

void ModelManagerDialog::refreshModels() {
  if (downloadInProgress_)
    return;

  modelManager_.refresh_model_list();
  refreshModelList();
  queueStatusUpdate("Model list refreshed");
}

void ModelManagerDialog::showDownloadProgress(const std::string &modelId) {
  // Start background download
  startBackgroundDownload(modelId);
}

std::string ModelManagerDialog::formatBytes(std::size_t bytes) const {
  std::ostringstream oss;
  if (bytes < 1024)
    oss << bytes << " B";
  else if (bytes < 1024 * 1024)
    oss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
  else if (bytes < 1024ULL * 1024 * 1024)
    oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0))
        << " MB";
  else
    oss << std::fixed << std::setprecision(1)
        << (bytes / (1024.0 * 1024.0 * 1024.0)) << " GB";
  return oss.str();
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
  modelNameLabel_ = new TLabel(TRect(2, 2, 38, 3),
                               ("Downloading: " + modelName_).c_str(), nullptr);
  insert(modelNameLabel_);

  progressLabel_ =
      new TLabel(TRect(2, 4, 38, 5), "Progress: 0% (0 / 0 bytes)", nullptr);
  insert(progressLabel_);

  statusLabel_ =
      new TLabel(TRect(2, 6, 38, 7), "Starting download...", nullptr);
  insert(statusLabel_);

  closeButton_ =
      new TButton(TRect(14, 8, 24, 10), "~C~lose", cmCancel, bfNormal);
  insert(closeButton_);

  // Make close button disabled during download
  closeButton_->setState(sfDisabled, true);
}

void DownloadProgressDialog::handleEvent(TEvent &event) {
  TDialog::handleEvent(event);
}

std::string DownloadProgressDialog::formatBytes(std::size_t bytes) const {
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

  return oss.str();
}

void DownloadProgressDialog::draw() { TDialog::draw(); }

void DownloadProgressDialog::updateProgress(std::size_t downloaded,
                                            std::size_t total) {
  downloadedBytes_ = downloaded;
  totalBytes_ = total;

  // Update progress label
  if (progressLabel_) {
    std::string progressText =
        "Progress: " +
        std::to_string(static_cast<int>(
            (total > 0) ? (static_cast<double>(downloaded) / total) * 100.0
                        : 0.0)) +
        "% (" + formatBytes(downloaded) + " / " + formatBytes(total) + ")";
    progressLabel_->setText(progressText.c_str());
  }

  // Update status label
  if (statusLabel_) {
    statusLabel_->setText("Downloading...");
  }

  drawView();
}

void DownloadProgressDialog::setComplete(bool success,
                                         const std::string &message) {
  isComplete_ = true;
  isSuccess_ = success;
  statusMessage_ = message;

  // Update labels
  if (progressLabel_) {
    progressLabel_->setText("Progress: 100% (Complete)");
  }

  if (statusLabel_) {
    statusLabel_->setText(success ? "Download completed!" : "Download failed!");
  }

  // Enable close button
  if (closeButton_) {
    closeButton_->setState(sfDisabled, false);
  }

  drawView();
}

TDialog *DownloadProgressDialog::create(TRect bounds,
                                        const std::string &modelName) {
  return new DownloadProgressDialog(bounds, modelName);
}

void ModelManagerDialog::setStatusText(const std::string &text) {
  statusText_ = text;
  if (statusLabel_) {
    statusLabel_->setText(statusText_.c_str());
    statusLabel_->drawView();
  }
  drawView();
}

void ModelManagerDialog::updateListBox(TListBox *listBox,
                                       const std::vector<std::string> &items) {
  if (!listBox)
    return;

  auto *collection = new TStringCollection(10, 5);
  for (const auto &item : items) {
    collection->insert(new TString(item.c_str()));
  }
  listBox->newList(collection);
  listBox->drawView();
}

void ModelManagerDialog::queueStatusUpdate(const std::string &text,
                                           bool requestRefresh) {
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingStatusText_ = text;
    pendingStatusUpdate_ = true;
    pendingRefresh_ = pendingRefresh_ || requestRefresh;
  }
  pendingUpdates_.store(true, std::memory_order_release);
}

void ModelManagerDialog::queueButtonsUpdate() {
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pendingButtonsUpdate_ = true;
  }
  pendingUpdates_.store(true, std::memory_order_release);
}

void ModelManagerDialog::applyPendingUpdates() {
  std::string statusCopy;
  bool shouldUpdateStatus = false;
  bool shouldRefresh = false;
  bool updateButtonsFlag = false;

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    if (pendingStatusUpdate_) {
      statusCopy = pendingStatusText_;
      shouldUpdateStatus = true;
      pendingStatusUpdate_ = false;
    }
    if (pendingRefresh_) {
      shouldRefresh = true;
      pendingRefresh_ = false;
    }
    if (pendingButtonsUpdate_) {
      updateButtonsFlag = true;
      pendingButtonsUpdate_ = false;
    }
    pendingUpdates_.store(false, std::memory_order_release);
  }

  if (shouldUpdateStatus)
    setStatusText(statusCopy);
  if (updateButtonsFlag)
    updateButtons();
  if (shouldRefresh)
    refreshModelList();
}

void ModelManagerDialog::startBackgroundDownload(const std::string &modelId) {
  auto modelOpt = modelManager_.get_model_by_id(modelId);
  if (!modelOpt)
    return;

  // Stop any existing download
  stopBackgroundDownload();

  currentDownloadModelId_ = modelId;
  downloadInProgress_ = true;
  downloadShouldStop_ = false;

  queueButtonsUpdate();
  queueStatusUpdate("Starting download: " + modelOpt->name);

  // Start background thread
  downloadThread_ = std::thread([this, modelId]() {
    std::string errorMessage;
    bool success = modelManager_.download_model(
        modelId,
        [this](const ck::ai::ModelDownloadProgress &progress) {
          // Check if download should stop
          if (downloadShouldStop_.load()) {
            return;
          }

          if (progress.total_bytes > 0) {
            std::ostringstream oss;
            oss << "Downloading: " << progress.model_id << " - " << std::fixed
                << std::setprecision(1) << progress.progress_percentage << "% ("
                << formatBytes(progress.bytes_downloaded) << " / "
                << formatBytes(progress.total_bytes) << ")";
            queueStatusUpdate(oss.str());
          } else {
            queueStatusUpdate("Downloading: " + progress.model_id + " - " +
                              formatBytes(progress.bytes_downloaded) +
                              " received");
          }
        },
        &errorMessage);

    // Update UI on main thread
    TProgram::application->insertIdleHandler([this, success, errorMessage,
                                              modelId]() {
      downloadInProgress_ = false;
      currentDownloadModelId_.clear();

      if (success) {
        queueStatusUpdate("Download completed: " + modelId, true);
        refreshModelList(); // Only refresh on success
      } else {
        if (errorMessage.empty())
          errorMessage = "Download failed";
        queueStatusUpdate("Download failed: " + modelId + " - " + errorMessage);
      }

      queueButtonsUpdate();
      return false; // Remove this handler
    });
  });
}

void ModelManagerDialog::stopBackgroundDownload() {
  if (downloadInProgress_) {
    downloadShouldStop_ = true;

    if (downloadThread_.joinable()) {
      downloadThread_.join();
    }

    downloadInProgress_ = false;
    currentDownloadModelId_.clear();
    queueButtonsUpdate();
    queueStatusUpdate("Download cancelled");
  }
}
