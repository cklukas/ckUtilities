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

bool editTraversalFilters(TraversalFilesystemOptions &options)
{
    struct Data
    {
        unsigned short flags = 0;
        unsigned short valueFlags = 0;
        unsigned short symlinkMode = 0;
        unsigned short warningMode = 0;
        char maxDepth[8]{};
        char minDepth[8]{};
        char filesFrom[PATH_MAX]{};
        char fsType[64]{};
        char linkCount[16]{};
        char sameFile[PATH_MAX]{};
        char inode[32]{};
    } data{};

    if (options.depthFirst)
        data.flags |= 0x0001;
    if (options.stayOnFilesystem)
        data.flags |= 0x0002;
    if (options.assumeNoLeaf)
        data.flags |= 0x0004;
    if (options.ignoreReaddirRace)
        data.flags |= 0x0008;
    if (options.dayStart)
        data.flags |= 0x0010;

    if (options.maxDepthEnabled)
        data.valueFlags |= 0x0001;
    if (options.minDepthEnabled)
        data.valueFlags |= 0x0002;
    if (options.filesFromEnabled)
        data.valueFlags |= 0x0004;
    if (options.filesFromNullSeparated)
        data.valueFlags |= 0x0008;
    if (options.fstypeEnabled)
        data.valueFlags |= 0x0010;
    if (options.linksEnabled)
        data.valueFlags |= 0x0020;
    if (options.sameFileEnabled)
        data.valueFlags |= 0x0040;
    if (options.inumEnabled)
        data.valueFlags |= 0x0080;

    data.symlinkMode = static_cast<unsigned short>(options.symlinkMode);
    data.warningMode = static_cast<unsigned short>(options.warningMode);

    std::snprintf(data.maxDepth, sizeof(data.maxDepth), "%s", bufferToString(options.maxDepth).c_str());
    std::snprintf(data.minDepth, sizeof(data.minDepth), "%s", bufferToString(options.minDepth).c_str());
    std::snprintf(data.filesFrom, sizeof(data.filesFrom), "%s", bufferToString(options.filesFrom).c_str());
    std::snprintf(data.fsType, sizeof(data.fsType), "%s", bufferToString(options.fsType).c_str());
    std::snprintf(data.linkCount, sizeof(data.linkCount), "%s", bufferToString(options.linkCount).c_str());
    std::snprintf(data.sameFile, sizeof(data.sameFile), "%s", bufferToString(options.sameFile).c_str());
    std::snprintf(data.inode, sizeof(data.inode), "%s", bufferToString(options.inode).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 80, 24), "Traversal & Filesystem");
    dialog->options |= ofCentered;

    auto *symlinkButtons = new TRadioButtons(TRect(3, 3, 30, 9),
                                             makeItemList({"Physical (-P)",
                                                           "Follow args (-H)",
                                                           "Follow all (-L)"}));
    dialog->insert(symlinkButtons);
    symlinkButtons->setData(&data.symlinkMode);

    auto *warningButtons = new TRadioButtons(TRect(32, 3, 64, 8),
                                             makeItemList({"Default warnings",
                                                           "Always warn (-warn)",
                                                           "Suppress (-nowarn)"}));
    dialog->insert(warningButtons);
    warningButtons->setData(&data.warningMode);

    auto *flagBoxes = new TCheckBoxes(TRect(3, 9, 30, 17),
                                      makeItemList({"Use -~d~epth",
                                                    "Stay on file~s~ystem",
                                                    "Assume -nolea~f~",
                                                    "Ignore readdir race",
                                                    "Use -day~s~tart"}));
    dialog->insert(flagBoxes);
    flagBoxes->setData(&data.flags);

    auto *valueBoxes = new TCheckBoxes(TRect(32, 9, 60, 17),
                                       makeItemList({"Limit ~m~ax depth",
                                                     "Limit mi~n~ depth",
                                                     "Paths from ~f~ile",
                                                     "List is ~N~UL separated",
                                                     "Filter ~f~stype",
                                                     "Match ~l~inks",
                                                     "Match ~s~amefile",
                                                     "Match ~i~node"}));
    dialog->insert(valueBoxes);
    valueBoxes->setData(&data.valueFlags);

    auto *maxInput = new TInputLine(TRect(62, 9, 72, 10), sizeof(data.maxDepth) - 1);
    dialog->insert(new TLabel(TRect(60, 9, 62, 10), "Max:", maxInput));
    dialog->insert(maxInput);
    maxInput->setData(data.maxDepth);

    auto *minInput = new TInputLine(TRect(62, 10, 72, 11), sizeof(data.minDepth) - 1);
    dialog->insert(new TLabel(TRect(60, 10, 62, 11), "Min:", minInput));
    dialog->insert(minInput);
    minInput->setData(data.minDepth);

    auto *filesFromInput = new TInputLine(TRect(32, 12, 78, 13), std::min<int>(static_cast<int>(sizeof(data.filesFrom)) - 1, 255));
    dialog->insert(new TLabel(TRect(32, 11, 66, 12), "-files-from list:", filesFromInput));
    dialog->insert(filesFromInput);
    filesFromInput->setData(data.filesFrom);

    auto *fsTypeInput = new TInputLine(TRect(32, 13, 64, 14), sizeof(data.fsType) - 1);
    dialog->insert(new TLabel(TRect(32, 12, 64, 13), "Filesystem type:", fsTypeInput));
    dialog->insert(fsTypeInput);
    fsTypeInput->setData(data.fsType);

    auto *linkCountInput = new TInputLine(TRect(32, 14, 46, 15), sizeof(data.linkCount) - 1);
    dialog->insert(new TLabel(TRect(32, 13, 46, 14), "Links:", linkCountInput));
    dialog->insert(linkCountInput);
    linkCountInput->setData(data.linkCount);

    auto *sameFileInput = new TInputLine(TRect(32, 15, 78, 16), std::min<int>(static_cast<int>(sizeof(data.sameFile)) - 1, 255));
    dialog->insert(new TLabel(TRect(32, 14, 70, 15), "-samefile target:", sameFileInput));
    dialog->insert(sameFileInput);
    sameFileInput->setData(data.sameFile);

    auto *inodeInput = new TInputLine(TRect(32, 16, 46, 17), sizeof(data.inode) - 1);
    dialog->insert(new TLabel(TRect(32, 15, 46, 16), "Inode:", inodeInput));
    dialog->insert(inodeInput);
    inodeInput->setData(data.inode);

    dialog->insert(new TStaticText(TRect(3, 17, 76, 19),
                                   "Combine traversal flags to fine-tune walking order and filesystem"
                                   " scoping."));

    dialog->insert(new TButton(TRect(30, 20, 40, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 20, 52, 22), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        symlinkButtons->getData(&data.symlinkMode);
        warningButtons->getData(&data.warningMode);
        flagBoxes->getData(&data.flags);
        valueBoxes->getData(&data.valueFlags);
        maxInput->getData(data.maxDepth);
        minInput->getData(data.minDepth);
        filesFromInput->getData(data.filesFrom);
        fsTypeInput->getData(data.fsType);
        linkCountInput->getData(data.linkCount);
        sameFileInput->getData(data.sameFile);
        inodeInput->getData(data.inode);

        options.depthFirst = (data.flags & 0x0001) != 0;
        options.stayOnFilesystem = (data.flags & 0x0002) != 0;
        options.assumeNoLeaf = (data.flags & 0x0004) != 0;
        options.ignoreReaddirRace = (data.flags & 0x0008) != 0;
        options.dayStart = (data.flags & 0x0010) != 0;

        options.maxDepthEnabled = (data.valueFlags & 0x0001) != 0;
        options.minDepthEnabled = (data.valueFlags & 0x0002) != 0;
        options.filesFromEnabled = (data.valueFlags & 0x0004) != 0;
        options.filesFromNullSeparated = (data.valueFlags & 0x0008) != 0;
        options.fstypeEnabled = (data.valueFlags & 0x0010) != 0;
        options.linksEnabled = (data.valueFlags & 0x0020) != 0;
        options.sameFileEnabled = (data.valueFlags & 0x0040) != 0;
        options.inumEnabled = (data.valueFlags & 0x0080) != 0;

        options.symlinkMode = static_cast<TraversalFilesystemOptions::SymlinkMode>(data.symlinkMode);
        options.warningMode = static_cast<TraversalFilesystemOptions::WarningMode>(data.warningMode);

        copyToArray(options.maxDepth, data.maxDepth);
        copyToArray(options.minDepth, data.minDepth);
        copyToArray(options.filesFrom, data.filesFrom);
        copyToArray(options.fsType, data.fsType);
        copyToArray(options.linkCount, data.linkCount);
        copyToArray(options.sameFile, data.sameFile);
        copyToArray(options.inode, data.inode);
    }

    TObject::destroy(dialog);
    return accepted;
}

} // namespace ck::find

