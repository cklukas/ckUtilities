#include "prompt_dialog.hpp"

#include "../commands.hpp"
#include "chat_app.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <vector>
#include <tvision/util.h>

namespace
{
  constexpr const char *kPromptStatus = "Manage system prompts";

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
        text = newStr(backingText_->c_str());
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

  class PromptEditDialog : public TDialog
  {
  public:
    PromptEditDialog(const std::string &title, const std::string &name,
                     const std::string &message)
        : TWindowInit(&PromptEditDialog::initFrame),
          TDialog(TRect(0, 0, 72, 18), title.c_str())
    {
      options |= ofCentered;

      TRect nameRect(2, 2, 70, 3);
      nameLine_ = new TInputLine(nameRect, 64);
      insert(nameLine_);
      setInitialName(name);

      insert(new TLabel(TRect(2, 1, 18, 2), "Prompt Name:", nameLine_));

      TRect memoRect(2, 5, 69, 13);
      TRect vScrollRect(memoRect.b.x, memoRect.a.y, memoRect.b.x + 1, memoRect.b.y);
      vScroll_ = new TScrollBar(vScrollRect);
      insert(vScroll_);
      TRect hScrollRect(memoRect.a.x, memoRect.b.y, memoRect.b.x, memoRect.b.y + 1);
      hScroll_ = new TScrollBar(hScrollRect);
      insert(hScroll_);

      messageMemo_ = new TMemo(memoRect, hScroll_, vScroll_, nullptr, 4096);
      insert(messageMemo_);
      setInitialMessage(message);

      insert(new TLabel(TRect(2, 4, 18, 5), "Message:", messageMemo_));

      insert(new TButton(TRect(18, 15, 32, 17), "~O~K", cmOK, bfDefault));
      insert(new TButton(TRect(36, 15, 50, 17), "~C~ancel", cmCancel, bfNormal));

      selectNext(False);
    }

    std::string promptName() const
    {
      std::array<char, 129> buffer{};
      nameLine_->getData(buffer.data());
      return buffer.data();
    }

    std::string promptMessage() const
    {
      const std::size_t rawSize = static_cast<std::size_t>(messageMemo_->dataSize());
      std::vector<char> raw(rawSize ? rawSize : sizeof(TMemoData));
      messageMemo_->getData(raw.data());
      const auto *memo = reinterpret_cast<const TMemoData *>(raw.data());
      return decodeMemoText(memo->buffer, memo->length);
    }

  private:
    TInputLine *nameLine_;
    TScrollBar *vScroll_ = nullptr;
    TScrollBar *hScroll_ = nullptr;
    TMemo *messageMemo_;

    void setInitialName(const std::string &name)
    {
      std::array<char, 65> buffer{};
      std::strncpy(buffer.data(), name.c_str(), buffer.size() - 1);
      nameLine_->setData(buffer.data());
    }

    void setInitialMessage(const std::string &message)
    {
      const std::string encoded = encodeMemoText(message);
      const std::size_t maxBuffer = 4096;
      const std::size_t limited =
          std::min<std::size_t>(encoded.size(), maxBuffer);

      // TMemo expects TMemoData shape: [length][buffer...]
      std::vector<char> raw(sizeof(ushort) +
                            std::max<std::size_t>(limited, std::size_t{1}));
      auto *memo = reinterpret_cast<TMemoData *>(raw.data());
      memo->length = static_cast<ushort>(
          std::min<std::size_t>(limited, std::numeric_limits<ushort>::max()));
      if (memo->length > 0)
        std::memcpy(memo->buffer, encoded.data(), memo->length);

      messageMemo_->setData(memo);
    }

    static std::string encodeMemoText(const std::string &text)
    {
      std::string encoded;
      encoded.reserve(text.size());
      for (char ch : text)
      {
        if (ch == '\n')
          encoded.push_back('\r');
        else
          encoded.push_back(ch);
      }
      return encoded;
    }

    static std::string decodeMemoText(const char *data, ushort length)
    {
      std::string decoded;
      decoded.reserve(length);
      for (ushort i = 0; i < length; ++i)
      {
        char ch = data[i];
        if (ch == '\r')
        {
          // Turbo Vision memo stores new lines as CR
          decoded.push_back('\n');
        }
        else
        {
          decoded.push_back(ch);
        }
      }
      return decoded;
    }
  };
} // namespace

PromptDialog::PromptDialog(TRect bounds, ck::ai::SystemPromptManager &manager,
                           ChatApp *app)
    : TWindowInit(&PromptDialog::initFrame),
      TDialog(bounds, "Manage System Prompts"), manager_(manager),
      chatApp_(app)
{
  setupControls();
  refreshList();
  setStatus(kPromptStatus);
}

PromptDialog::~PromptDialog()
{
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
}

void PromptDialog::setupControls()
{
  TRect listRect(2, 2, 48, 15);
  listBox_ = new TListBox(listRect, 1, nullptr);
  insert(listBox_);

  addButton_ = new TButton(TRect(50, 2, 64, 4), "~A~dd", cmNoOp, bfNormal);
  activateButton_ = new TButton(TRect(50, 5, 64, 7), "Acti~v~ate", cmNoOp, bfNormal);
  editButton_ = new TButton(TRect(50, 8, 64, 10), "~E~dit", cmNoOp, bfNormal);
  deleteButton_ = new TButton(TRect(50, 11, 64, 13), "~D~elete", cmNoOp, bfNormal);
  closeButton_ = new TButton(TRect(50, 14, 64, 16), "~C~lose", cmClose, bfNormal);

  insert(addButton_);
  insert(activateButton_);
  insert(editButton_);
  insert(deleteButton_);
  insert(closeButton_);

  statusLabel_ = new StatusLabel(TRect(2, 16, 74, 17), statusText_);
  insert(statusLabel_);

  listBox_->options |= ofSelectable;
}

void PromptDialog::handleEvent(TEvent &event)
{
  TDialog::handleEvent(event);

  const bool isListEvent =
      event.what == evBroadcast &&
      event.message.command == cmListItemSelected;

  if (isListEvent)
  {
    updateButtons();
    if (event.message.infoPtr == listBox_)
    {
      if (auto prompt = selectedPrompt())
        setStatus(prompt->name + " selected");
      else
        setStatus(kPromptStatus);
    }
  }
  else if (event.what == evCommand)
  {
    if (event.message.command == cmClose)
    {
      close();
      clearEvent(event);
      return;
    }

    if (event.message.infoPtr == addButton_)
    {
      addPrompt();
      clearEvent(event);
    }
    else if (event.message.infoPtr == editButton_)
    {
      editPrompt();
      clearEvent(event);
    }
    else if (event.message.infoPtr == deleteButton_)
    {
      deletePrompt();
      clearEvent(event);
    }
    else if (event.message.infoPtr == activateButton_)
    {
      activatePrompt();
      clearEvent(event);
    }
  }

  if (event.what == evBroadcast || event.what == evCommand)
    updateButtons();
}

void PromptDialog::refreshList()
{
  prompts_ = manager_.get_prompts();
  std::stable_sort(prompts_.begin(), prompts_.end(),
                   [](const auto &a, const auto &b)
                   {
                     return a.name < b.name;
                   });

  indexMap_.clear();
  auto *collection = new TStringCollection(10, 5);
  int activeSelection = -1;
  for (std::size_t i = 0; i < prompts_.size(); ++i)
  {
    const auto &prompt = prompts_[i];
    std::string entry = prompt.name;

    std::vector<std::string> tags;
    if (prompt.is_default)
      tags.emplace_back("default");
    if (prompt.is_active)
      tags.emplace_back("active");

    if (!tags.empty())
    {
      entry += " [";
      for (std::size_t t = 0; t < tags.size(); ++t)
      {
        entry += tags[t];
        if (t + 1 < tags.size())
          entry += ", ";
      }
      entry += "]";
    }
    collection->insert(newStr(entry.c_str()));
    indexMap_.push_back(static_cast<int>(i));
    if (prompt.is_active)
      activeSelection = static_cast<int>(i);
  }
  listBox_->newList(collection);
  if (activeSelection >= 0)
  {
    auto it = std::find(indexMap_.begin(), indexMap_.end(), activeSelection);
    if (it != indexMap_.end())
      listBox_->focusItem(static_cast<int>(std::distance(indexMap_.begin(), it)));
  }
  updateButtons();
}

void PromptDialog::updateButtons()
{
  auto selection = selectedPrompt();
  const bool hasSelection = selection.has_value();
  const bool isDefault = selection && selection->is_default;

  activateButton_->setState(sfDisabled, !hasSelection);
  editButton_->setState(sfDisabled, !hasSelection);

  if (deleteButton_)
  {
    const char *desiredTitle = isDefault ? "~R~estore" : "~D~elete";
    if (!deleteButton_->title || std::strcmp(deleteButton_->title, desiredTitle) != 0)
    {
      if (deleteButton_->title)
        delete[] deleteButton_->title;
      deleteButton_->title = newStr(desiredTitle);
    }

    const bool enable = hasSelection;
    deleteButton_->setState(sfDisabled, !enable);
    deleteButton_->drawView();
  }

  if (addButton_)
    addButton_->drawView();
  if (activateButton_)
    activateButton_->drawView();
  if (editButton_)
    editButton_->drawView();
  if (closeButton_)
    closeButton_->drawView();
}

void PromptDialog::setStatus(const std::string &message)
{
  statusText_ = message;
  if (statusLabel_)
  {
    statusLabel_->drawView();
  }
}

int PromptDialog::selectedIndex() const
{
  if (!listBox_)
    return -1;
  return listBox_->focused;
}

std::optional<ck::ai::SystemPrompt> PromptDialog::selectedPrompt() const
{
  int idx = selectedIndex();
  if (idx < 0 || idx >= static_cast<int>(indexMap_.size()))
    return std::nullopt;
  int promptIndex = indexMap_[idx];
  if (promptIndex < 0 || promptIndex >= static_cast<int>(prompts_.size()))
    return std::nullopt;
  return prompts_[promptIndex];
}

void PromptDialog::addPrompt()
{
  auto *dialog = new PromptEditDialog("Add Prompt", "", "");
  ushort code = TProgram::application->execView(dialog);
  if (code != cmOK)
  {
    TObject::destroy(dialog);
    return;
  }

  std::string name = dialog->promptName();
  std::string message = dialog->promptMessage();
  TObject::destroy(dialog);
  if (name.empty() || message.empty())
  {
    messageBox("Name and message are required", mfError | mfOKButton);
    setStatus("Prompt creation cancelled: missing name or message");
    return;
  }

  ck::ai::SystemPrompt prompt;
  prompt.name = name;
  prompt.message = message;
  manager_.add_or_update_prompt(prompt);
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
  refreshList();
  setStatus("Prompt added: " + name);
}

void PromptDialog::editPrompt()
{
  auto promptOpt = selectedPrompt();
  if (!promptOpt)
  {
    setStatus("No prompt selected for edit");
    return;
  }
  auto prompt = *promptOpt;

  auto *dialog = new PromptEditDialog("Edit Prompt", prompt.name, prompt.message);
  ushort code = TProgram::application->execView(dialog);
  if (code != cmOK)
  {
    TObject::destroy(dialog);
    setStatus("Prompt edit cancelled");
    return;
  }

  std::string name = dialog->promptName();
  std::string message = dialog->promptMessage();
  TObject::destroy(dialog);
  if (name.empty() || message.empty())
  {
    messageBox("Name and message are required", mfError | mfOKButton);
    setStatus("Prompt update failed: missing name or message");
    return;
  }

  prompt.name = name;
  prompt.message = message;
  manager_.add_or_update_prompt(prompt);
  if (chatApp_)
    chatApp_->handlePromptManagerChange();
  refreshList();
  setStatus("Prompt updated: " + name);
}

void PromptDialog::deletePrompt()
{
  auto promptOpt = selectedPrompt();
  if (!promptOpt)
  {
    setStatus("No prompt selected");
    return;
  }
  auto prompt = *promptOpt;
  if (prompt.is_default)
  {
    if (manager_.restore_default_prompt(prompt.id))
    {
      if (chatApp_)
        chatApp_->handlePromptManagerChange();
      refreshList();
      setStatus("Prompt restored to default: " + prompt.name);
    }
    else
    {
      setStatus("Failed to restore prompt: " + prompt.name);
    }
    return;
  }

  if (messageBox("Delete selected prompt?", mfConfirmation | mfYesNoCancel) == cmYes)
  {
    if (manager_.delete_prompt(prompt.id))
    {
      if (chatApp_)
        chatApp_->handlePromptManagerChange();
      refreshList();
      setStatus("Prompt deleted: " + prompt.name);
    }
    else
    {
      setStatus("Failed to delete prompt: " + prompt.name);
    }
  }
}

void PromptDialog::activatePrompt()
{
  auto promptOpt = selectedPrompt();
  if (!promptOpt)
  {
    setStatus("No prompt selected to activate");
    return;
  }
  auto prompt = *promptOpt;
  if (manager_.set_active_prompt(prompt.id))
  {
    if (chatApp_)
      chatApp_->handlePromptManagerChange();
    refreshList();
    setStatus("Prompt activated: " + prompt.name);
  }
  else
  {
    setStatus("Failed to activate prompt: " + prompt.name);
  }
}

TDialog *PromptDialog::create(TRect bounds,
                              ck::ai::SystemPromptManager &manager)
{
  return new PromptDialog(bounds, manager, nullptr);
}
