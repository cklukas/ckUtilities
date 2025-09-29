#include "model_loading_dialog.hpp"

#include <iomanip>
#include <sstream>
#include <tvision/util.h>

namespace {
class StatusLabel : public TLabel {
public:
  StatusLabel(const TRect &bounds, std::string &backingText)
      : TLabel(bounds, backingText.c_str(), nullptr),
        backingText_(&backingText) {}

  void update() {
    if (text)
      delete[] (char *)text;
    if (backingText_)
      text = newStr(TStringView(*backingText_));
    else
      text = nullptr;
  }

  void draw() override {
    update();
    TLabel::draw();
  }

private:
  std::string *backingText_;
};
} // namespace

ModelLoadingProgressDialog::ModelLoadingProgressDialog(
    TRect bounds, const std::string &modelName)
    : TWindowInit(&ModelLoadingProgressDialog::initFrame),
      TDialog(bounds, "Loading Model"), modelName_(modelName),
      modelNameLabel_(nullptr), statusLabel_(nullptr), closeButton_(nullptr),
      isComplete_(false), isSuccess_(false), statusText_("Initializing...") {
  setupControls();
}

ModelLoadingProgressDialog::~ModelLoadingProgressDialog() {}

void ModelLoadingProgressDialog::setupControls() {
  modelNameLabel_ = new TLabel(TRect(2, 2, 38, 3), modelName_.c_str(), nullptr);
  insert(modelNameLabel_);

  statusLabel_ = new StatusLabel(TRect(2, 4, 38, 5), statusText_);
  insert(statusLabel_);

  closeButton_ =
      new TButton(TRect(14, 6, 24, 8), "~C~lose", cmCancel, bfNormal);
  insert(closeButton_);

  // Make close button disabled during loading
  closeButton_->setState(sfDisabled, true);
}

void ModelLoadingProgressDialog::handleEvent(TEvent &event) {
  TDialog::handleEvent(event);
}

void ModelLoadingProgressDialog::draw() { TDialog::draw(); }

void ModelLoadingProgressDialog::updateProgress(const std::string &status) {
  // Add a simple loading indicator
  static int spinner = 0;
  const char *spinnerChars = "|/-\\";
  statusText_ = status + " " + spinnerChars[spinner % 4];
  spinner++;

  if (statusLabel_) {
    statusLabel_->drawView();
  }
  drawView();
}

void ModelLoadingProgressDialog::setComplete(bool success,
                                             const std::string &message) {
  isComplete_ = true;
  isSuccess_ = success;
  statusMessage_ = message;

  // Update status message
  statusText_ = message;
  if (statusLabel_) {
    statusLabel_->drawView();
  }

  // Enable close button
  if (closeButton_) {
    closeButton_->setState(sfDisabled, false);
  }

  drawView();

  // Auto-close after a short delay if successful
  if (success) {
    // Close the dialog immediately
    close();
  }
}

std::string ModelLoadingProgressDialog::formatBytes(std::size_t bytes) const {
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

TFrame *ModelLoadingProgressDialog::initFrame(TRect r) { return new TFrame(r); }

TDialog *ModelLoadingProgressDialog::create(TRect bounds,
                                            const std::string &modelName) {
  return new ModelLoadingProgressDialog(bounds, modelName);
}
