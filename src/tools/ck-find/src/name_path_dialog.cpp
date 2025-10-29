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

bool editNamePathOptions(NamePathOptions &options)
{
    struct Data
    {
        unsigned short flags = 0;
        unsigned short pruneFlags = 0;
        unsigned short pruneMode = 0;
        char name[256]{};
        char iname[256]{};
        char path[256]{};
        char ipath[256]{};
        char regex[256]{};
        char iregex[256]{};
        char lname[256]{};
        char ilname[256]{};
        char prune[256]{};
    } data{};

    if (options.nameEnabled)
        data.flags |= 0x0001;
    if (options.inameEnabled)
        data.flags |= 0x0002;
    if (options.pathEnabled)
        data.flags |= 0x0004;
    if (options.ipathEnabled)
        data.flags |= 0x0008;
    if (options.regexEnabled)
        data.flags |= 0x0010;
    if (options.iregexEnabled)
        data.flags |= 0x0020;
    if (options.lnameEnabled)
        data.flags |= 0x0040;
    if (options.ilnameEnabled)
        data.flags |= 0x0080;
    if (options.pruneEnabled)
        data.pruneFlags |= 0x0001;
    if (options.pruneDirectoriesOnly)
        data.pruneFlags |= 0x0002;
    data.pruneMode = static_cast<unsigned short>(options.pruneTest);

    std::snprintf(data.name, sizeof(data.name), "%s", bufferToString(options.namePattern).c_str());
    std::snprintf(data.iname, sizeof(data.iname), "%s", bufferToString(options.inamePattern).c_str());
    std::snprintf(data.path, sizeof(data.path), "%s", bufferToString(options.pathPattern).c_str());
    std::snprintf(data.ipath, sizeof(data.ipath), "%s", bufferToString(options.ipathPattern).c_str());
    std::snprintf(data.regex, sizeof(data.regex), "%s", bufferToString(options.regexPattern).c_str());
    std::snprintf(data.iregex, sizeof(data.iregex), "%s", bufferToString(options.iregexPattern).c_str());
    std::snprintf(data.lname, sizeof(data.lname), "%s", bufferToString(options.lnamePattern).c_str());
    std::snprintf(data.ilname, sizeof(data.ilname), "%s", bufferToString(options.ilnamePattern).c_str());
    std::snprintf(data.prune, sizeof(data.prune), "%s", bufferToString(options.prunePattern).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 80, 24), "Name and Path Tests");
    dialog->options |= ofCentered;

    dialog->insert(new TStaticText(TRect(3, 2, 76, 4),
                                   "Enable matchers and provide glob or regex values.\n"
                                   "Leave a field blank to skip that test."));

    auto *patternBoxes = new TCheckBoxes(TRect(3, 4, 28, 16),
                                         makeItemList({"~N~ame (-name)",
                                                       "Case-insensitive ~n~ame (-iname)",
                                                       "~P~ath (-path)",
                                                       "Case-insensitive pa~t~h (-ipath)",
                                                       "Regular e~x~pression (-regex)",
                                                       "Case-insensitive re~g~ex (-iregex)",
                                                       "Symlink ~l~name (-lname)",
                                                       "Case-insensitive l~n~ame (-ilname)"}));
    dialog->insert(patternBoxes);
    patternBoxes->setData(&data.flags);

    auto *nameInput = new TInputLine(TRect(30, 4, 56, 5), sizeof(data.name) - 1);
    dialog->insert(new TLabel(TRect(30, 3, 56, 4), "~N~ame pattern:", nameInput));
    dialog->insert(nameInput);
    nameInput->setData(data.name);

    auto *inameInput = new TInputLine(TRect(30, 6, 56, 7), sizeof(data.iname) - 1);
    dialog->insert(new TLabel(TRect(30, 5, 56, 6), "Case-insensitive ~n~ame:", inameInput));
    dialog->insert(inameInput);
    inameInput->setData(data.iname);

    auto *pathInput = new TInputLine(TRect(30, 8, 56, 9), sizeof(data.path) - 1);
    dialog->insert(new TLabel(TRect(30, 7, 56, 8), "~P~ath glob:", pathInput));
    dialog->insert(pathInput);
    pathInput->setData(data.path);

    auto *ipathInput = new TInputLine(TRect(30, 10, 56, 11), sizeof(data.ipath) - 1);
    dialog->insert(new TLabel(TRect(30, 9, 56, 10), "Case-insensitive pa~t~h:", ipathInput));
    dialog->insert(ipathInput);
    ipathInput->setData(data.ipath);

    auto *regexInput = new TInputLine(TRect(58, 4, 78, 5), sizeof(data.regex) - 1);
    dialog->insert(new TLabel(TRect(58, 3, 78, 4), "Re~g~ex (-regex):", regexInput));
    dialog->insert(regexInput);
    regexInput->setData(data.regex);

    auto *iregexInput = new TInputLine(TRect(58, 6, 78, 7), sizeof(data.iregex) - 1);
    dialog->insert(new TLabel(TRect(58, 5, 78, 6), "Case-insensitive re~g~ex:", iregexInput));
    dialog->insert(iregexInput);
    iregexInput->setData(data.iregex);

    auto *lnameInput = new TInputLine(TRect(58, 8, 78, 9), sizeof(data.lname) - 1);
    dialog->insert(new TLabel(TRect(58, 7, 78, 8), "Symlink ~l~name:", lnameInput));
    dialog->insert(lnameInput);
    lnameInput->setData(data.lname);

    auto *ilnameInput = new TInputLine(TRect(58, 10, 78, 11), sizeof(data.ilname) - 1);
    dialog->insert(new TLabel(TRect(58, 9, 78, 10), "Case-insensitive l~n~ame:", ilnameInput));
    dialog->insert(ilnameInput);
    ilnameInput->setData(data.ilname);

    auto *pruneBoxes = new TCheckBoxes(TRect(3, 16, 28, 20),
                                       makeItemList({"Enable -p~r~une",
                                                     "Directories ~o~nly"}));
    dialog->insert(pruneBoxes);
    pruneBoxes->setData(&data.pruneFlags);

    auto *pruneModeButtons = new TRadioButtons(TRect(30, 16, 78, 21),
                                               makeItemList({"Use -name",
                                                             "Use -iname",
                                                             "Use -path",
                                                             "Use -ipath",
                                                             "Use -regex",
                                                             "Use -iregex"}));
    dialog->insert(pruneModeButtons);
    pruneModeButtons->setData(&data.pruneMode);

    auto *pruneInput = new TInputLine(TRect(30, 21, 78, 22), sizeof(data.prune) - 1);
    dialog->insert(new TLabel(TRect(30, 20, 74, 21), "-prune pattern:", pruneInput));
    dialog->insert(pruneInput);
    pruneInput->setData(data.prune);

    dialog->insert(new TButton(TRect(30, 22, 40, 24), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 22, 52, 24), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        options.nameEnabled = (data.flags & 0x0001) != 0;
        options.inameEnabled = (data.flags & 0x0002) != 0;
        options.pathEnabled = (data.flags & 0x0004) != 0;
        options.ipathEnabled = (data.flags & 0x0008) != 0;
        options.regexEnabled = (data.flags & 0x0010) != 0;
        options.iregexEnabled = (data.flags & 0x0020) != 0;
        options.lnameEnabled = (data.flags & 0x0040) != 0;
        options.ilnameEnabled = (data.flags & 0x0080) != 0;
        options.pruneEnabled = (data.pruneFlags & 0x0001) != 0;
        options.pruneDirectoriesOnly = (data.pruneFlags & 0x0002) != 0;
        options.pruneTest = static_cast<NamePathOptions::PruneTest>(data.pruneMode);

        copyToArray(options.namePattern, data.name);
        copyToArray(options.inamePattern, data.iname);
        copyToArray(options.pathPattern, data.path);
        copyToArray(options.ipathPattern, data.ipath);
        copyToArray(options.regexPattern, data.regex);
        copyToArray(options.iregexPattern, data.iregex);
        copyToArray(options.lnamePattern, data.lname);
        copyToArray(options.ilnamePattern, data.ilname);
        copyToArray(options.prunePattern, data.prune);
    }

    return accepted;
}

} // namespace ck::find
