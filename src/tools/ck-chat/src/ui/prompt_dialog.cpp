#include "prompt_dialog.hpp"

#include "../commands.hpp"
#include "chat_app.hpp"

#include <algorithm>
#include <sstream>
#include <optional>
#include <vector>
#include <tvision/util.h>

namespace {
constexpr const char *kPromptStatus = "Manage system prompts";

class StatusLabel : public TLabel {
public:
  StatusLabel(const TRect &bounds, std::string &backingText)
      : TLabel(bounds, backingText.c_str(), nullptr), backingText_(&backingText) {
  }

  void update() {
    if (text)
      delete[] (char *)text;
    if (backingText_)
      text = newStr(backingText_->c_str());
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

class PromptEditDialog : public TDialog {
public:
  PromptEditDialog(const std::string &title, const std::string &name,
                   const std::string &message)
      : TWindowInit(&PromptEditDialog::initFrame),
        TDialog(TRect(0, 0, 72, 19), title.c_str()) {
    options |= ofCentered;

    TRect nameRect(2, 2, 68, 3);
    nameLine_ = new TInputLine(nameRect, 64);
    insert(nameLine_);
    nameLine_->setData((void *)name.c_str());

    insert(new TLabel(TRect(2, 1, 18, 2), "Prompt Name:", nameLine_));

    TRect memoRect(2, 5, 64, 13);
    TRect vScrollRect(memoRect.b.x, memoRect.a.y, memoRect.b.x + 1, memoRect.b.y);
    vScroll_ = new TScrollBar(vScrollRect);
    insert(vScroll_);
    TRect hScrollRect(memoRect.a.x, memoRect.b.y, memoRect.b.x, memoRect.b.y + 1);
    hScroll_ = new TScrollBar(hScrollRect);
    insert(hScroll_);

    messageMemo_ = new TMemo(memoRect, hScroll_, vScroll_, nullptr, 4096);
    insert(messageMemo_);
    messageMemo_->setData((void *)message.c_str());

    insert(new TLabel(TRect(2, 4, 18, 5), "Message:", messageMemo_));

    insert(new TButton(TRect(18, 15, 32, 17), "~O~K", cmOK, bfDefault));
    insert(new TButton(TRect(36, 15, 50, 17), "~C~ancel", cmCancel, bfNormal));

    selectNext(False);
  }

  std::string promptName() const {
    char buffer[128] = {0};
    nameLine_->getData(buffer);
    return buffer;
  }

  std::string promptMessage() const {
    size_t size = static_cast<size_t>(messageMemo_->dataSize());
    std::vector<char> buffer(size + 1, 0);
    messageMemo_->getData(buffer.data());
    return buffer.data();
  }

private:
  TInputLine *nameLine_;
  TScrollBar *vScroll_ = nullptr;
  TScrollBar *hScroll_ = nullptr;
  TMemo *messageMemo_;
};
} // namespace

PromptDialog::PromptDialog(TRect bounds, ck::ai::SystemPromptManager &manager,
                           ChatApp *app)
    : TWindowInit(&PromptDialog::initFrame),
      TDialog(bounds, "Manage System Prompts"), manager_(manager),
      chatApp_(app) {
  setupControls();
  refreshList();
  setStatus(kPromptStatus);
}

PromptDialog::~PromptDialog() {
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
}

void PromptDialog::setupControls() {
  TRect listRect(2, 2, 58, 15);
  listBox_ = new TListBox(listRect, 1, nullptr);
  insert(listBox_);

  addButton_ = new TButton(TRect(60, 2, 74, 4), "~A~dd", cmNoOp, bfNormal);
  activateButton_ = new TButton(TRect(60, 5, 74, 7), "~S~et", cmNoOp, bfNormal);
  editButton_ = new TButton(TRect(60, 8, 74, 10), "~E~dit", cmNoOp, bfNormal);
  deleteButton_ = new TButton(TRect(60, 11, 74, 13), "~D~elete", cmNoOp, bfNormal);
  closeButton_ = new TButton(TRect(60, 14, 74, 16), "~C~lose", cmClose, bfNormal);

  insert(addButton_);
  insert(activateButton_);
  insert(editButton_);
  insert(deleteButton_);
  insert(closeButton_);

  statusLabel_ = new StatusLabel(TRect(2, 16, 74, 17), statusText_);
  detailLabel_ = new StatusLabel(TRect(2, 17, 74, 18), detailText_);
  insert(statusLabel_);
  insert(detailLabel_);

  listBox_->options |= ofSelectable;
}

void PromptDialog::handleEvent(TEvent &event) {
  TDialog::handleEvent(event);

  if (event.what == evBroadcast && event.message.command == cmListItemSelected) {
    updateButtons();
    if (event.message.infoPtr == listBox_) {
      auto prompt = selectedPrompt();
      if (prompt)
        setDetail(prompt->message);
      else
        setDetail("");
    }
  } else if (event.what == evCommand) {
    if (event.message.command == cmClose)
      return;

    if (event.message.infoPtr == addButton_) {
      addPrompt();
      clearEvent(event);
    } else if (event.message.infoPtr == editButton_) {
      editPrompt();
      clearEvent(event);
    } else if (event.message.infoPtr == deleteButton_) {
      deletePrompt();
      clearEvent(event);
    } else if (event.message.infoPtr == activateButton_) {
      activatePrompt();
      clearEvent(event);
    }
  }
}

void PromptDialog::refreshList() {
  prompts_ = manager_.get_prompts();
  std::stable_sort(prompts_.begin(), prompts_.end(),
                   [](const auto &a, const auto &b) {
                     return a.name < b.name;
                   });

  indexMap_.clear();
  auto *collection = new TStringCollection(10, 5);
  int activeSelection = -1;
  for (std::size_t i = 0; i < prompts_.size(); ++i) {
    const auto &prompt = prompts_[i];
    std::string entry = prompt.name;
    if (prompt.is_active)
      entry += " [active]";
    collection->insert(newStr(entry.c_str()));
    indexMap_.push_back(static_cast<int>(i));
    if (prompt.is_active)
      activeSelection = static_cast<int>(i);
  }
  listBox_->newList(collection);
  if (activeSelection >= 0) {
    auto it = std::find(indexMap_.begin(), indexMap_.end(), activeSelection);
    if (it != indexMap_.end())
      listBox_->focusItem(static_cast<int>(std::distance(indexMap_.begin(), it)));
  }
  updateButtons();
  if (auto prompt = selectedPrompt())
    setDetail(prompt->message);
  else
    setDetail("");
}

void PromptDialog::updateButtons() {
  bool hasSelection = selectedPrompt().has_value();
  bool multiplePrompts = prompts_.size() > 1;
  auto selection = selectedPrompt();
  bool isDefault = selection && selection->is_default;

  activateButton_->setState(sfDisabled, !hasSelection);
  editButton_->setState(sfDisabled, !hasSelection);
  deleteButton_->setState(sfDisabled,
                          !hasSelection || !multiplePrompts || isDefault);

  addButton_->drawView();
  activateButton_->drawView();
  editButton_->drawView();
  deleteButton_->drawView();
  closeButton_->drawView();
}

void PromptDialog::setStatus(const std::string &message) {
  statusText_ = message;
  if (statusLabel_) {
    statusLabel_->drawView();
  }
}

void PromptDialog::setDetail(const std::string &message) {
  detailText_ = message;
  if (detailLabel_) {
    detailLabel_->drawView();
  }
}

int PromptDialog::selectedIndex() const {
  if (!listBox_)
    return -1;
  return listBox_->focused;
}

std::optional<ck::ai::SystemPrompt> PromptDialog::selectedPrompt() const {
  int idx = selectedIndex();
  if (idx < 0 || idx >= static_cast<int>(indexMap_.size()))
    return std::nullopt;
  int promptIndex = indexMap_[idx];
  if (promptIndex < 0 || promptIndex >= static_cast<int>(prompts_.size()))
    return std::nullopt;
  return prompts_[promptIndex];
}

void PromptDialog::addPrompt() {
  auto *dialog = new PromptEditDialog("Add Prompt", "", "");
  ushort code = TProgram::application->execView(dialog);
  if (code != cmOK) {
    TObject::destroy(dialog);
    return;
  }

  std::string name = dialog->promptName();
  std::string message = dialog->promptMessage();
  TObject::destroy(dialog);
  if (name.empty() || message.empty()) {
    messageBox("Name and message are required", mfError | mfOKButton);
    return;
  }

  ck::ai::SystemPrompt prompt;
  prompt.name = name;
  prompt.message = message;
  manager_.add_or_update_prompt(prompt);
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
  refreshList();
}

void PromptDialog::editPrompt() {
  auto promptOpt = selectedPrompt();
  if (!promptOpt)
    return;
  auto prompt = *promptOpt;

  auto *dialog = new PromptEditDialog("Edit Prompt", prompt.name, prompt.message);
  ushort code = TProgram::application->execView(dialog);
  if (code != cmOK) {
    TObject::destroy(dialog);
    return;
  }

  std::string name = dialog->promptName();
  std::string message = dialog->promptMessage();
  TObject::destroy(dialog);
  if (name.empty() || message.empty()) {
    messageBox("Name and message are required", mfError | mfOKButton);
    return;
  }

  prompt.name = name;
  prompt.message = message;
  manager_.add_or_update_prompt(prompt);
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
  refreshList();
}

void PromptDialog::deletePrompt() {
  auto promptOpt = selectedPrompt();
  if (!promptOpt)
    return;
  auto prompt = *promptOpt;
  if (prompt.is_default) {
    messageBox("Default prompts cannot be deleted", mfError | mfOKButton);
    return;
  }

  if (messageBox("Delete selected prompt?", mfConfirmation | mfYesNoCancel) == cmYes) {
    manager_.delete_prompt(prompt.id);
    if (chatApp_)
      chatApp_->handlePromptManagerChange();
    refreshList();
  }
}

void PromptDialog::activatePrompt() {
  auto promptOpt = selectedPrompt();
  if (!promptOpt)
    return;
  auto prompt = *promptOpt;
  manager_.set_active_prompt(prompt.id);
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
  refreshList();
}

TDialog *PromptDialog::create(TRect bounds,
                              ck::ai::SystemPromptManager &manager) {
  return new PromptDialog(bounds, manager, nullptr);
}
