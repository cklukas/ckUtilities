#include "model_dialog.hpp"
#include "../commands.hpp"
#include "chat_app.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <tvision/util.h>

namespace
{
  constexpr const char *kDefaultStatusMessage =
      "Ready - Select a model from the lists above";

  class StatusLabel : public TLabel
  {
  public:
    StatusLabel(const TRect &bounds, std::string &backingText)
        : TLabel(bounds, backingText.c_str(), nullptr), backingText_(&backingText)
    {
    }

    void update()
    {
      if (text)
        delete[] (char *)text;
      if (backingText_)
        text = newStr(TStringView(*backingText_));
      else
        text = nullptr;
    }

    void draw() override
    {
      update();
      TLabel::draw();
    }

  private:
    std::string *backingText_;
  };

  char *duplicateString(const std::string &value)
  {
    char *copy = new char[value.size() + 1];
    std::memcpy(copy, value.c_str(), value.size() + 1);
    return copy;
  }
}

ModelDialog::ModelDialog(TRect bounds,
                         ck::ai::ModelManager &modelManager,
                         ChatApp *app)
    : TWindowInit(&ModelDialog::initFrame),
      TDialog(bounds, "Manage Models"),
      controller_(
          std::make_unique<ck::ai::ModelManagerController>(modelManager)),
      chatApp_(app)
{

  // Set up controller callbacks for UI updates
  controller_->setStatusCallback(
      [this](const std::string &msg)
      {
        updateStatusLabel(msg);
        updateDetailLabel("");
      });

  controller_->setErrorCallback([this](const std::string &error)
                                {
    updateStatusLabel("ERROR: " + error);
    updateDetailLabel(""); });

  controller_->setModelListUpdateCallback([this]()
                                          {
    updateModelLists();
    if (chatApp_)
      chatApp_->handleModelManagerChange(); });

  setupControls();
  updateModelLists();
  if (chatApp_)
    chatApp_->handleModelManagerChange();
}

ModelDialog::~ModelDialog()
{
  if (chatApp_)
    chatApp_->handleModelManagerChange();
}

void ModelDialog::setupControls()
{
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
  contextWindowInput_ = new TInputLine(TRect(74, 21, 92, 22), 10);
  insert(contextWindowInput_);
  insert(new TLabel(TRect(58, 21, 74, 22), "Ctx max:", contextWindowInput_));

  contextInfoText_.clear();
  contextInfoLabel_ = new StatusLabel(TRect(2, 22, 58, 23), contextInfoText_);
  insert(contextInfoLabel_);

  responseTokensInput_ = new TInputLine(TRect(74, 22, 92, 23), 10);
  insert(responseTokensInput_);
  insert(new TLabel(TRect(58, 22, 74, 23), "Resp max:", responseTokensInput_));

  summaryThresholdInput_ = new TInputLine(TRect(74, 23, 92, 24), 10);
  insert(summaryThresholdInput_);
  insert(new TLabel(TRect(58, 23, 74, 24), "Summ. thr.:", summaryThresholdInput_));

  gpuLayersInput_ = new TInputLine(TRect(74, 24, 92, 25), 10);
  insert(gpuLayersInput_);
  insert(new TLabel(TRect(58, 24, 74, 25), "GPU layers:", gpuLayersInput_));

  applySettingsButton_ =
      new TButton(TRect(73, 26, 93, 28), "~A~pply", cmApplyRuntimeSettings, bfNormal);
  insert(applySettingsButton_);

  statusText_ = kDefaultStatusMessage;
  statusLabel_ = new StatusLabel(TRect(2, 26, 73, 27), statusText_);
  insert(statusLabel_);

  detailStatusText_ = "Tip: 'auto' uses heuristics for GPU layers.";
  detailStatusLabel_ = new StatusLabel(TRect(2, 27, 73, 28), detailStatusText_);
  insert(detailStatusLabel_);

  updateButtons();
  refreshRuntimeSettingsDisplay();
}

void ModelDialog::handleEvent(TEvent &event)
{
  TDialog::handleEvent(event);

  if (event.what == evCommand)
  {
    switch (event.message.command)
    {
    case cmDownloadModel:
      // Update selection based on current focus before action
      if (availableListBox_->focused >= 0)
      {
        setAvailableSelectionFromListIndex(availableListBox_->focused);
      }
      updateStatusForSelection();
      controller_->downloadSelectedModel();
      clearEvent(event);
      break;
    case cmActivateModel:
      // Update selection based on current focus before action
      if (downloadedListBox_->focused >= 0)
      {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
      }
      updateStatusForSelection();
      controller_->activateSelectedModel();
      clearEvent(event);
      break;
    case cmDeactivateModel:
      // Update selection based on current focus before action
      if (downloadedListBox_->focused >= 0)
      {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
      }
      updateStatusForSelection();
      controller_->deactivateSelectedModel();
      clearEvent(event);
      break;
    case cmDeleteModel:
      // Update selection based on current focus before action
      if (downloadedListBox_->focused >= 0)
      {
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
      updateStatusForSelection();
      if (auto selected = controller_->getSelectedDownloadedModel())
      {
        showStatusMessage(formatDetailedInfo(*selected));
      }
      else if (auto selected = controller_->getSelectedAvailableModel())
      {
        showStatusMessage(formatDetailedInfo(*selected));
      }
      else
      {
        showStatusMessage("Select a model from either list to see details");
      }
      clearEvent(event);
      break;
    case cmApplyRuntimeSettings:
      applyRuntimeSettings();
      clearEvent(event);
      break;
    case cmClose:
      close();
      clearEvent(event);
      break;
    }
  }
  else if (event.what == evBroadcast)
  {
    if (event.message.command == cmListItemSelected)
    {
      if (event.message.infoPtr == availableListBox_)
      {
        setAvailableSelectionFromListIndex(availableListBox_->focused);
        updateStatusForSelection();
        updateButtons();
      }
      else if (event.message.infoPtr == downloadedListBox_)
      {
        setDownloadedSelectionFromListIndex(downloadedListBox_->focused);
        updateStatusForSelection();
        updateButtons();
        refreshRuntimeSettingsDisplay();
      }
    }
  }

  syncSelectionFromLists();
}

void ModelDialog::draw() { TDialog::draw(); }

// This method is no longer needed as controller handles model refresh

void ModelDialog::updateModelLists()
{
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

  struct DisplayEntry
  {
    std::string display;
    int index;
  };

  std::vector<DisplayEntry> availableEntries;
  availableEntries.reserve(availableModels.size());
  for (std::size_t i = 0; i < availableModels.size(); ++i)
  {
    const auto &model = availableModels[i];
    std::string sizeStr = controller_->formatModelSize(model.size_bytes);
    std::string displayText =
        controller_->getModelDisplayName(model) + " (" + sizeStr + ")";
    availableEntries.push_back({displayText, static_cast<int>(i)});
  }

  std::stable_sort(availableEntries.begin(), availableEntries.end(),
                   [](const DisplayEntry &a, const DisplayEntry &b)
                   {
                     return a.display < b.display;
                   });

  for (const auto &entry : availableEntries)
  {
    availableModelStrings_.push_back(entry.display);
    availableModelIndexMap_.push_back(entry.index);
    if (entry.index == selectedAvailableIndex)
      availableListSelection =
          static_cast<int>(availableModelStrings_.size()) - 1;
  }

  std::vector<DisplayEntry> downloadedEntries;
  downloadedEntries.reserve(downloadedModels.size());
  for (std::size_t i = 0; i < downloadedModels.size(); ++i)
  {
    const auto &model = downloadedModels[i];
    std::string statusText = controller_->getModelStatusText(model);
    std::string displayText =
        controller_->getModelDisplayName(model) + " " + statusText;
    downloadedEntries.push_back({displayText, static_cast<int>(i)});
  }

  std::stable_sort(downloadedEntries.begin(), downloadedEntries.end(),
                   [](const DisplayEntry &a, const DisplayEntry &b)
                   {
                     return a.display < b.display;
                   });

  for (const auto &entry : downloadedEntries)
  {
    downloadedModelStrings_.push_back(entry.display);
    downloadedModelIndexMap_.push_back(entry.index);
    if (entry.index == selectedDownloadedIndex)
      downloadedListSelection =
          static_cast<int>(downloadedModelStrings_.size()) - 1;
  }

  // Update TurboVision list boxes
  updateListBox(availableListBox_, availableModelStrings_);
  updateListBox(downloadedListBox_, downloadedModelStrings_);

  if (availableListBox_)
    availableListBox_->drawView();

  if (downloadedListBox_)
    downloadedListBox_->drawView();

  if (availableListBox_ && availableListSelection >= 0)
    availableListBox_->focusItem(availableListSelection);

  if (downloadedListBox_ && downloadedListSelection >= 0)
    downloadedListBox_->focusItem(downloadedListSelection);

  updateButtons();
  syncSelectionFromLists();
}

void ModelDialog::updateButtons()
{
  auto availableSelected = controller_->getSelectedAvailableModel();
  auto downloadedSelected = controller_->getSelectedDownloadedModel();

  const bool canDownload = availableSelected && !availableSelected->is_downloaded;
  const bool canActivate = downloadedSelected && !downloadedSelected->is_active;
  const bool canDeactivate = downloadedSelected && downloadedSelected->is_active;
  const bool canDelete = downloadedSelected.has_value();
  const bool canShowInfo = availableSelected.has_value() || downloadedSelected.has_value();

  auto updateButtonState = [](TButton *button, bool enabled)
  {
    if (!button)
      return;
    button->setState(sfDisabled, !enabled);
    button->drawView();
  };

  updateButtonState(downloadButton_, canDownload);
  updateButtonState(activateButton_, canActivate);
  updateButtonState(deactivateButton_, canDeactivate);
  updateButtonState(deleteButton_, canDelete);
  updateButtonState(infoButton_, canShowInfo);
  updateButtonState(applySettingsButton_, true);
}

void ModelDialog::updateStatusForSelection()
{
  if (!controller_)
  {
    updateStatusLabel(kDefaultStatusMessage);
    updateDetailLabel("");
    return;
  }

  if (auto selected = controller_->getSelectedDownloadedModel())
  {
    updateStatusLabel(buildModelInfoLine(*selected, true));
    updateDetailLabel(selected->description);
    refreshRuntimeSettingsDisplay();
    return;
  }

  if (auto selected = controller_->getSelectedAvailableModel())
  {
    updateStatusLabel(buildModelInfoLine(*selected, false));
    updateDetailLabel(selected->description);
    refreshRuntimeSettingsDisplay();
    return;
  }

  updateStatusLabel(kDefaultStatusMessage);
  updateDetailLabel("");
  refreshRuntimeSettingsDisplay();
}

void ModelDialog::syncSelectionFromLists()
{
  if (!controller_)
    return;

  bool changed = false;

  if (availableListBox_ && (availableListBox_->state & sfFocused))
  {
    int focusedIndex = availableListBox_->focused;
    if (focusedIndex >= 0 &&
        focusedIndex < static_cast<int>(availableModelIndexMap_.size()))
    {
      int mappedIndex = availableModelIndexMap_[focusedIndex];
      if (mappedIndex != controller_->getSelectedAvailableIndex())
      {
        controller_->setSelectedAvailableModel(mappedIndex);
        changed = true;
      }
    }
  }
  else if (downloadedListBox_ && (downloadedListBox_->state & sfFocused))
  {
    int focusedIndex = downloadedListBox_->focused;
    if (focusedIndex >= 0 &&
        focusedIndex < static_cast<int>(downloadedModelIndexMap_.size()))
    {
      int mappedIndex = downloadedModelIndexMap_[focusedIndex];
      if (mappedIndex != controller_->getSelectedDownloadedIndex())
      {
        controller_->setSelectedDownloadedModel(mappedIndex);
        changed = true;
      }
    }
  }
  else
  {
    if (controller_->getSelectedAvailableIndex() != -1 ||
        controller_->getSelectedDownloadedIndex() != -1)
    {
      controller_->clearSelection();
      changed = true;
    }
  }

  if (changed)
  {
    updateButtons();
    updateStatusForSelection();
  }
}

std::string ModelDialog::buildModelInfoLine(
    const ck::ai::ModelInfo &model, bool fromDownloadedList) const
{
  std::ostringstream oss;

  const std::string &identifier = model.id.empty() ? model.name : model.id;
  oss << identifier;

  if (controller_)
  {
    oss << " | " << controller_->formatModelSize(model.size_bytes);
  }

  if (!model.category.empty())
  {
    oss << " | " << model.category;
  }

  if (!model.hardware_requirements.empty())
  {
    oss << " | HW: " << model.hardware_requirements;
  }

  if (chatApp_)
  {
    int layers = chatApp_->gpuLayersForModel(model.id);
    oss << " | GPU: " << (layers == -1 ? std::string("auto") : std::to_string(layers));
  }

  if (fromDownloadedList)
  {
    if (model.is_active)
    {
      oss << " | Active";
    }
    if (!model.local_path.empty())
    {
      oss << " | " << model.local_path.filename().string();
    }
  }
  else if (model.is_downloaded)
  {
    oss << " | Downloaded";
  }

  return oss.str();
}

std::string ModelDialog::formatDetailedInfo(
    const ck::ai::ModelInfo &model) const
{
  std::ostringstream oss;
  oss << "Name: " << model.name << "\n";
  if (!model.id.empty())
    oss << "ID: " << model.id << "\n";
  if (!model.description.empty())
    oss << "Description: " << model.description << "\n";
  oss << "Size: " << controller_->formatModelSize(model.size_bytes) << "\n";
  if (!model.category.empty())
    oss << "Category: " << model.category << "\n";
  if (!model.hardware_requirements.empty())
    oss << "Hardware: " << model.hardware_requirements << "\n";
  if (!model.download_url.empty())
    oss << "Download: " << model.download_url << "\n";
  if (!model.filename.empty())
    oss << "Filename: " << model.filename << "\n";
  if (!model.local_path.empty())
    oss << "Local Path: " << model.local_path << "\n";
  oss << "Downloaded: " << (model.is_downloaded ? "yes" : "no") << "\n";
  oss << "Active: " << (model.is_active ? "yes" : "no") << "\n";
  if (chatApp_)
  {
    int layers = chatApp_->gpuLayersForModel(model.id);
    oss << "GPU layers: " << (layers == -1 ? std::string("auto") : std::to_string(layers)) << "\n";
  }
  return oss.str();
}

void ModelDialog::showStatusMessage(const std::string &message)
{
  // Create a copy to ensure string lifetime
  std::string safeCopy = message;
  messageBox(safeCopy.c_str(), mfInformation | mfOKButton);
}

void ModelDialog::showErrorMessage(const std::string &error)
{
  // Create a copy to ensure string lifetime
  std::string safeCopy = error;
  messageBox(safeCopy.c_str(), mfError | mfOKButton);
}

void ModelDialog::updateStatusLabel(const std::string &message)
{
  if (statusText_ == message)
    return;

  statusText_ = message;

  if (statusLabel_)
  {
    static_cast<StatusLabel *>(statusLabel_)->update();
    statusLabel_->drawView();
  }
  else
  {
    drawView();
  }
}

void ModelDialog::updateDetailLabel(const std::string &message)
{
  if (detailStatusText_ == message)
    return;

  detailStatusText_ = message;

  if (detailStatusLabel_)
  {
    static_cast<StatusLabel *>(detailStatusLabel_)->update();
    detailStatusLabel_->drawView();
  }
}

void ModelDialog::updateListBox(TListBox *listBox,
                                const std::vector<std::string> &items)
{
  if (!listBox)
    return;

  auto *collection = new TStringCollection(10, 5);
  for (const auto &item : items)
  {
    collection->insert(duplicateString(item));
  }
  listBox->newList(collection);
}

void ModelDialog::refreshRuntimeSettingsDisplay()
{
  if (!contextInfoLabel_)
    return;

  if (!chatApp_)
  {
    contextInfoText_.clear();
    static_cast<StatusLabel *>(contextInfoLabel_)->update();
    contextInfoLabel_->drawView();
    return;
  }

  auto setInputValue = [](TInputLine *input, std::size_t value)
  {
    if (!input)
      return;
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%zu", value);
    input->setData(buffer.data());
  };
  auto setInputString = [](TInputLine *input, const std::string &value)
  {
    if (!input)
      return;
    std::array<char, 64> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
    input->setData(buffer.data());
  };

  std::optional<std::string> selectedModelId;
  if (controller_)
  {
    if (auto selected = controller_->getSelectedDownloadedModel())
      selectedModelId = selected->id;
    else if (auto selected = controller_->getSelectedAvailableModel())
      selectedModelId = selected->id;
  }

  auto limits = chatApp_->resolveTokenLimits(selectedModelId);

  setInputValue(contextWindowInput_, limits.context_tokens);
  setInputValue(responseTokensInput_, limits.max_response_tokens);
  setInputValue(summaryThresholdInput_, limits.summary_trigger_tokens);

  std::ostringstream info;
  if (selectedModelId)
  {
    info << "Configured tokens: ctx " << limits.context_tokens << " | resp "
         << limits.max_response_tokens << " | summary "
         << limits.summary_trigger_tokens;
  }
  else
  {
    const auto &settings = chatApp_->conversationSettings();
    info << "Active chat tokens: ctx " << settings.max_context_tokens
         << " | resp " << settings.max_response_tokens << " | summary "
         << settings.summary_trigger_tokens;
  }

  contextInfoText_ = info.str();
  static_cast<StatusLabel *>(contextInfoLabel_)->update();
  contextInfoLabel_->drawView();

  std::string gpuValue = "auto";
  if (chatApp_)
  {
    auto resolveLayers = [this](const std::string &modelId)
    {
      int layers = chatApp_->gpuLayersForModel(modelId);
      return layers == -1 ? std::string("auto") : std::to_string(layers);
    };
    if (controller_)
    {
      if (auto selected = controller_->getSelectedDownloadedModel())
        gpuValue = resolveLayers(selected->id);
      else if (auto selected = controller_->getSelectedAvailableModel())
        gpuValue = resolveLayers(selected->id);
      else
      {
        int layers = chatApp_->runtime().gpu_layers;
        gpuValue = layers == -1 ? std::string("auto") : std::to_string(layers);
      }
    }
  }

  setInputString(gpuLayersInput_, gpuValue);
}

void ModelDialog::applyRuntimeSettings()
{
  if (!chatApp_)
    return;

  auto trim = [](std::string &value)
  {
    auto notSpace = [](unsigned char ch)
    { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
  };

  auto readField = [](TInputLine *input)
  {
    std::string value;
    if (!input)
      return value;
    std::array<char, 64> buffer{};
    input->getData(buffer.data());
    value.assign(buffer.data());
    return value;
  };

  auto settings = chatApp_->conversationSettings();
  std::optional<std::string> selectedModelId;
  if (controller_)
  {
    if (auto downloaded = controller_->getSelectedDownloadedModel())
    {
      selectedModelId = downloaded->id;
    }
    else if (auto available = controller_->getSelectedAvailableModel())
    {
      selectedModelId = available->id;
    }
  }

  auto limits = chatApp_->resolveTokenLimits(selectedModelId);

  std::string contextText = readField(contextWindowInput_);
  trim(contextText);
  std::size_t contextTokens = limits.context_tokens > 0 ? limits.context_tokens
                                                       : settings.max_context_tokens;
  if (!contextText.empty())
  {
    try
    {
      contextTokens = std::stoull(contextText);
    }
    catch (const std::exception &)
    {
      showErrorMessage("Invalid value for context window");
      return;
    }
  }
  if (contextTokens == 0)
  {
    showErrorMessage("Context window must be greater than zero");
    return;
  }

  std::string responseText = readField(responseTokensInput_);
  trim(responseText);
  std::size_t maxResponseTokens =
      limits.max_response_tokens > 0 ? limits.max_response_tokens
                                     : settings.max_response_tokens;
  if (!responseText.empty())
  {
    try
    {
      maxResponseTokens = std::stoull(responseText);
    }
    catch (const std::exception &)
    {
      showErrorMessage("Invalid value for response token limit");
      return;
    }
  }

  std::string summaryText = readField(summaryThresholdInput_);
  trim(summaryText);
  std::size_t summaryTokens =
      limits.summary_trigger_tokens > 0 ? limits.summary_trigger_tokens
                                        : settings.summary_trigger_tokens;
  if (!summaryText.empty())
  {
    try
    {
      summaryTokens = std::stoull(summaryText);
    }
    catch (const std::exception &)
    {
      showErrorMessage("Invalid value for summary threshold");
      return;
    }
  }

  if (maxResponseTokens == 0)
    maxResponseTokens = settings.max_response_tokens;
  if (summaryTokens == 0)
    summaryTokens = settings.summary_trigger_tokens;

  if (maxResponseTokens > contextTokens)
    maxResponseTokens = contextTokens;
  if (summaryTokens > contextTokens)
    summaryTokens = contextTokens;

  std::string gpuText = readField(gpuLayersInput_);
  trim(gpuText);

  int gpuLayers = -1;
  if (selectedModelId)
    gpuLayers = chatApp_->gpuLayersForModel(*selectedModelId);
  else
    gpuLayers = chatApp_->runtime().gpu_layers;

  if (!gpuText.empty())
  {
    std::string lowered = gpuText;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    if (lowered == "auto")
    {
      gpuLayers = -1;
    }
    else
    {
      try
      {
        gpuLayers = std::stoi(gpuText);
      }
      catch (const std::exception &)
      {
        showErrorMessage("Invalid value for GPU layers");
        return;
      }
    }
  }

  if (selectedModelId)
  {
    chatApp_->updateModelTokenSettings(*selectedModelId, contextTokens,
                                       maxResponseTokens, summaryTokens);
    chatApp_->updateModelGpuLayers(*selectedModelId, gpuLayers);
  }
  else
  {
    chatApp_->updateConversationSettings(contextTokens, maxResponseTokens,
                                         summaryTokens);
  }

  refreshRuntimeSettingsDisplay();
  updateStatusLabel("Runtime settings updated");
  updateDetailLabel(detailStatusText_);
}

void ModelDialog::setAvailableSelectionFromListIndex(int listIndex)
{
  if (!controller_)
    return;

  if (listIndex >= 0 &&
      listIndex < static_cast<int>(availableModelIndexMap_.size()))
  {
    controller_->setSelectedAvailableModel(
        availableModelIndexMap_[listIndex]);
  }
}

void ModelDialog::setDownloadedSelectionFromListIndex(int listIndex)
{
  if (!controller_)
    return;

  if (listIndex >= 0 &&
      listIndex < static_cast<int>(downloadedModelIndexMap_.size()))
  {
    controller_->setSelectedDownloadedModel(
        downloadedModelIndexMap_[listIndex]);
  }
}

// All business logic methods removed - now handled by ModelManagerController

TDialog *ModelDialog::create(TRect bounds,
                             ck::ai::ModelManager &modelManager)
{
  return new ModelDialog(bounds, modelManager, nullptr);
}
