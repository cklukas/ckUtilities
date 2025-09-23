#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"

#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TDialog
#define Uses_TProgram
#define Uses_TRadioButtons
#define Uses_TStaticText
#include <tvision/tv.h>

namespace ck::find
{

bool editTextOptions(TextSearchOptions &options)
{
    struct Data
    {
        unsigned short mode = 0;
        unsigned short flags = 0;
    } data{};
    data.mode = static_cast<unsigned short>(options.mode);
    if (options.matchCase)
        data.flags |= 0x0001;
    if (options.searchInContents)
        data.flags |= 0x0002;
    if (options.searchInFileNames)
        data.flags |= 0x0004;
    if (options.allowMultipleTerms)
        data.flags |= 0x0008;
    if (options.treatBinaryAsText)
        data.flags |= 0x0010;

    auto *dialog = new TDialog(TRect(0, 0, 60, 16), "Text Options");
    dialog->options |= ofCentered;

    auto *modeButtons = new TRadioButtons(TRect(3, 3, 30, 8),
                                          makeItemList({"Contains te~x~t",
                                                        "Match ~w~hole word",
                                                        "Regular ~e~xpression"}));
    dialog->insert(modeButtons);
    modeButtons->setData(&data.mode);

    auto *optionBoxes = new TCheckBoxes(TRect(32, 3, 58, 9),
                                        makeItemList({"~M~atch case",
                                                      "Search file ~c~ontents",
                                                      "Search file ~n~ames",
                                                      "Allow ~m~ultiple terms",
                                                      "Treat ~b~inary as text"}));
    dialog->insert(optionBoxes);
    optionBoxes->setData(&data.flags);

    dialog->insert(new TStaticText(TRect(3, 9, 58, 12),
                                   "Use regular expressions when you need complex\n"
                                   "pattern matching. Whole-word mode respects\n"
                                   "word boundaries."));

    dialog->insert(new TButton(TRect(16, 12, 26, 14), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(28, 12, 38, 14), "Cancel", cmCancel, bfNormal));

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        modeButtons->getData(&data.mode);
        optionBoxes->getData(&data.flags);
        options.mode = static_cast<TextSearchOptions::Mode>(data.mode);
        options.matchCase = (data.flags & 0x0001) != 0;
        options.searchInContents = (data.flags & 0x0002) != 0;
        options.searchInFileNames = (data.flags & 0x0004) != 0;
        options.allowMultipleTerms = (data.flags & 0x0008) != 0;
        options.treatBinaryAsText = (data.flags & 0x0010) != 0;
    }
    TObject::destroy(dialog);
    return accepted;
}

} // namespace ck::find

