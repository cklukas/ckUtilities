#include "chat_window.hpp"
#include "chat_app.hpp"
#include "chat_transcript_view.hpp"
#include "prompt_input_view.hpp"
#include "../clipboard.hpp"
#include "../commands.hpp"
#include "ck/hotkeys.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>
#include <tvision/views.h>

// Variable Name	    | Meaning
// ---------------------|---------------------------------------------------------------
// transcript           | The view that displays the chat transcript/history.
// transcriptScrollBar	| The scrollbar associated with the chat transcript view.
// copyButtons          | A collection of buttons, each for copying a specific assistant
//
// promptInput          | The input field where the user types their message.
// promptScrollBar      | The scrollbar for the user's text input area.
// submitButton         | The button used to send the user's message.
//                      | message from the transcript.

void ChatWindow::setButtonTitle(TButton &button, const char *title)
{
    delete[] const_cast<char *>(button.title);
    button.title = newStr(title);
    button.drawView();
}

ChatWindow::ChatWindow(ChatApp &owner, const TRect &bounds, int number)
    : TWindowInit(&ChatWindow::initFrame),
      TWindow(bounds, "Chat", number),
      app(owner)
{
    options |= ofTileable;

    TRect extent = getExtent();
    extent.grow(-2, -1);

    const int inputLines = 4;  // number of lines for the prompt input area
    const int labelHeight = 1; // height of the prompt input label ("Prompt:")
    const int transcriptScrollWidth = 1;
    const int copyButtonColumnWidth = 7;
    const int inputScrollWidth = 1;
    const int buttonWidth = 12; // width of the submit button

    TRect transcriptRect = extent; // area for the chat transcript view
    transcriptRect.b.y -= (inputLines + labelHeight);
    transcriptRect.b.x -= (transcriptScrollWidth + copyButtonColumnWidth + 3);

    TRect transcriptScrollRect(transcriptRect.b.x + copyButtonColumnWidth, transcriptRect.a.y,
                               transcriptRect.b.x + copyButtonColumnWidth + transcriptScrollWidth, transcriptRect.b.y);
    auto *transcriptScroll = new TScrollBar(transcriptScrollRect);
    transcriptScroll->growMode = gfGrowHiY;
    transcriptScroll->setState(sfVisible, True);
    insert(transcriptScroll);
    transcriptScrollBar = transcriptScroll;

    transcript = new ChatTranscriptView(transcriptRect, nullptr, transcriptScroll);
    transcript->growMode = gfGrowHiX | gfGrowHiY;
    transcript->setLayoutChangedCallback([this]()
                                         { updateCopyButtons(); });
    transcript->setShowThinking(showThinking_);
    transcript->setShowAnalysis(showAnalysis_);
    transcript->setHiddenDetailCallback([this](std::size_t messageIndex, const std::string &channel,
                                              const std::string &content)
                                        {
                                            (void)messageIndex;
                                            showHiddenContent(channel, content);
                                        });
    insert(transcript);

    int labelTop = transcriptRect.b.y;
    int inputTop = labelTop + labelHeight;
    int promptRight = extent.b.x - (buttonWidth + inputScrollWidth);
    if (promptRight <= extent.a.x + 1)
        promptRight = extent.a.x + 2;
    int scrollLeft = promptRight;
    int buttonLeft = std::max(scrollLeft + inputScrollWidth + 1, extent.b.x - buttonWidth);
    if (buttonLeft >= extent.b.x)
        buttonLeft = extent.b.x - 1;

    TRect promptScrollRect(scrollLeft, inputTop, scrollLeft + inputScrollWidth, extent.b.y);
    promptScrollBar = new TScrollBar(promptScrollRect);
    promptScrollBar->growMode = gfGrowLoY | gfGrowHiY | gfGrowLoX | gfGrowHiX;
    promptScrollBar->setState(sfVisible, True);
    insert(promptScrollBar);

    TRect promptRect(extent.a.x, inputTop, scrollLeft, extent.b.y);
    promptInput = new PromptInputView(promptRect, nullptr, promptScrollBar);
    promptInput->growMode = gfGrowHiX | gfGrowLoY | gfGrowHiY;
    insert(promptInput);

    TRect labelRect(extent.a.x - 1, labelTop, scrollLeft, inputTop);
    auto *label = new TLabel(labelRect, "Prompt:", promptInput);
    label->growMode = gfGrowLoY | gfGrowHiY | gfGrowHiX;
    // set label forground color to yellow
    insert(label);

    const int buttonHeight = 2;
    int buttonTop = inputTop + std::max(0, (inputLines - buttonHeight) / 2);

    TRect buttonRect(buttonLeft, buttonTop, buttonLeft + 10, buttonTop + 2);

    submitButton = new TButton(buttonRect, "~S~ubmit", cmSendPrompt, bfDefault);
    submitButton->growMode = gfGrowLoX | gfGrowHiX | gfGrowLoY | gfGrowHiY;
    submitButton->setState(sfVisible, True);

    insert(submitButton);

    promptInput->select();

    session.setLogSink([this](const std::string &entry) { app.appendLog(entry); });
    app.registerWindow(this);
    session.setSystemPrompt(app.systemPrompt());
    newConversation();
}

TPalette &ChatWindow::getPalette() const
{
    static TPalette palette(cpGrayDialog, sizeof(cpGrayDialog) - 1);
    return palette;
}

void ChatWindow::handleEvent(TEvent &event)
{
    const TKey sendKey = ck::hotkeys::key(cmSendPrompt);
    if (event.what == evKeyDown && sendKey.code != 0)
    {
        TKey pressed(event.keyDown.keyCode, event.keyDown.controlKeyState);
        if (pressed == sendKey)
        {
            sendPrompt();
            clearEvent(event);
            return;
        }
    }

    if (event.what == evCommand)
    {
        ushort command = event.message.command;
        if (command == cmSendPrompt)
        {
            sendPrompt();
            clearEvent(event);
            return;
        }
        if (command >= cmCopyResponseBase)
        {
            if (auto *info = findCopyButtonByCommand(command))
            {
                copyAssistantMessage(info->messageIndex);
                clearEvent(event);
                return;
            }
        }
    }

    TWindow::handleEvent(event);
}

void ChatWindow::sizeLimits(TPoint &min, TPoint &max)
{
    TWindow::sizeLimits(min, max);
    constexpr short minWidth = 50;
    constexpr short minHeight = 16;
    if (min.x < minWidth)
        min.x = minWidth;
    if (min.y < minHeight)
        min.y = minHeight;
    (void)max;
}

void ChatWindow::shutDown()
{
    session.setLogSink(nullptr);
    session.cancelActiveResponse();
    clearCopyButtons();
    app.unregisterWindow(this);
    TWindow::shutDown();
}

void ChatWindow::processPendingResponses()
{
    if (!transcript)
        return;

    if (!session.consumeDirtyFlag())
        return;

    updateTranscriptFromSession(true);
    refreshWindowTitle();
}

void ChatWindow::applySystemPrompt(const std::string &prompt)
{
    session.setSystemPrompt(prompt);
}

void ChatWindow::applyConversationSettings(const ck::chat::ChatSession::ConversationSettings &settings)
{
    conversationSettings_ = settings;
    session.setConversationSettings(settings);
}

void ChatWindow::setShowThinking(bool show)
{
    bool changed = (showThinking_ != show);
    showThinking_ = show;
    if (transcript)
    {
        transcript->setShowThinking(showThinking_);
        if (changed)
        {
            updateTranscriptFromSession(false);
            transcript->drawView();
        }
    }
}

void ChatWindow::setShowAnalysis(bool show)
{
    bool changed = (showAnalysis_ != show);
    showAnalysis_ = show;
    if (transcript)
    {
        transcript->setShowAnalysis(showAnalysis_);
        if (changed)
        {
            updateTranscriptFromSession(false);
            transcript->drawView();
        }
    }
}

void ChatWindow::setStopSequences(const std::vector<std::string> &stops)
{
    stopSequences_ = stops;
    session.setStopSequences(stopSequences_);
}

void ChatWindow::newConversation()
{
    session.setSystemPrompt(app.systemPrompt());
    session.resetConversation();
    session.consumeDirtyFlag();
    clearCopyButtons();
    if (promptInput)
    {
        promptInput->clearText();
        promptInput->select();
    }
    updateTranscriptFromSession(true);
    refreshWindowTitle();
}

void ChatWindow::sendPrompt()
{
    if (!promptInput || !transcript)
        return;

    std::string prompt = promptInput->text();
    if (prompt.empty())
        return;

    auto llm = app.getActiveLlm();
    if (!llm)
    {
        messageBox("No active model loaded. Use Manage Models to activate one.",
                   mfInformation | mfOKButton);
        return;
    }

    session.addUserMessage(prompt);
    session.setSystemPrompt(app.systemPrompt());
    session.startAssistantResponse(prompt, std::move(llm));
    promptInput->clearText();
    app.appendLog("[USER]\n" + prompt + "\n");
    session.consumeDirtyFlag();
    updateTranscriptFromSession(true);
}

void ChatWindow::copyAssistantMessage(std::size_t messageIndex)
{
    if (!transcript)
        return;

    if (transcript->isMessagePending(messageIndex))
        return;

    std::string content;
    if (!transcript->messageForCopy(messageIndex, content))
        return;

    clipboard::copyToClipboard(content);
    messageBox(clipboard::statusMessage().c_str(), mfOKButton);
}

void ChatWindow::ensureCopyButton(std::size_t messageIndex)
{
    if (!transcript)
        return;

    if (auto *info = findCopyButton(messageIndex))
    {
        updateCopyButtonState(messageIndex);
        return;
    }

    ushort command = static_cast<ushort>(cmCopyResponseBase + copyButtons.size());
    TRect column = copyColumnBounds();
    TRect initialBounds(column.a.x, column.a.y,
                        column.a.x + 8, column.a.y + 2);
    bool pending = transcript->isMessagePending(messageIndex);
    const char *label = pending ? "wait" : "Copy";
    auto *button = new TButton(initialBounds, label, command, bfNormal);
    button->growMode = gfGrowLoX | gfGrowHiX;
    button->setState(sfVisible, False);
    button->setState(sfDisabled, pending ? True : False);
    insert(button);
    copyButtons.push_back(CopyButtonInfo{messageIndex, button, command});
    updateCopyButtons();
}

void ChatWindow::refreshWindowTitle()
{
    auto stats = session.contextStats();
    auto modelInfo = app.activeModelInfo();

    std::ostringstream titleStream;
    titleStream << "Chat";
    if (modelInfo)
        titleStream << " - " << modelInfo->name;
    else
        titleStream << " - No Model";

    if (modelInfo)
    {
        int requestedLayers = app.gpuLayersForModel(modelInfo->id);
        int effectiveLayers = app.effectiveGpuLayers(*modelInfo);
        titleStream << " | gpu "
                    << (requestedLayers == -1 ? std::string("auto")
                                              : std::to_string(requestedLayers))
                    << " (" << effectiveLayers << ")";
    }

    if (stats.max_context_tokens > 0)
    {
        titleStream << " | ctx " << stats.prompt_tokens << '/' << stats.max_context_tokens;
        double percent = stats.max_context_tokens > 0
                             ? (100.0 * static_cast<double>(stats.prompt_tokens) /
                                static_cast<double>(stats.max_context_tokens))
                             : 0.0;
        titleStream << " (" << std::fixed << std::setprecision(1) << percent << "%)";
        titleStream.unsetf(std::ios::floatfield);
    }
    else
    {
        titleStream << " | ctx " << stats.prompt_tokens;
    }

    if (stats.max_response_tokens > 0)
        titleStream << " | resp≤" << stats.max_response_tokens;
    else
        titleStream << " | resp unlimited";

    if (stats.summarization_enabled)
    {
        titleStream << " | summarize@" << stats.summary_trigger_tokens;
        if (stats.summary_present)
            titleStream << " (active)";
    }
    else
    {
        titleStream << " | summarize off";
    }

    std::string newTitle = titleStream.str();
    if (newTitle != lastWindowTitle_)
    {
        lastWindowTitle_ = newTitle;
        delete[] const_cast<char *>(title);
        title = newStr(newTitle.c_str());
        drawView();
        if (frame)
            frame->drawView();
    }
}

void ChatWindow::updateCopyButtonState(std::size_t messageIndex)
{
    if (!transcript)
        return;

    auto *info = findCopyButton(messageIndex);
    if (!info || !info->button)
        return;

    bool pending = transcript->isMessagePending(messageIndex);
    const char *label = pending ? "wait" : "Copy";
    if (!info->button->title || std::strcmp(info->button->title, label) != 0)
        setButtonTitle(*info->button, label);
    info->button->setState(sfDisabled, pending ? True : False);
}

void ChatWindow::updateCopyButtonPositions()
{
    if (!transcript)
        return;

    TRect column = copyColumnBounds();

    for (auto &info : copyButtons)
    {
        if (!info.button)
            continue;

        auto row = transcript->firstRowForMessage(info.messageIndex);
        if (!row.has_value())
        {
            info.button->setState(sfVisible, False);
            continue;
        }

        int relativeY = row.value() - transcript->delta.y;
        if (relativeY < 0 || relativeY >= transcript->size.y)
        {
            info.button->setState(sfVisible, False);
            continue;
        }

        int top = column.a.y + relativeY;
        if (top + 2 > column.b.y)
            top = column.b.y - 2;
        if (top < column.a.y)
            top = column.a.y;

        TRect current = info.button->getBounds();
        if (top != current.a.y)
        {
            current.a.y = top;
            current.b.y = top + 2;
            info.button->changeBounds(current);
        }
        info.button->setState(sfVisible, True);
    }
}

void ChatWindow::updateCopyButtons()
{
    if (!transcript)
        return;
    for (auto &info : copyButtons)
        updateCopyButtonState(info.messageIndex);

    updateCopyButtonPositions();
}

void ChatWindow::clearCopyButtons()
{
    for (auto &info : copyButtons)
    {
        if (info.button)
            TObject::destroy(info.button);
    }

    copyButtons.clear();
}

ChatWindow::CopyButtonInfo *ChatWindow::findCopyButton(std::size_t messageIndex)
{
    for (auto &info : copyButtons)
    {
        if (info.messageIndex == messageIndex)
            return &info;
    }
    return nullptr;
}

ChatWindow::CopyButtonInfo *ChatWindow::findCopyButtonByCommand(ushort command)
{
    for (auto &info : copyButtons)
    {
        if (info.command == command)
            return &info;
    }

    return nullptr;
}

TRect ChatWindow::copyColumnBounds() const
{
    if (!transcript)
        return TRect(0, 0, 0, 0);

    TRect transcriptBounds = transcript->getBounds();
    if (transcriptScrollBar)
    {
        TRect scrollBounds = transcriptScrollBar->getBounds();
        return TRect(transcriptBounds.b.x, transcriptBounds.a.y, scrollBounds.a.x, transcriptBounds.b.y);
    }

    return TRect(transcriptBounds.b.x, transcriptBounds.a.y, transcriptBounds.b.x, transcriptBounds.b.y);
}

void ChatWindow::updateTranscriptFromSession(bool forceScroll)
{
    if (!transcript)
        return;

    auto messages = session.snapshotMessages();
    transcript->setMessages(messages);
    if (forceScroll)
        transcript->scrollToBottom();
    transcript->drawView();

    for (std::size_t index = 0; index < messages.size(); ++index)
    {
        if (messages[index].role == ck::chat::ChatSession::Role::Assistant)
            ensureCopyButton(index);
    }

    updateCopyButtons();
    refreshWindowTitle();
}

void ChatWindow::showHiddenContent(const std::string &channel, const std::string &content)
{
    if (!TProgram::deskTop)
        return;

    std::string title = "Assistant";
    if (!channel.empty())
        title += " (" + channel + ")";

    TRect screen = TProgram::deskTop->getExtent();
    TRect bounds = screen;
    bounds.grow(-std::max<short>(5, static_cast<short>((screen.b.x - screen.a.x) / 6)),
                -std::max<short>(3, static_cast<short>((screen.b.y - screen.a.y) / 6)));
    if (bounds.b.x - bounds.a.x < 60)
        bounds.b.x = bounds.a.x + 70;
    if (bounds.b.y - bounds.a.y < 14)
        bounds.b.y = bounds.a.y + 16;

    auto *dialog = new TDialog(bounds, title.c_str());
    if (!dialog)
        return;

    TRect local = dialog->getExtent();
    local.grow(-2, -2);

    TRect memoRect = local;
    memoRect.b.y -= 3;
    if (memoRect.b.y <= memoRect.a.y + 1)
        memoRect.b.y = memoRect.a.y + 2;

    TRect vScrollRect(memoRect.b.x, memoRect.a.y, memoRect.b.x + 1, memoRect.b.y);
    auto *vScroll = new TScrollBar(vScrollRect);
    dialog->insert(vScroll);

    TRect hScrollRect(memoRect.a.x, memoRect.b.y, memoRect.b.x, memoRect.b.y + 1);
    auto *hScroll = new TScrollBar(hScrollRect);
    dialog->insert(hScroll);

    auto bufferSize = static_cast<int>(std::max<std::size_t>(content.size() + 256, 4096));
    auto *memo = new TMemo(memoRect, hScroll, vScroll, nullptr, bufferSize);
    dialog->insert(memo);

    auto encode = [](const std::string &text) {
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
    };

    const std::string encoded = encode(content);
    std::size_t limited = std::min<std::size_t>(encoded.size(), static_cast<std::size_t>(0xFFFF));
    std::vector<char> raw(sizeof(ushort) + std::max<std::size_t>(limited, std::size_t{1}));
    auto *memoData = reinterpret_cast<TMemoData *>(raw.data());
    memoData->length = static_cast<ushort>(limited);
    if (limited > 0)
        std::memcpy(memoData->buffer, encoded.data(), limited);
    memo->setData(memoData);
    memo->select();

    int dialogWidth = dialog->size.x;
    TRect okRect(dialogWidth / 2 - 6, memoRect.b.y + 1, dialogWidth / 2 + 6, memoRect.b.y + 3);
    auto *okButton = new TButton(okRect, "~O~K", cmOK, bfDefault);
    dialog->insert(okButton);

    TProgram::deskTop->insert(dialog);
    TProgram::deskTop->execView(dialog);
    destroy(dialog);
}
