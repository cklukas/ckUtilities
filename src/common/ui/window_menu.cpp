#define Uses_TWindow
#define Uses_TView
#include "ck/ui/window_menu.hpp"

namespace ck::ui
{
    namespace
    {
        struct OtherWindowContext
        {
            TView *current = nullptr;
        };

        Boolean findOtherWindow(TView *view, void *data)
        {
            auto *context = static_cast<OtherWindowContext *>(data);
            if (!view)
                return False;
            if (view == context->current)
                return False;
            if ((view->options & ofSelectable) == 0)
                return False;
            if ((view->state & (sfVisible | sfDisabled)) != sfVisible)
                return False;
            return Boolean(dynamic_cast<TWindow *>(view) != nullptr);
        }

        Boolean findTileableWindow(TView *view, void *)
        {
            if (!view)
                return False;
            if ((view->options & ofTileable) == 0)
                return False;
            if ((view->state & sfVisible) == 0)
                return False;
            return Boolean(dynamic_cast<TWindow *>(view) != nullptr);
        }

        TWindow *asWindow(TView *view)
        {
            return dynamic_cast<TWindow *>(view);
        }
    } // namespace

    void WindowMenuController::update(TApplication &application, TDeskTop *deskTop)
    {
        CommandState state{};

        TWindow *currentWindow = nullptr;
        if (deskTop)
        {
            currentWindow = asWindow(deskTop->current);
            if (!currentWindow && deskTop->current)
                currentWindow = asWindow(deskTop->current->owner);
        }

        if (currentWindow)
        {
            state.canClose = (currentWindow->flags & wfClose) != 0;
            state.canResize = (currentWindow->flags & (wfGrow | wfMove)) != 0;
            state.canZoom = (currentWindow->flags & wfZoom) != 0;
        }

        if (deskTop)
        {
            OtherWindowContext context{deskTop->current};
            state.hasNext = deskTop->firstThat(findOtherWindow, &context) != nullptr;
            bool hasTileable = deskTop->firstThat(findTileableWindow, nullptr) != nullptr;
            state.canTile = hasTileable;
            state.canCascade = hasTileable;
        }

        if (!lastState_ || *lastState_ != state)
        {
            applyState(state, application);
            lastState_ = state;
        }
    }

    void WindowMenuController::applyState(const CommandState &state, TApplication &application)
    {
        if (state.canClose)
            application.enableCommand(cmClose);
        else
            application.disableCommand(cmClose);

        if (state.canResize)
            application.enableCommand(cmResize);
        else
            application.disableCommand(cmResize);

        if (state.canZoom)
            application.enableCommand(cmZoom);
        else
            application.disableCommand(cmZoom);

        if (state.hasNext)
            application.enableCommand(cmNext);
        else
            application.disableCommand(cmNext);

        if (state.canTile)
            application.enableCommand(cmTile);
        else
            application.disableCommand(cmTile);

        if (state.canCascade)
            application.enableCommand(cmCascade);
        else
            application.disableCommand(cmCascade);
    }

    TSubMenu &createWindowMenu()
    {
        auto *menu = new TSubMenu("~W~indow", hcNoContext);
        *menu + *new TMenuItem("~R~esize/Move", cmResize, kbNoKey, hcNoContext) +
            *new TMenuItem("~Z~oom", cmZoom, kbNoKey, hcNoContext) +
            *new TMenuItem("~N~ext", cmNext, kbNoKey, hcNoContext) +
            *new TMenuItem("~C~lose", cmClose, kbNoKey, hcNoContext) +
            *new TMenuItem("~T~ile", cmTile, kbNoKey, hcNoContext) +
            *new TMenuItem("C~a~scade", cmCascade, kbNoKey, hcNoContext);
        return *menu;
    }
} // namespace ck::ui
