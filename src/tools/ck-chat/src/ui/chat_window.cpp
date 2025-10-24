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

namespace
{
    constexpr int kScrollBarWidth = 1;
    constexpr int kCopyButtonColumnWidth = 12;
}

class PanelFrame : public TView
{
public:
    PanelFrame(const TRect &bounds, std::string titleText)
        : TView(bounds), title_(std::move(titleText))
    {
        options &= ~(ofSelectable | ofFirstClick);
        eventMask = 0;
    }

    void setTitle(const std::string &titleText)
    {
        title_ = titleText;
        drawView();
    }

    virtual void draw() override
    {
        if (size.x <= 0 || size.y <= 0)
            return;

        TDrawBuffer buffer;
        auto colors = getColor(1);
        TColorAttr attr = colors[0];
        const char kUpperLeft = '\xDA';
        const char kUpperRight = '\xBF';
        const char kLowerLeft = '\xC0';
        const char kLowerRight = '\xD9';
        const char kHorizontal = '\xC4';
        const char kVertical = '\xB3';

        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', attr, size.x);
            if (y == 0)
            {
                buffer.putChar(0, kUpperLeft);
                if (size.x > 1)
                    buffer.putChar(size.x - 1, kUpperRight);
                if (size.x > 2)
                    buffer.moveChar(1, kHorizontal, attr, size.x - 2);

                if (!title_.empty() && size.x > 4)
                {
                    std::string display = title_;
                    if (static_cast<int>(display.size()) > size.x - 4)
                        display.resize(static_cast<std::size_t>(size.x - 4));
                    int start = 2;
                    for (std::size_t i = 0; i < display.size(); ++i)
                        buffer.putChar(static_cast<int>(start + i), display[i]);
                }
            }
            else if (y == size.y - 1)
            {
                buffer.putChar(0, kLowerLeft);
                if (size.x > 1)
                    buffer.putChar(size.x - 1, kLowerRight);
                if (size.x > 2)
                    buffer.moveChar(1, kHorizontal, attr, size.x - 2);
            }
            else
            {
                buffer.putChar(0, kVertical);
                if (size.x > 1)
                    buffer.putChar(size.x - 1, kVertical);
            }
            writeLine(0, y, size.x, 1, buffer);
        }
    }

private:
    std::string title_;
};

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

    const int inputLines = 4; // number of lines for the prompt input area
    const int transcriptScrollWidth = kScrollBarWidth;
    const int inputScrollWidth = 1;
    const int buttonWidth = 12; // width of the submit button
    const int buttonHeight = 2;

    short promptFrameHeight = static_cast<short>(inputLines + 2);
    if (promptFrameHeight < 3)
        promptFrameHeight = 3;

    short promptFrameTop = static_cast<short>(extent.b.y - promptFrameHeight);
    if (promptFrameTop <= extent.a.y + 2)
        promptFrameTop = static_cast<short>(extent.a.y + 3);

    TRect promptFrameRect(extent.a.x, promptFrameTop, extent.b.x, extent.b.y);
    auto *promptFrame = new PanelFrame(promptFrameRect, "Prompt");
    promptFrame->growMode = gfGrowHiX | gfGrowLoY | gfGrowHiY;
    insert(promptFrame);

    TRect transcriptFrameRect = extent;
    transcriptFrameRect.b.y = promptFrameTop;
    if (transcriptFrameRect.b.y <= transcriptFrameRect.a.y + 2)
        transcriptFrameRect.b.y = static_cast<short>(transcriptFrameRect.a.y + 3);

    auto *transcriptFrame = new PanelFrame(transcriptFrameRect, "AI Chat");
    transcriptFrame->growMode = gfGrowHiX | gfGrowHiY;
    insert(transcriptFrame);

    TRect transcriptInterior = transcriptFrameRect;
    transcriptInterior.grow(-1, -1);
    if (transcriptInterior.b.x <= transcriptInterior.a.x + 1)
        transcriptInterior.b.x = static_cast<short>(transcriptInterior.a.x + 2);
    if (transcriptInterior.b.y <= transcriptInterior.a.y + 1)
        transcriptInterior.b.y = static_cast<short>(transcriptInterior.a.y + 2);

    short transcriptScrollLeft = static_cast<short>(transcriptInterior.b.x - transcriptScrollWidth);
    if (transcriptScrollLeft <= transcriptInterior.a.x)
        transcriptScrollLeft = static_cast<short>(transcriptInterior.a.x + 1);
    TRect transcriptScrollRect(transcriptScrollLeft + 1, transcriptInterior.a.y,
                               transcriptInterior.b.x + 1, transcriptInterior.b.y);

    auto *transcriptScroll = new TScrollBar(transcriptScrollRect);
    transcriptScroll->growMode = gfGrowLoX | gfGrowHiX | gfGrowLoY | gfGrowHiY;
    transcriptScroll->setState(sfVisible, True);
    insert(transcriptScroll);
    transcriptScrollBar = transcriptScroll;

    TRect transcriptRect(transcriptInterior.a.x, transcriptInterior.a.y,
                         transcriptScrollLeft, transcriptInterior.b.y);
    if (transcriptRect.b.x <= transcriptRect.a.x)
        transcriptRect.b.x = static_cast<short>(transcriptRect.a.x + 1);

    transcript = new ChatTranscriptView(transcriptRect, nullptr, transcriptScroll);
    transcript->growMode = gfGrowHiX | gfGrowHiY;
    transcript->setLayoutChangedCallback([this](bool userScroll)
                                         { onTranscriptLayoutChanged(userScroll); });
    transcript->setShowThinking(showThinking_);
    transcript->setShowAnalysis(showAnalysis_);
    transcript->setHiddenDetailCallback([this](std::size_t messageIndex, const std::string &channel,
                                               const std::string &content)
                                        {
                                            (void)messageIndex;
                                            showHiddenContent(channel, content); });
    insert(transcript);

    TRect promptInterior = promptFrameRect;
    promptInterior.grow(-1, -1);
    if (promptInterior.b.x <= promptInterior.a.x + 1)
        promptInterior.b.x = static_cast<short>(promptInterior.a.x + 2);
    if (promptInterior.b.y <= promptInterior.a.y + 1)
        promptInterior.b.y = static_cast<short>(promptInterior.a.y + 2);

    int promptRight = promptInterior.b.x - (buttonWidth + inputScrollWidth);
    if (promptRight <= promptInterior.a.x + 1)
        promptRight = promptInterior.a.x + 2;
    int scrollLeft = promptRight;
    int buttonLeft = promptInterior.b.x - buttonWidth;
    if (buttonLeft < scrollLeft + inputScrollWidth + 1)
        buttonLeft = scrollLeft + inputScrollWidth + 1;
    if (buttonLeft >= promptInterior.b.x)
        buttonLeft = promptInterior.b.x - 1;

    TRect promptScrollRect(scrollLeft, promptInterior.a.y,
                           static_cast<short>(scrollLeft + inputScrollWidth), promptInterior.b.y);
    promptScrollBar = new TScrollBar(promptScrollRect);
    promptScrollBar->growMode = gfGrowLoY | gfGrowHiY | gfGrowLoX | gfGrowHiX;
    promptScrollBar->setState(sfVisible, True);
    insert(promptScrollBar);

    TRect promptRect(promptInterior.a.x, promptInterior.a.y,
                     scrollLeft, promptInterior.b.y);
    if (promptRect.b.x <= promptRect.a.x)
        promptRect.b.x = static_cast<short>(promptRect.a.x + 1);
    promptInput = new PromptInputView(promptRect, nullptr, promptScrollBar);
    promptInput->growMode = gfGrowHiX | gfGrowLoY | gfGrowHiY;
    insert(promptInput);

    int promptContentHeight = promptInterior.b.y - promptInterior.a.y;
    int buttonTop = promptInterior.a.y + std::max(0, (promptContentHeight - buttonHeight) / 2);
    short buttonRight = static_cast<short>(std::min(promptInterior.b.x,
                                                    buttonLeft + buttonWidth));
    if (buttonRight <= buttonLeft)
        buttonRight = static_cast<short>(buttonLeft + 1);
    short buttonBottom = static_cast<short>(std::min(promptInterior.b.y,
                                                     buttonTop + buttonHeight));
    if (buttonBottom <= buttonTop)
        buttonBottom = static_cast<short>(buttonTop + 1);

    TRect buttonRect(static_cast<short>(buttonLeft), static_cast<short>(buttonTop),
                     buttonRight, buttonBottom);

    submitButton = new TButton(buttonRect, "~S~ubmit", cmSendPrompt, bfDefault);
    submitButton->growMode = gfGrowLoX | gfGrowHiX | gfGrowLoY | gfGrowHiY;
    submitButton->setState(sfVisible, True);

    insert(submitButton);

    promptInput->select();

    session.setLogSink([this](const std::string &entry)
                       { app.appendLog(entry); });
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
        if (command == cmCopyLastResponse)
        {
            copyLastAssistantMessage();
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

    if (transcriptScrollBar)
        transcriptScrollBar->drawView();
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
    autoScrollEnabled_ = true;
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
    autoScrollEnabled_ = true;
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

void ChatWindow::copyLastAssistantMessage()
{
    if (!transcript)
        return;

    auto indexOpt = transcript->lastAssistantMessageIndex();
    if (!indexOpt)
    {
        messageBox("No completed assistant response to copy.",
                   mfInformation | mfOKButton);
        return;
    }

    copyAssistantMessage(*indexOpt);
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
    constexpr int kMinButtonWidth = 6;
    if (column.b.x - column.a.x < kMinButtonWidth)
        return;

    const int buttonHeight = 2;
    int top = column.a.y;
    int bottom = std::min(column.b.y, top + buttonHeight);
    if (bottom <= top)
        bottom = top + buttonHeight;

    int right = column.b.x;

    TRect initialBounds(column.a.x, top, right, bottom);
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
    if (modelInfo)
        titleStream << modelInfo->name;
    else
        titleStream << "No Model";

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

void ChatWindow::clearCopyColumnBackground()
{
    if (!transcript)
        return;

    TRect transcriptBounds = transcript->getBounds();
    int height = transcriptBounds.b.y - transcriptBounds.a.y;
    if (height <= 0)
        return;

    short textRight = transcriptBounds.b.x;
    short scrollLeft = textRight;
    short scrollRight = static_cast<short>(scrollLeft + kScrollBarWidth);
    if (transcriptScrollBar)
    {
        TRect scrollBounds = transcriptScrollBar->getBounds();
        scrollLeft = scrollBounds.a.x;
        scrollRight = scrollBounds.b.x;
    }
    short copyLeft = scrollRight;
    short copyRight = static_cast<short>(copyLeft + kCopyButtonColumnWidth);

    if (copyLeft < scrollRight)
        copyLeft = scrollRight;
    if (copyRight < copyLeft)
        copyRight = static_cast<short>(copyLeft);

    int copyWidth = copyRight - copyLeft;
    // if (copyWidth > 0)
    // {
    //     auto colors = getColor(1);
    //     TColorAttr copyAttr = colors[0];
    //     setBack(copyAttr, TColorDesired(TColorBIOS(0x02))); // green background for button lane
    //     TDrawBuffer copyBuffer;
    //     copyBuffer.moveChar(0, ' ', copyAttr, copyWidth);
    //     for (int y = transcriptBounds.a.y; y < transcriptBounds.b.y; ++y)
    //         writeLine(copyLeft, y, copyWidth, 1, copyBuffer);
    // }

    // if (transcriptScrollBar)
    //    transcriptScrollBar->drawView();
}

void ChatWindow::updateCopyButtonPositions()
{
    return;

    if (!transcript)
        return;

    TRect column = copyColumnBounds();
    constexpr int kButtonHeight = 2;
    constexpr int kMinButtonWidth = 6;

    // clearCopyColumnBackground();
    int rightEdge = column.b.x;

    for (auto &info : copyButtons)
    {
        if (!info.button)
            continue;

        if (column.b.x - column.a.x < kMinButtonWidth)
        {
            info.button->setState(sfVisible, False);
            continue;
        }

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
        if (top + kButtonHeight > column.b.y)
            top = column.b.y - kButtonHeight;
        if (top < column.a.y)
            top = column.a.y;

        TRect current = info.button->getBounds();
        int bottom = std::min(column.b.y, top + kButtonHeight);
        if (bottom <= top)
            bottom = top + kButtonHeight;

        bool needsUpdate = (current.a.x != column.a.x) ||
                           (current.b.x != rightEdge) ||
                           (current.a.y != top) ||
                           (current.b.y != bottom);

        if (needsUpdate)
        {
            TRect desired(column.a.x, top, rightEdge, bottom);
            info.button->changeBounds(desired);
        }

        info.button->setState(sfVisible, True);
        info.button->drawView();
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

void ChatWindow::onTranscriptLayoutChanged(bool userScroll)
{
    if (!transcript)
        return;

    if (userScroll)
    {
        if (!transcript->isAtBottom())
            autoScrollEnabled_ = false;
        else
            autoScrollEnabled_ = true;
    }
    else if (!autoScrollEnabled_ && transcript->isAtBottom())
    {
        autoScrollEnabled_ = true;
    }

    updateCopyButtons();
    if (transcriptScrollBar)
        transcriptScrollBar->drawView();
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
    short top = transcriptBounds.a.y;
    short bottom = transcriptBounds.b.y;
    short textRight = transcriptBounds.b.x;
    short left = static_cast<short>(textRight + kScrollBarWidth);
    short right = static_cast<short>(left + kCopyButtonColumnWidth);
    if (transcriptScrollBar)
    {
        TRect scrollBounds = transcriptScrollBar->getBounds();
        left = static_cast<short>(scrollBounds.b.x);
        right = static_cast<short>(left + kCopyButtonColumnWidth);
    }
    if (right <= left)
        right = static_cast<short>(left + 1);
    return TRect(left, top, right, bottom);
}

void ChatWindow::updateTranscriptFromSession(bool forceScroll)
{
    if (!transcript)
        return;

    auto messages = session.snapshotMessages();
    transcript->setMessages(messages);
    bool shouldAutoScroll = forceScroll && autoScrollEnabled_;
    if (shouldAutoScroll)
        transcript->scrollToBottom();
    else if (!autoScrollEnabled_ && transcript->isAtBottom())
        autoScrollEnabled_ = true;
    transcript->drawView();
    if (transcriptScrollBar)
        transcriptScrollBar->drawView();

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

    auto encode = [](const std::string &text)
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
