#include "proper_model_dialog.hpp"
#include "../commands.hpp"
#include "chat_app.hpp"

#include <algorithm>
#include <iomanip>
#include <cstring>
#include <sstream>

namespace {
constexpr const char *kDefaultStatusMessage =
    "Ready - Select a model from the lists above";

class StatusLabel : public TLabel {
public:
  StatusLabel(const TRect &bounds, std::string &backingText)
      : TLabel(bounds, backingText.c_str(), nullptr), backingText_(&backingText) {
  }

  void syncText() { text = backingText_ ? backingText_->c_str() : nullptr; }

  void draw() override {
    syncText();
    TLabel::draw();
  }

private:
  std::string *backingText_;
};

char *duplicateString(const std::string &value) {
  char *copy = new char[value.size() + 1];
  std::memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}
}

ProperModelDialog::ProperModelDialog(TRect bounds,
                                     ck::ai::ModelManager &modelManager,
                                     ChatApp *app)
    : TWindowInit(&ProperModelDialog::initFrame),
      TDialog(bounds, "Manage Models"),
      controller_(
          std::make_unique<ck::ai::ModelManagerController>(modelManager)),
      chatApp_(app) {

  // Set up controller callbacks for UI updates
  controller_->setStatusCallback(
      [this](const std::string &msg) { updateStatusLabel(msg); });

  controller_->setErrorCallback([this](const std::string &error) {
    updateStatusLabel("ERROR: " + error);
  });

  controller_->setModelListUpdateCallback([this]() {
    updateModelLists();
    // Note: Menu update notification temporarily disabled to prevent crashes
    // TODO: Re-enable once TurboVision menu rebuilding is implemented safely
    // if (chatApp_) {
    //   chatApp_->refreshModelsMenu();
    // }
  });

  setupControls();
  updateModelLists();
}

ProperModelDialog::~ProperModelDialog() {}

void ProperModelDialog::setupControls() {
  // Create available models list (left side)
  TRect availableRect(2, 3, 46, 17);
  availableListBox_ = new TListBox(availableRect, 1, nullptr);
  insert(availableListBox_);

  TLabel *availableLabel =
      new TLabel(TRect(2, 2, 46, 3), "Available Models (Click to Download)",
                 availableListBox_);
  insert(availableLabel);

  // Create downloaded models list (right side)
  TRect downloadedRect(48, 3, 92, 17);
  downloadedListBox_ = new TListBox(downloadedRect, 1, nullptr);
  insert(downloadedListBox_);

  TLabel *downloadedLabel =
      new TLabel(TRect(48, 2, 92, 3), "Downloaded Models (Click to Manage)",
                 downloadedListBox_);
  insert(downloadedLabel);

  // Create buttons with proper widths
  downloadButton_ = new TButton(TRect(2, 19, 16, 21), "~D~ownload",
                                cmDownloadModel, bfDefault);
  insert(downloadButton_);

  activateButton_ = new TButton(TRect(18, 19, 30, 21), "~A~ctivate",
                                cmActivateModel, bfNormal);
  insert(activateButton_);

  deactivateButton_ = new TButton(TRect(32, 19, 46, 21), "~D~eactivate",
                                  cmDeactivateModel, bfNormal);
  insert(deactivateButton_);

  deleteButton_ =
      new TButton(TRect(48, 19, 58, 21), "~D~elete", cmDeleteModel, bfNormal);
  insert(deleteButton_);

  refreshButton_ = new TButton(TRect(60, 19, 72, 21), "~R~efresh",
                               cmRefreshModels, bfNormal);
  insert(refreshButton_);

  infoButton_ = new TButton(TRect(74, 19, 84, 21), "~I~nfo", cmAbout, bfNormal);
  insert(infoButton_);

  closeButton_ =
      new TButton(TRect(86, 19, 96, 21), "~C~lose", cmClose, bfNormal);
  insert(closeButton_);

  // Status label
  statusText_ = kDefaultStatusMessage;
  statusLabel_ = new StatusLabel(TRect(2, 22, 92, 23), statusText_);
  insert(statusLabel_);

  detailStatusText_.clear();
  detailStatusLabel_ = new StatusLabel(TRect(2, 23, 92, 24), detailStatusText_);
  insert(detailStatusLabel_);

  updateButtons();
}

void ProperModelDialog::handleEvent(TEvent &event) {
  TDialog::handleEvent(event);

  if (event.what == evCommand) {
    switch (event.message.command) {
    case cmDownloadModel:
      // Update selection based on current focus before action
      if (availableListBox_->focused >= 0) {
        setAvailableSelectionFromListIndex(availableListBox_->focused);
      }
      updateStatusForSelection();
      controller_->downloadSelectedModel();
      clearEvent(event);
      break;
    case cmActivateModel:
      // Update selection based on current focus before action
      if (downloadedListBox_->focused >= 0) {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
      }
      updateStatusForSelection();
      controller_->activateSelectedModel();
      clearEvent(event);
      break;
    case cmDeactivateModel:
      // Update selection based on current focus before action
      if (downloadedListBox_->focused >= 0) {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
      }
      updateStatusForSelection();
      controller_->deactivateSelectedModel();
      clearEvent(event);
      break;
    case cmDeleteModel:
      // Update selection based on current focus before action
      if (downloadedListBox_->focused >= 0) {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
      }
      updateStatusForSelection();
      controller_->deleteSelectedModel();
      clearEvent(event);
      break;
    case cmRefreshModels:
      controller_->refreshModels();
      clearEvent(event);
      break;
    case cmAbout:
      // Update selection based on current focus before showing info
      if (downloadedListBox_->focused >= 0) {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
        updateStatusForSelection();
        auto selected = controller_->getSelectedDownloadedModel();
        if (selected) {
          std::string modelName = selected->name;
          std::string modelSize =
              controller_->formatModelSize(selected->size_bytes);
          std::string infoMsg = "Model: " + modelName + " (" + modelSize + ")";
          showStatusMessage(infoMsg);
        } else {
          showStatusMessage("No downloaded model selected");
        }
      } else if (availableListBox_->focused >= 0) {
        setAvailableSelectionFromListIndex(availableListBox_->focused);
        updateStatusForSelection();
        auto selected = controller_->getSelectedAvailableModel();
        if (selected) {
          std::string modelName = selected->name;
          std::string modelSize =
              controller_->formatModelSize(selected->size_bytes);
          std::string infoMsg = "Model: " + modelName + " (" + modelSize + ")";
          showStatusMessage(infoMsg);
        } else {
          showStatusMessage("No available model selected");
        }
      } else {
        showStatusMessage("Select a model from either list to see details");
      }
      clearEvent(event);
      break;
    case cmClose:
      close();
      clearEvent(event);
      break;
    }
  } else if (event.what == evBroadcast) {
    if (event.message.command == cmListItemSelected) {
      if (event.message.infoPtr == availableListBox_) {
        setAvailableSelectionFromListIndex(availableListBox_->focused);
        updateStatusForSelection();
        updateButtons();
      } else if (event.message.infoPtr == downloadedListBox_) {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
        updateStatusForSelection();
        updateButtons();
      }
    }
  }

  syncSelectionFromLists();
}

void ProperModelDialog::draw() { TDialog::draw(); }

// This method is no longer needed as controller handles model refresh

void ProperModelDialog::updateModelLists() {
  // Get data from controller
  auto availableModels = controller_->getAvailableModels();
  auto downloadedModels = controller_->getDownloadedModels();

  int selectedAvailableIndex = controller_->getSelectedAvailableIndex();
  int selectedDownloadedIndex = controller_->getSelectedDownloadedIndex();
  int availableListSelection = -1;
  int downloadedListSelection = -1;

  // Clear existing strings
  availableModelStrings_.clear();
  downloadedModelStrings_.clear();
  availableModelIndexMap_.clear();
  downloadedModelIndexMap_.clear();

  struct DisplayEntry {
    std::string display;
    int index;
  };

  std::vector<DisplayEntry> availableEntries;
  availableEntries.reserve(availableModels.size());
  for (std::size_t i = 0; i < availableModels.size(); ++i) {
    const auto &model = availableModels[i];
    std::string sizeStr = controller_->formatModelSize(model.size_bytes);
    std::string displayText =
        controller_->getModelDisplayName(model) + " (" + sizeStr + ")";
    availableEntries.push_back({displayText, static_cast<int>(i)});
  }

  std::stable_sort(availableEntries.begin(), availableEntries.end(),
                   [](const DisplayEntry &a, const DisplayEntry &b) {
                     return a.display < b.display;
                   });

  for (const auto &entry : availableEntries) {
    availableModelStrings_.push_back(entry.display);
    availableModelIndexMap_.push_back(entry.index);
    if (entry.index == selectedAvailableIndex)
      availableListSelection =
          static_cast<int>(availableModelStrings_.size()) - 1;
  }

  std::vector<DisplayEntry> downloadedEntries;
  downloadedEntries.reserve(downloadedModels.size());
  for (std::size_t i = 0; i < downloadedModels.size(); ++i) {
    const auto &model = downloadedModels[i];
    std::string statusText = controller_->getModelStatusText(model);
    std::string displayText =
        controller_->getModelDisplayName(model) + " " + statusText;
    downloadedEntries.push_back({displayText, static_cast<int>(i)});
  }

  std::stable_sort(downloadedEntries.begin(), downloadedEntries.end(),
                   [](const DisplayEntry &a, const DisplayEntry &b) {
                     return a.display < b.display;
                   });

  for (const auto &entry : downloadedEntries) {
    downloadedModelStrings_.push_back(entry.display);
    downloadedModelIndexMap_.push_back(entry.index);
    if (entry.index == selectedDownloadedIndex)
      downloadedListSelection =
          static_cast<int>(downloadedModelStrings_.size()) - 1;
  }

  // Update TurboVision list boxes
  updateListBox(availableListBox_, availableModelStrings_);
  updateListBox(downloadedListBox_, downloadedModelStrings_);

  if (availableListBox_ && availableListSelection >= 0)
    availableListBox_->focusItem(availableListSelection);

  if (downloadedListBox_ && downloadedListSelection >= 0)
    downloadedListBox_->focusItem(downloadedListSelection);

  updateButtons();
  syncSelectionFromLists();
}

void ProperModelDialog::updateButtons() {
  // Use controller for validation instead of local state
  if (downloadButton_)
    downloadButton_->setState(sfDisabled, !controller_->canDownloadSelected());

  if (activateButton_)
    activateButton_->setState(sfDisabled, !controller_->canActivateSelected());

  if (deactivateButton_)
    deactivateButton_->setState(sfDisabled,
                                !controller_->canDeactivateSelected());

  if (deleteButton_)
    deleteButton_->setState(sfDisabled, !controller_->canDeleteSelected());
}

void ProperModelDialog::updateStatusForSelection() {
  if (!controller_) {
    updateStatusLabel(kDefaultStatusMessage);
    updateDetailLabel("");
    return;
  }

  if (auto selected = controller_->getSelectedDownloadedModel()) {
    updateStatusLabel(buildModelInfoLine(*selected, true));
    updateDetailLabel(selected->description);
    return;
  }

  if (auto selected = controller_->getSelectedAvailableModel()) {
    updateStatusLabel(buildModelInfoLine(*selected, false));
    updateDetailLabel(selected->description);
    return;
  }

  updateStatusLabel(kDefaultStatusMessage);
  updateDetailLabel("");
}

void ProperModelDialog::syncSelectionFromLists() {
  if (!controller_)
    return;

  bool changed = false;

  if (availableListBox_ && (availableListBox_->state & sfFocused)) {
    int focusedIndex = availableListBox_->focused;
    if (focusedIndex >= 0 &&
        focusedIndex < static_cast<int>(availableModelIndexMap_.size())) {
      int mappedIndex = availableModelIndexMap_[focusedIndex];
      if (mappedIndex != controller_->getSelectedAvailableIndex()) {
        controller_->setSelectedAvailableModel(mappedIndex);
        changed = true;
      }
    }
  } else if (downloadedListBox_ && (downloadedListBox_->state & sfFocused)) {
    int focusedIndex = downloadedListBox_->focused;
    if (focusedIndex >= 0 &&
        focusedIndex < static_cast<int>(downloadedModelIndexMap_.size())) {
      int mappedIndex = downloadedModelIndexMap_[focusedIndex];
      if (mappedIndex != controller_->getSelectedDownloadedIndex()) {
        controller_->setSelectedDownloadedModel(mappedIndex);
        changed = true;
      }
    }
  } else {
    if (controller_->getSelectedAvailableIndex() != -1 ||
        controller_->getSelectedDownloadedIndex() != -1) {
      controller_->clearSelection();
      changed = true;
    }
  }

  if (changed) {
    updateButtons();
    updateStatusForSelection();
  }
}

std::string ProperModelDialog::buildModelInfoLine(
    const ck::ai::ModelInfo &model, bool fromDownloadedList) const {
  std::ostringstream oss;
  oss << model.name;

  if (!model.id.empty() && model.id != model.name) {
    oss << " | ID: " << model.id;
  }

  if (controller_) {
    oss << " | " << controller_->formatModelSize(model.size_bytes);
  }

  if (!model.category.empty()) {
    oss << " | " << model.category;
  }

  if (!model.hardware_requirements.empty()) {
    oss << " | HW: " << model.hardware_requirements;
  }

  if (fromDownloadedList) {
    if (model.is_active) {
      oss << " | Active";
    }
    if (!model.local_path.empty()) {
      oss << " | " << model.local_path.filename().string();
    }
  } else if (model.is_downloaded) {
    oss << " | Downloaded";
  }

  if (!model.description.empty()) {
    std::string description = model.description;
    constexpr std::size_t kMaxDescriptionLength = 80;
    if (description.size() > kMaxDescriptionLength) {
      description = description.substr(0, kMaxDescriptionLength - 3) + "...";
    }
    oss << " | " << description;
  }

  return oss.str();
}

void ProperModelDialog::showStatusMessage(const std::string &message) {
  // Create a copy to ensure string lifetime
  std::string safeCopy = message;
  messageBox(safeCopy.c_str(), mfInformation | mfOKButton);
}

void ProperModelDialog::showErrorMessage(const std::string &error) {
  // Create a copy to ensure string lifetime
  std::string safeCopy = error;
  messageBox(safeCopy.c_str(), mfError | mfOKButton);
}

void ProperModelDialog::updateStatusLabel(const std::string &message) {
  if (statusText_ == message)
    return;

  statusText_ = message;

  if (statusLabel_) {
    static_cast<StatusLabel *>(statusLabel_)->syncText();
    statusLabel_->drawView();
  } else {
    drawView();
  }
}

void ProperModelDialog::updateDetailLabel(const std::string &message) {
  if (detailStatusText_ == message)
    return;

  detailStatusText_ = message;

  if (detailStatusLabel_) {
    static_cast<StatusLabel *>(detailStatusLabel_)->syncText();
    detailStatusLabel_->drawView();
  }
}

void ProperModelDialog::updateListBox(TListBox *listBox,
                                      const std::vector<std::string> &items) {
  if (!listBox)
    return;

  auto *collection = new TStringCollection(10, 5);
  for (const auto &item : items) {
    collection->insert(duplicateString(item));
  }
  listBox->newList(collection);
}

void ProperModelDialog::setAvailableSelectionFromListIndex(int listIndex) {
  if (!controller_)
    return;

  if (listIndex >= 0 &&
      listIndex < static_cast<int>(availableModelIndexMap_.size())) {
    controller_->setSelectedAvailableModel(
        availableModelIndexMap_[listIndex]);
  }
}

void ProperModelDialog::setDownloadedSelectionFromListIndex(int listIndex) {
  if (!controller_)
    return;

  if (listIndex >= 0 &&
      listIndex < static_cast<int>(downloadedModelIndexMap_.size())) {
    controller_->setSelectedDownloadedModel(
        downloadedModelIndexMap_[listIndex]);
  }
}

// All business logic methods removed - now handled by ModelManagerController

TDialog *ProperModelDialog::create(TRect bounds,
                                   ck::ai::ModelManager &modelManager) {
  return new ProperModelDialog(bounds, modelManager, nullptr);
}
