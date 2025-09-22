To avoid event handling issues:

* Event ownership: Handle app-level commands in LauncherApp::handleEvent first; clear and return. Do not let them propagate to views.
* No self-reposting: When forwarding a command from a view/dialog, set infoPtr and only repost if infoPtr != this.
* Always clear and stop: After consuming a command, clearEvent(event) and return to prevent re-dispatch.
* Logging over modals: Prefer non-blocking logs; only show modal message boxes when explicitly debugging.
* One-launch path: Centralize launch flow (suspend → exec → resume → redraw) in a single function; keep views dumb.
* Keep platform scope tight: Avoid premature Windows branches; add them later with tests.
* Checklist for reviews: Verify command origin checks (infoPtr), event clearing/returns, no repost loops, and launch flow encapsulation.
* Document the pattern: Add a brief comment block near handlers describing this contract so it’s hard to regress.