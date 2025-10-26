#pragma once

#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TMenu
#define Uses_TMenuItem
#define Uses_TSubMenu
#define Uses_TKeys
#include <tvision/tv.h>

#include <optional>

namespace ck::ui
{
    class WindowMenuController
    {
    public:
        void update(TApplication &application, TDeskTop *deskTop);

    private:
        struct CommandState
        {
            bool canClose = false;
            bool canResize = false;
            bool canZoom = false;
            bool hasNext = false;
            bool canTile = false;
            bool canCascade = false;

            bool operator==(const CommandState &) const = default;
        };

        void applyState(const CommandState &state, TApplication &application);

        std::optional<CommandState> lastState_;
    };

    TSubMenu &createWindowMenu();
}
