#pragma once

#include "../../../../include/ck/ai/model_manager.hpp"
#include "../../../../include/ck/ai/model_manager_controller.hpp"

#include "../tvision_include.hpp"

#include <memory>
#include <string>
#include <vector>

// Forward declaration
class ChatApp;

class ProperModelDialog : public TDialog {
public:
  ProperModelDialog(TRect bounds, ck::ai::ModelManager &modelManager,
                    ChatApp *app = nullptr);
  ~ProperModelDialog();

  virtual void handleEvent(TEvent &event) override;
  virtual void draw() override;

  static TDialog *create(TRect bounds, ck::ai::ModelManager &modelManager);

private:
  void setupControls();
  void updateModelLists();
  void updateButtons();
  void updateStatusForSelection();
  std::string buildModelInfoLine(const ck::ai::ModelInfo &model,
                                 bool fromDownloadedList) const;
  std::string formatDetailedInfo(const ck::ai::ModelInfo &model) const;
  void syncSelectionFromLists();
  void setAvailableSelectionFromListIndex(int listIndex);
  void setDownloadedSelectionFromListIndex(int listIndex);
  void showStatusMessage(const std::string &message);
  void showErrorMessage(const std::string &error);
  void updateStatusLabel(const std::string &message);
  void updateDetailLabel(const std::string &message);
  void updateListBox(TListBox *listBox, const std::vector<std::string> &items);

  // Business logic controller
  std::unique_ptr<ck::ai::ModelManagerController> controller_;

  // Reference to main app for menu updates
  ChatApp *chatApp_;

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
  TLabel *detailStatusLabel_;
  std::string statusText_;
  std::string detailStatusText_;

  // UI display data
  std::vector<std::string> availableModelStrings_;
  std::vector<std::string> downloadedModelStrings_;
  std::vector<int> availableModelIndexMap_;
  std::vector<int> downloadedModelIndexMap_;
};
