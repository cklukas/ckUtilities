#include "chat_window.hpp"
#include "chat_app.hpp"
#include "chat_transcript_view.hpp"
#include "prompt_input_view.hpp"
#include "../clipboard.hpp"
#include "../commands.hpp"

#include <algorithm>
#include <cstring>

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

    app.registerWindow(this);
    newConversation();
}

TPalette &ChatWindow::getPalette() const
{
    static TPalette palette(cpGrayDialog, sizeof(cpGrayDialog) - 1);
    return palette;
}

void ChatWindow::handleEvent(TEvent &event)
{
    if (event.what == evKeyDown && event.keyDown.keyCode == kbAltS)
    {
        sendPrompt();
        clearEvent(event);
        return;
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
}

void ChatWindow::newConversation()
{
    session.resetConversation();
    session.consumeDirtyFlag();
    clearCopyButtons();
    if (promptInput)
    {
        promptInput->clearText();
        promptInput->select();
    }
    updateTranscriptFromSession(true);
}

void ChatWindow::sendPrompt()
{
    if (!promptInput || !transcript)
        return;

    std::string prompt = promptInput->text();
    if (prompt.empty())
        return;

    session.addUserMessage(prompt);
    session.startAssistantResponse(prompt);
    promptInput->clearText();
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
}