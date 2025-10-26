#pragma once

#define Uses_TStatusLine
#include <tvision/tv.h>

#include <string>

namespace ck::ui
{

class CommandAwareStatusLine : public TStatusLine
{
public:
    using TStatusLine::TStatusLine;

    const char *hint(ushort helpCtx) override;

private:
    std::string hintBuffer_;
};

} // namespace ck::ui

