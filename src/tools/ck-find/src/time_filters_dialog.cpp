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

bool editTimeFilters(TimeFilterOptions &options)
{
    struct Data
    {
        unsigned short preset = 0;
        unsigned short fields = 0;
        char from[32]{};
        char to[32]{};
        char mtime[16]{};
        char mmin[16]{};
        char atime[16]{};
        char amin[16]{};
        char ctime[16]{};
        char cmin[16]{};
        char used[16]{};
        char newer[PATH_MAX]{};
        char anewer[PATH_MAX]{};
        char cnewer[PATH_MAX]{};
        char newermt[64]{};
        char newerat[64]{};
        char newerct[64]{};
    } data{};

    data.preset = static_cast<unsigned short>(options.preset);
    if (options.includeModified)
        data.fields |= 0x0001;
    if (options.includeCreated)
        data.fields |= 0x0002;
    if (options.includeAccessed)
        data.fields |= 0x0004;

    std::snprintf(data.from, sizeof(data.from), "%s", bufferToString(options.customFrom).c_str());
    std::snprintf(data.to, sizeof(data.to), "%s", bufferToString(options.customTo).c_str());
    std::snprintf(data.mtime, sizeof(data.mtime), "%s", bufferToString(options.mtime).c_str());
    std::snprintf(data.mmin, sizeof(data.mmin), "%s", bufferToString(options.mmin).c_str());
    std::snprintf(data.atime, sizeof(data.atime), "%s", bufferToString(options.atime).c_str());
    std::snprintf(data.amin, sizeof(data.amin), "%s", bufferToString(options.amin).c_str());
    std::snprintf(data.ctime, sizeof(data.ctime), "%s", bufferToString(options.ctime).c_str());
    std::snprintf(data.cmin, sizeof(data.cmin), "%s", bufferToString(options.cmin).c_str());
    std::snprintf(data.used, sizeof(data.used), "%s", bufferToString(options.used).c_str());
    std::snprintf(data.newer, sizeof(data.newer), "%s", bufferToString(options.newer).c_str());
    std::snprintf(data.anewer, sizeof(data.anewer), "%s", bufferToString(options.anewer).c_str());
    std::snprintf(data.cnewer, sizeof(data.cnewer), "%s", bufferToString(options.cnewer).c_str());
    std::snprintf(data.newermt, sizeof(data.newermt), "%s", bufferToString(options.newermt).c_str());
    std::snprintf(data.newerat, sizeof(data.newerat), "%s", bufferToString(options.newerat).c_str());
    std::snprintf(data.newerct, sizeof(data.newerct), "%s", bufferToString(options.newerct).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 78, 24), "Date & Time Filters");
    dialog->options |= ofCentered;

    auto *presetButtons = new TRadioButtons(TRect(3, 3, 30, 12),
                                            makeItemList({"Any ~t~ime",
                                                          "Past ~2~4 hours",
                                                          "Past ~7~ days",
                                                          "Past ~1~ month",
                                                          "Past ~6~ months",
                                                          "Past ~1~ year",
                                                          "Past ~6~ years",
                                                          "~C~ustom range"}));
    dialog->insert(presetButtons);
    presetButtons->setData(&data.preset);

    auto *fieldBoxes = new TCheckBoxes(TRect(32, 3, 58, 7),
                                       makeItemList({"Last ~m~odified",
                                                     "~C~reation time",
                                                     "Last ~a~ccess"}));
    dialog->insert(fieldBoxes);
    fieldBoxes->setData(&data.fields);

    auto *fromInput = new TInputLine(TRect(32, 7, 58, 8), sizeof(data.from) - 1);
    dialog->insert(new TLabel(TRect(32, 6, 56, 7), "~F~rom (YYYY-MM-DD):", fromInput));
    dialog->insert(fromInput);
    fromInput->setData(data.from);

    auto *toInput = new TInputLine(TRect(32, 9, 58, 10), sizeof(data.to) - 1);
    dialog->insert(new TLabel(TRect(32, 8, 56, 9), "~T~o (YYYY-MM-DD):", toInput));
    dialog->insert(toInput);
    toInput->setData(data.to);

    dialog->insert(new TStaticText(TRect(3, 12, 74, 14),
                                   "Fine-tune date rules. Enter +7 to match files at least 7 days old.\n"
                                   "Use -5 for changes within 5 days, or supply full timestamps."));

    auto *mtimeInput = new TInputLine(TRect(32, 14, 48, 15), sizeof(data.mtime) - 1);
    dialog->insert(new TLabel(TRect(3, 14, 32, 15), "~M~odified in past (days):", mtimeInput));
    dialog->insert(mtimeInput);
    mtimeInput->setData(data.mtime);

    auto *mminInput = new TInputLine(TRect(32, 15, 48, 16), sizeof(data.mmin) - 1);
    dialog->insert(new TLabel(TRect(3, 15, 32, 16), "Modified in past (m~i~nutes):", mminInput));
    dialog->insert(mminInput);
    mminInput->setData(data.mmin);

    auto *atimeInput = new TInputLine(TRect(32, 16, 48, 17), sizeof(data.atime) - 1);
    dialog->insert(new TLabel(TRect(3, 16, 32, 17), "~A~ccessed in past (days):", atimeInput));
    dialog->insert(atimeInput);
    atimeInput->setData(data.atime);

    auto *aminInput = new TInputLine(TRect(32, 17, 48, 18), sizeof(data.amin) - 1);
    dialog->insert(new TLabel(TRect(3, 17, 32, 18), "Accessed in past (m~i~nutes):", aminInput));
    dialog->insert(aminInput);
    aminInput->setData(data.amin);

    auto *ctimeInput = new TInputLine(TRect(32, 18, 48, 19), sizeof(data.ctime) - 1);
    dialog->insert(new TLabel(TRect(3, 18, 32, 19), "Meta~d~ata change in (days):", ctimeInput));
    dialog->insert(ctimeInput);
    ctimeInput->setData(data.ctime);

    auto *cminInput = new TInputLine(TRect(32, 19, 48, 20), sizeof(data.cmin) - 1);
    dialog->insert(new TLabel(TRect(3, 19, 32, 20), "Metadata change in (m~i~nutes):", cminInput));
    dialog->insert(cminInput);
    cminInput->setData(data.cmin);

    auto *usedInput = new TInputLine(TRect(32, 20, 48, 21), sizeof(data.used) - 1);
    dialog->insert(new TLabel(TRect(3, 20, 32, 21), "Last ~u~sed in past (days):", usedInput));
    dialog->insert(usedInput);
    usedInput->setData(data.used);

    auto pathLen = std::min<int>(static_cast<int>(sizeof(data.newer)) - 1, 255);
    auto *newerInput = new TInputLine(TRect(57, 14, 74, 15), pathLen);
    dialog->insert(new TLabel(TRect(34, 14, 57, 15), "Cutoff file (~m~odified):", newerInput));
    dialog->insert(newerInput);
    newerInput->setData(data.newer);

    auto *anewerInput = new TInputLine(TRect(57, 15, 74, 16), pathLen);
    dialog->insert(new TLabel(TRect(34, 15, 57, 16), "Cutoff file (~a~ccessed):", anewerInput));
    dialog->insert(anewerInput);
    anewerInput->setData(data.anewer);

    auto *cnewerInput = new TInputLine(TRect(57, 16, 74, 17), pathLen);
    dialog->insert(new TLabel(TRect(34, 16, 57, 17), "Cutoff file (meta~d~ata):", cnewerInput));
    dialog->insert(cnewerInput);
    cnewerInput->setData(data.cnewer);

    auto *newermtInput = new TInputLine(TRect(57, 17, 74, 18), sizeof(data.newermt) - 1);
    dialog->insert(new TLabel(TRect(34, 17, 57, 18), "Cutoff time (~m~odified):", newermtInput));
    dialog->insert(newermtInput);
    newermtInput->setData(data.newermt);

    auto *neweratInput = new TInputLine(TRect(57, 18, 74, 19), sizeof(data.newerat) - 1);
    dialog->insert(new TLabel(TRect(34, 18, 57, 19), "Cutoff time (~a~ccessed):", neweratInput));
    dialog->insert(neweratInput);
    neweratInput->setData(data.newerat);

    auto *newerctInput = new TInputLine(TRect(57, 19, 74, 20), sizeof(data.newerct) - 1);
    dialog->insert(new TLabel(TRect(34, 19, 57, 20), "Cutoff time (meta~d~ata):", newerctInput));
    dialog->insert(newerctInput);
    newerctInput->setData(data.newerct);

    dialog->insert(new TButton(TRect(30, 22, 40, 24), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 22, 52, 24), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        options.preset = static_cast<TimeFilterOptions::Preset>(data.preset);
        options.includeModified = (data.fields & 0x0001) != 0;
        options.includeCreated = (data.fields & 0x0002) != 0;
        options.includeAccessed = (data.fields & 0x0004) != 0;

        copyToArray(options.customFrom, data.from);
        copyToArray(options.customTo, data.to);

        copyToArray(options.mtime, data.mtime);
        options.useMTime = data.mtime[0] != '\0';
        copyToArray(options.mmin, data.mmin);
        options.useMMin = data.mmin[0] != '\0';
        copyToArray(options.atime, data.atime);
        options.useATime = data.atime[0] != '\0';
        copyToArray(options.amin, data.amin);
        options.useAMin = data.amin[0] != '\0';
        copyToArray(options.ctime, data.ctime);
        options.useCTime = data.ctime[0] != '\0';
        copyToArray(options.cmin, data.cmin);
        options.useCMin = data.cmin[0] != '\0';
        copyToArray(options.used, data.used);
        options.useUsed = data.used[0] != '\0';

        copyToArray(options.newer, data.newer);
        options.useNewer = data.newer[0] != '\0';
        copyToArray(options.anewer, data.anewer);
        options.useANewer = data.anewer[0] != '\0';
        copyToArray(options.cnewer, data.cnewer);
        options.useCNewer = data.cnewer[0] != '\0';
        copyToArray(options.newermt, data.newermt);
        options.useNewermt = data.newermt[0] != '\0';
        copyToArray(options.newerat, data.newerat);
        options.useNewerat = data.newerat[0] != '\0';
        copyToArray(options.newerct, data.newerct);
        options.useNewerct = data.newerct[0] != '\0';
    }

    return accepted;
}

} // namespace ck::find
