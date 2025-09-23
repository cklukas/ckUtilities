#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TProgram
#define Uses_TRadioButtons
#define Uses_TStaticText
#include <tvision/tv.h>

namespace ck::find
{

bool editActionOptions(ActionOptions &options)
{
    struct Data
    {
        unsigned short flags = 0;
        unsigned short appendFlags = 0;
        unsigned short execVariant = 0;
        char execCommand[512]{};
        char fprintFile[PATH_MAX]{};
        char fprint0File[PATH_MAX]{};
        char flsFile[PATH_MAX]{};
        char printfFormat[256]{};
        char fprintfFile[PATH_MAX]{};
        char fprintfFormat[256]{};
    } data{};

    if (options.print)
        data.flags |= 0x0001;
    if (options.print0)
        data.flags |= 0x0002;
    if (options.ls)
        data.flags |= 0x0004;
    if (options.deleteMatches)
        data.flags |= 0x0008;
    if (options.quitEarly)
        data.flags |= 0x0010;
    if (options.execEnabled)
        data.flags |= 0x0020;
    if (options.execUsePlus)
        data.flags |= 0x0040;

    if (options.fprintAppend)
        data.appendFlags |= 0x0001;
    if (options.fprint0Append)
        data.appendFlags |= 0x0002;
    if (options.flsAppend)
        data.appendFlags |= 0x0004;
    if (options.fprintfAppend)
        data.appendFlags |= 0x0008;

    data.execVariant = static_cast<unsigned short>(options.execVariant);

    std::snprintf(data.execCommand, sizeof(data.execCommand), "%s", bufferToString(options.execCommand).c_str());
    std::snprintf(data.fprintFile, sizeof(data.fprintFile), "%s", bufferToString(options.fprintFile).c_str());
    std::snprintf(data.fprint0File, sizeof(data.fprint0File), "%s", bufferToString(options.fprint0File).c_str());
    std::snprintf(data.flsFile, sizeof(data.flsFile), "%s", bufferToString(options.flsFile).c_str());
    std::snprintf(data.printfFormat, sizeof(data.printfFormat), "%s", bufferToString(options.printfFormat).c_str());
    std::snprintf(data.fprintfFile, sizeof(data.fprintfFile), "%s", bufferToString(options.fprintfFile).c_str());
    std::snprintf(data.fprintfFormat, sizeof(data.fprintfFormat), "%s", bufferToString(options.fprintfFormat).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 78, 24), "Actions & Output");
    dialog->options |= ofCentered;

    auto *actionBoxes = new TCheckBoxes(TRect(3, 3, 30, 10),
                                        makeItemList({"-~p~rint",
                                                      "-print~0~",
                                                      "-~l~s",
                                                      "-~d~elete",
                                                      "-~q~uit",
                                                      "-~e~xec / -ok",
                                                      "Use '+' terminator"}));
    dialog->insert(actionBoxes);
    actionBoxes->setData(&data.flags);

    auto *execButtons = new TRadioButtons(TRect(32, 3, 60, 9),
                                          makeItemList({"-exec",
                                                        "-execdir",
                                                        "-ok",
                                                        "-okdir"}));
    dialog->insert(execButtons);
    execButtons->setData(&data.execVariant);

    auto *execInput = new TInputLine(TRect(3, 10, 74, 11), sizeof(data.execCommand) - 1);
    dialog->insert(new TLabel(TRect(3, 9, 36, 10), "Command ({} for path):", execInput));
    dialog->insert(execInput);
    execInput->setData(data.execCommand);

    auto *appendBoxes = new TCheckBoxes(TRect(3, 12, 30, 18),
                                        makeItemList({"Append -fprint",
                                                      "Append -fprint0",
                                                      "Append -fls",
                                                      "Append -fprintf"}));
    dialog->insert(appendBoxes);
    appendBoxes->setData(&data.appendFlags);

    auto pathLen = std::min<int>(static_cast<int>(sizeof(data.fprintFile)) - 1, 255);
    auto *fprintInput = new TInputLine(TRect(32, 12, 74, 13), pathLen);
    dialog->insert(new TLabel(TRect(32, 11, 58, 12), "-fprint file:", fprintInput));
    dialog->insert(fprintInput);
    fprintInput->setData(data.fprintFile);

    auto *fprint0Input = new TInputLine(TRect(32, 13, 74, 14), pathLen);
    dialog->insert(new TLabel(TRect(32, 12, 58, 13), "-fprint0 file:", fprint0Input));
    dialog->insert(fprint0Input);
    fprint0Input->setData(data.fprint0File);

    auto *flsInput = new TInputLine(TRect(32, 14, 74, 15), pathLen);
    dialog->insert(new TLabel(TRect(32, 13, 58, 14), "-fls file:", flsInput));
    dialog->insert(flsInput);
    flsInput->setData(data.flsFile);

    auto *printfInput = new TInputLine(TRect(32, 15, 74, 16), sizeof(data.printfFormat) - 1);
    dialog->insert(new TLabel(TRect(32, 14, 66, 15), "-printf format:", printfInput));
    dialog->insert(printfInput);
    printfInput->setData(data.printfFormat);

    auto *fprintfFileInput = new TInputLine(TRect(32, 16, 74, 17), pathLen);
    dialog->insert(new TLabel(TRect(32, 15, 66, 16), "-fprintf file:", fprintfFileInput));
    dialog->insert(fprintfFileInput);
    fprintfFileInput->setData(data.fprintfFile);

    auto *fprintfFormatInput = new TInputLine(TRect(32, 17, 74, 18), sizeof(data.fprintfFormat) - 1);
    dialog->insert(new TLabel(TRect(32, 16, 66, 17), "-fprintf format:", fprintfFormatInput));
    dialog->insert(fprintfFormatInput);
    fprintfFormatInput->setData(data.fprintfFormat);

    dialog->insert(new TStaticText(TRect(3, 18, 74, 20),
                                   "Commands use {} for the current path. Output files are optional;"
                                   " leave blank to skip."));

    dialog->insert(new TButton(TRect(30, 20, 40, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 20, 52, 22), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        actionBoxes->getData(&data.flags);
        appendBoxes->getData(&data.appendFlags);
        execButtons->getData(&data.execVariant);
        execInput->getData(data.execCommand);
        fprintInput->getData(data.fprintFile);
        fprint0Input->getData(data.fprint0File);
        flsInput->getData(data.flsFile);
        printfInput->getData(data.printfFormat);
        fprintfFileInput->getData(data.fprintfFile);
        fprintfFormatInput->getData(data.fprintfFormat);

        options.print = (data.flags & 0x0001) != 0;
        options.print0 = (data.flags & 0x0002) != 0;
        options.ls = (data.flags & 0x0004) != 0;
        options.deleteMatches = (data.flags & 0x0008) != 0;
        options.quitEarly = (data.flags & 0x0010) != 0;
        options.execEnabled = (data.flags & 0x0020) != 0;
        options.execUsePlus = (data.flags & 0x0040) != 0;
        options.execVariant = static_cast<ActionOptions::ExecVariant>(data.execVariant);

        options.fprintAppend = (data.appendFlags & 0x0001) != 0;
        options.fprint0Append = (data.appendFlags & 0x0002) != 0;
        options.flsAppend = (data.appendFlags & 0x0004) != 0;
        options.fprintfAppend = (data.appendFlags & 0x0008) != 0;

        copyToArray(options.execCommand, data.execCommand);
        copyToArray(options.fprintFile, data.fprintFile);
        copyToArray(options.fprint0File, data.fprint0File);
        copyToArray(options.flsFile, data.flsFile);
        copyToArray(options.printfFormat, data.printfFormat);
        copyToArray(options.fprintfFile, data.fprintfFile);
        copyToArray(options.fprintfFormat, data.fprintfFormat);

        options.fprintEnabled = data.fprintFile[0] != '\0';
        options.fprint0Enabled = data.fprint0File[0] != '\0';
        options.flsEnabled = data.flsFile[0] != '\0';
        options.printfEnabled = data.printfFormat[0] != '\0';
        options.fprintfEnabled = data.fprintfFile[0] != '\0' || data.fprintfFormat[0] != '\0';
    }

    TObject::destroy(dialog);
    return accepted;
}

} // namespace ck::find

