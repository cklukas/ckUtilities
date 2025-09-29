#pragma once

#include "../tvision_include.hpp"

#include <string>

class ModelLoadingProgressDialog : public TDialog {
public:
  ModelLoadingProgressDialog(TRect bounds, const std::string &modelName);
  ~ModelLoadingProgressDialog();

  virtual void handleEvent(TEvent &event) override;
  virtual void draw() override;

  void updateProgress(const std::string &status);
  void setComplete(bool success, const std::string &message);
  static TDialog *create(TRect bounds, const std::string &modelName);

private:
  void setupControls();
  std::string formatBytes(std::size_t bytes) const;
  static TFrame *initFrame(TRect r);

  std::string modelName_;
  TLabel *modelNameLabel_;
  TLabel *statusLabel_;
  TButton *closeButton_;
  bool isComplete_;
  bool isSuccess_;
  std::string statusMessage_;
  std::string statusText_;
};
