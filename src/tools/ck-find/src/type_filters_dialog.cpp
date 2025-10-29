#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"

#include <cstdio>
#include <cstring>
#include <string>

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

bool editTypeFilters(TypeFilterOptions &options)
{
    struct Data
    {
        unsigned short flags = 0;
        unsigned short typeFlags = 0;
        unsigned short xtypeFlags = 0;
        char extensions[256]{};
        char detectors[256]{};
    } data{};

    if (options.typeEnabled)
        data.flags |= 0x0001;
    if (options.xtypeEnabled)
        data.flags |= 0x0002;
    if (options.useExtensions)
        data.flags |= 0x0004;
    if (options.extensionCaseInsensitive)
        data.flags |= 0x0008;
    if (options.useDetectors)
        data.flags |= 0x0010;

    auto captureLetters = [](const std::string &letters, unsigned short &bits) {
        for (char ch : letters)
        {
            switch (ch)
            {
            case 'b':
                bits |= 0x0001;
                break;
            case 'c':
                bits |= 0x0002;
                break;
            case 'd':
                bits |= 0x0004;
                break;
            case 'p':
                bits |= 0x0008;
                break;
            case 'f':
                bits |= 0x0010;
                break;
            case 'l':
                bits |= 0x0020;
                break;
            case 's':
                bits |= 0x0040;
                break;
            case 'D':
                bits |= 0x0080;
                break;
            default:
                break;
            }
        }
    };

    captureLetters(bufferToString(options.typeLetters), data.typeFlags);
    captureLetters(bufferToString(options.xtypeLetters), data.xtypeFlags);

    std::snprintf(data.extensions, sizeof(data.extensions), "%s", bufferToString(options.extensions).c_str());
    std::snprintf(data.detectors, sizeof(data.detectors), "%s", bufferToString(options.detectorTags).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 74, 22), "Type Filters");
    dialog->options |= ofCentered;

    auto *flagBoxes = new TCheckBoxes(TRect(3, 3, 32, 12),
                                      makeItemList({"Enable -~t~ype",
                                                    "Enable -~x~type",
                                                    "Filter by ~e~xtension",
                                                    "Case-insensitive e~x~t",
                                                    "Use detector ~t~ags"}));
    dialog->insert(flagBoxes);
    flagBoxes->setData(&data.flags);

    auto *typeBoxes = new TCheckBoxes(TRect(34, 3, 50, 13),
                                      makeItemList({"Block (b)",
                                                    "Char (c)",
                                                    "Directory (d)",
                                                    "FIFO (p)",
                                                    "Regular (f)",
                                                    "Symlink (l)",
                                                    "Socket (s)",
                                                    "Door (D)"}));
    dialog->insert(typeBoxes);
    typeBoxes->setData(&data.typeFlags);

    auto *xtypeBoxes = new TCheckBoxes(TRect(52, 3, 68, 13),
                                       makeItemList({"b",
                                                     "c",
                                                     "d",
                                                     "p",
                                                     "f",
                                                     "l",
                                                     "s",
                                                     "D"}));
    dialog->insert(xtypeBoxes);
    xtypeBoxes->setData(&data.xtypeFlags);

    auto *extensionInput = new TInputLine(TRect(3, 13, 70, 14), sizeof(data.extensions) - 1);
    dialog->insert(new TLabel(TRect(3, 12, 70, 13), "Extensions (comma-separated):", extensionInput));
    dialog->insert(extensionInput);
    extensionInput->setData(data.extensions);

    auto *detectorInput = new TInputLine(TRect(3, 16, 70, 17), sizeof(data.detectors) - 1);
    dialog->insert(new TLabel(TRect(3, 15, 70, 16), "Detector tags (space/comma):", detectorInput));
    dialog->insert(detectorInput);
    detectorInput->setData(data.detectors);

    dialog->insert(new TStaticText(TRect(3, 18, 70, 20),
                                   "Select letters to OR together. -xtype evaluates after symlinks are"
                                   " resolved."));

    dialog->insert(new TButton(TRect(28, 20, 38, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(40, 20, 50, 22), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        options.typeEnabled = (data.flags & 0x0001) != 0;
        options.xtypeEnabled = (data.flags & 0x0002) != 0;
        options.useExtensions = (data.flags & 0x0004) != 0;
        options.extensionCaseInsensitive = (data.flags & 0x0008) != 0;
        options.useDetectors = (data.flags & 0x0010) != 0;

        auto buildLetters = [](unsigned short bits) {
            std::string out;
            if (bits & 0x0001)
                out.push_back('b');
            if (bits & 0x0002)
                out.push_back('c');
            if (bits & 0x0004)
                out.push_back('d');
            if (bits & 0x0008)
                out.push_back('p');
            if (bits & 0x0010)
                out.push_back('f');
            if (bits & 0x0020)
                out.push_back('l');
            if (bits & 0x0040)
                out.push_back('s');
            if (bits & 0x0080)
                out.push_back('D');
            return out;
        };

        auto typeString = buildLetters(data.typeFlags);
        auto xtypeString = buildLetters(data.xtypeFlags);
        copyToArray(options.typeLetters, typeString.c_str());
        copyToArray(options.xtypeLetters, xtypeString.c_str());
        copyToArray(options.extensions, data.extensions);
        copyToArray(options.detectorTags, data.detectors);
    }

    return accepted;
}

} // namespace ck::find
