#include "ck/ui/status_line.hpp"

#include "ck/hotkeys.hpp"

namespace ck::ui
{

const char *CommandAwareStatusLine::hint(ushort helpCtx)
{
    if (helpCtx != hcNoContext)
    {
        std::string helpText = ck::hotkeys::commandHelp(helpCtx);
        if (!helpText.empty())
        {
            hintBuffer_ = std::move(helpText);
            return hintBuffer_.c_str();
        }
    }
    return TStatusLine::hint(helpCtx);
}

} // namespace ck::ui

