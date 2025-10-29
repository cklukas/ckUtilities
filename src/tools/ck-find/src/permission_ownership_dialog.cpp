#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"

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

bool editPermissionOwnership(PermissionOwnershipOptions &options)
{
    struct Data
    {
        unsigned short permFlags = 0;
        unsigned short ownerFlags = 0;
        unsigned short mode = 0;
        char permSpec[16]{};
        char user[64]{};
        char uid[32]{};
        char group[64]{};
        char gid[32]{};
    } data{};

    if (options.permEnabled)
        data.permFlags |= 0x0001;
    if (options.readable)
        data.permFlags |= 0x0002;
    if (options.writable)
        data.permFlags |= 0x0004;
    if (options.executable)
        data.permFlags |= 0x0008;
    data.mode = static_cast<unsigned short>(options.permMode);

    if (options.userEnabled)
        data.ownerFlags |= 0x0001;
    if (options.uidEnabled)
        data.ownerFlags |= 0x0002;
    if (options.groupEnabled)
        data.ownerFlags |= 0x0004;
    if (options.gidEnabled)
        data.ownerFlags |= 0x0008;
    if (options.noUser)
        data.ownerFlags |= 0x0010;
    if (options.noGroup)
        data.ownerFlags |= 0x0020;

    std::snprintf(data.permSpec, sizeof(data.permSpec), "%s", bufferToString(options.permSpec).c_str());
    std::snprintf(data.user, sizeof(data.user), "%s", bufferToString(options.user).c_str());
    std::snprintf(data.uid, sizeof(data.uid), "%s", bufferToString(options.uid).c_str());
    std::snprintf(data.group, sizeof(data.group), "%s", bufferToString(options.group).c_str());
    std::snprintf(data.gid, sizeof(data.gid), "%s", bufferToString(options.gid).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 70, 22), "Permissions & Ownership");
    dialog->options |= ofCentered;

    auto *permBoxes = new TCheckBoxes(TRect(3, 3, 26, 9),
                                      makeItemList({"Use -~p~erm",
                                                    "-~r~eadable",
                                                    "-~w~ritable",
                                                    "-~e~xecutable"}));
    dialog->insert(permBoxes);
    permBoxes->setData(&data.permFlags);

    auto *permMode = new TRadioButtons(TRect(28, 3, 52, 9),
                                       makeItemList({"Exact match",
                                                     "All bits (-perm -mode)",
                                                     "Any bit (-perm /mode)"}));
    dialog->insert(permMode);
    permMode->setData(&data.mode);

    auto *permInput = new TInputLine(TRect(3, 9, 52, 10), sizeof(data.permSpec) - 1);
    dialog->insert(new TLabel(TRect(3, 8, 28, 9), "-perm value:", permInput));
    dialog->insert(permInput);
    permInput->setData(data.permSpec);

    auto *ownerBoxes = new TCheckBoxes(TRect(3, 11, 26, 19),
                                       makeItemList({"Filter ~u~ser (-user)",
                                                     "Match ~U~ID (-uid)",
                                                     "Filter ~g~roup (-group)",
                                                     "Match ~G~ID (-gid)",
                                                     "-~n~ouser",
                                                     "-~n~ogroup"}));
    dialog->insert(ownerBoxes);
    ownerBoxes->setData(&data.ownerFlags);

    auto *userInput = new TInputLine(TRect(28, 11, 60, 12), sizeof(data.user) - 1);
    dialog->insert(new TLabel(TRect(28, 10, 60, 11), "User name:", userInput));
    dialog->insert(userInput);
    userInput->setData(data.user);

    auto *uidInput = new TInputLine(TRect(28, 12, 60, 13), sizeof(data.uid) - 1);
    dialog->insert(new TLabel(TRect(28, 11, 60, 12), "UID:", uidInput));
    dialog->insert(uidInput);
    uidInput->setData(data.uid);

    auto *groupInput = new TInputLine(TRect(28, 14, 60, 15), sizeof(data.group) - 1);
    dialog->insert(new TLabel(TRect(28, 13, 60, 14), "Group:", groupInput));
    dialog->insert(groupInput);
    groupInput->setData(data.group);

    auto *gidInput = new TInputLine(TRect(28, 15, 60, 16), sizeof(data.gid) - 1);
    dialog->insert(new TLabel(TRect(28, 14, 60, 15), "GID:", gidInput));
    dialog->insert(gidInput);
    gidInput->setData(data.gid);

    dialog->insert(new TStaticText(TRect(3, 19, 64, 21),
                                   "Specify numeric IDs or names. Leave unused fields blank."));

    dialog->insert(new TButton(TRect(24, 20, 34, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(36, 20, 46, 22), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        options.permEnabled = (data.permFlags & 0x0001) != 0;
        options.readable = (data.permFlags & 0x0002) != 0;
        options.writable = (data.permFlags & 0x0004) != 0;
        options.executable = (data.permFlags & 0x0008) != 0;
        options.permMode = static_cast<PermissionOwnershipOptions::PermMode>(data.mode);

        options.userEnabled = (data.ownerFlags & 0x0001) != 0;
        options.uidEnabled = (data.ownerFlags & 0x0002) != 0;
        options.groupEnabled = (data.ownerFlags & 0x0004) != 0;
        options.gidEnabled = (data.ownerFlags & 0x0008) != 0;
        options.noUser = (data.ownerFlags & 0x0010) != 0;
        options.noGroup = (data.ownerFlags & 0x0020) != 0;

        copyToArray(options.permSpec, data.permSpec);
        copyToArray(options.user, data.user);
        copyToArray(options.uid, data.uid);
        copyToArray(options.group, data.group);
        copyToArray(options.gid, data.gid);
    }

    return accepted;
}

} // namespace ck::find
