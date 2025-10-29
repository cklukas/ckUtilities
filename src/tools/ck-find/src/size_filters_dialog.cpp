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
#define Uses_TStaticText
#include <tvision/tv.h>

namespace ck::find
{

bool editSizeFilters(SizeFilterOptions &options)
{
    struct Data
    {
        unsigned short flags = 0;
        char minSpec[32]{};
        char maxSpec[32]{};
        char exactSpec[32]{};
    } data{};

    if (options.minEnabled)
        data.flags |= 0x0001;
    if (options.maxEnabled)
        data.flags |= 0x0002;
    if (options.exactEnabled)
        data.flags |= 0x0004;
    if (options.rangeInclusive)
        data.flags |= 0x0008;
    if (options.includeZeroByte)
        data.flags |= 0x0010;
    if (options.treatDirectoriesAsFiles)
        data.flags |= 0x0020;
    if (options.useDecimalUnits)
        data.flags |= 0x0040;
    if (options.emptyEnabled)
        data.flags |= 0x0080;

    std::snprintf(data.minSpec, sizeof(data.minSpec), "%s", bufferToString(options.minSpec).c_str());
    std::snprintf(data.maxSpec, sizeof(data.maxSpec), "%s", bufferToString(options.maxSpec).c_str());
    std::snprintf(data.exactSpec, sizeof(data.exactSpec), "%s", bufferToString(options.exactSpec).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 72, 20), "Size Filters");
    dialog->options |= ofCentered;

    auto *flagBoxes = new TCheckBoxes(TRect(3, 3, 34, 12),
                                      makeItemList({"~M~inimum size",
                                                    "Ma~x~imum size",
                                                    "Exact -~s~ize expression",
                                                    "R~a~nge inclusive",
                                                    "Include ~0~-byte entries",
                                                    "Treat directorie~s~ as files",
                                                    "Use ~d~ecimal units",
                                                    "Match ~e~mpty entries"}));
    dialog->insert(flagBoxes);
    flagBoxes->setData(&data.flags);

    auto *minInput = new TInputLine(TRect(36, 4, 68, 5), sizeof(data.minSpec) - 1);
    dialog->insert(new TLabel(TRect(36, 3, 68, 4), "-size lower bound:", minInput));
    dialog->insert(minInput);
    minInput->setData(data.minSpec);

    auto *maxInput = new TInputLine(TRect(36, 7, 68, 8), sizeof(data.maxSpec) - 1);
    dialog->insert(new TLabel(TRect(36, 6, 68, 7), "-size upper bound:", maxInput));
    dialog->insert(maxInput);
    maxInput->setData(data.maxSpec);

    auto *exactInput = new TInputLine(TRect(36, 10, 68, 11), sizeof(data.exactSpec) - 1);
    dialog->insert(new TLabel(TRect(36, 9, 68, 10), "Exact -size expression:", exactInput));
    dialog->insert(exactInput);
    exactInput->setData(data.exactSpec);

    dialog->insert(new TStaticText(TRect(3, 12, 68, 14),
                                   "Use find syntax such as +10M, -512k, or 100c. Leave values blank\n"
                                   "to disable those tests."));

    dialog->insert(new TButton(TRect(24, 15, 34, 17), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(36, 15, 46, 17), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        options.minEnabled = (data.flags & 0x0001) != 0;
        options.maxEnabled = (data.flags & 0x0002) != 0;
        options.exactEnabled = (data.flags & 0x0004) != 0;
        options.rangeInclusive = (data.flags & 0x0008) != 0;
        options.includeZeroByte = (data.flags & 0x0010) != 0;
        options.treatDirectoriesAsFiles = (data.flags & 0x0020) != 0;
        options.useDecimalUnits = (data.flags & 0x0040) != 0;
        options.emptyEnabled = (data.flags & 0x0080) != 0;

        copyToArray(options.minSpec, data.minSpec);
        copyToArray(options.maxSpec, data.maxSpec);
        copyToArray(options.exactSpec, data.exactSpec);
    }

    return accepted;
}

} // namespace ck::find
