#define Uses_TView
#define Uses_TWindow
#define Uses_TApplication
#define Uses_TMenuBar
#define Uses_TMenu
#define Uses_TMenuItem
#define Uses_TStatusLine
#define Uses_TStatusDef
#define Uses_TEvent
#define Uses_TRect

#include <tvision/tv.h>
#include <tvision/colors.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gridlayout.hpp"

using namespace gridlayout;

class ColorPane : public TView
{
public:
    ushort attr;
    const char *name;

    ColorPane(ushort a, const char *n, bool framed)
        : TView(TRect(0, 0, 1, 1)), attr(a), name(n)
    {
        if (framed)
            options |= ofFramed;
        growMode = gfGrowHiX | gfGrowHiY;
    }

    void draw() override
    {
        // Draw the frame only if present; then paint the client area.
        if (options & ofFramed)
            TView::draw(); // draws the thin frame and clears the view

        TRect client = getExtent();
        if (options & ofFramed) client.grow(-1, -1);
        if (client.isEmpty()) return;

        TDrawBuffer b;
        b.moveChar(0, ' ', attr, client.b.x - client.a.x);

        for (int y = client.a.y; y < client.b.y; ++y)
            writeLine(client.a.x, y, client.b.x - client.a.x, 1, b);

        // And draw the name.
        if (client.b.x - client.a.x > 2 && client.b.y - client.a.y > 0)
        {
            b.moveStr(1, name, attr);
            writeLine(client.a.x, client.a.y, client.b.x - client.a.x, 1, b);
        }
    }
};

class ColorGridView : public TView
{
public:
    ColorGridView(const TRect &r) : TView(r)
    {
        options |= ofFramed;
        growMode = gfGrowHiX | gfGrowHiY;
    }

    void sizeLimits(TPoint &min, TPoint &max) override
    {
        min = TPoint(17 * cellW + nameW + 2, 18 * cellH + 2);
        max = TPoint(0x7fff, 0x7fff);
    }

    void draw() override
    {
        // Let the base class draw the frame and a default background.
        TView::draw();

        // Get the client area to draw our content in.
        TRect client = getExtent();
        client.grow(-1, -1);
        if (client.isEmpty())
            return;

        TDrawBuffer b;
        TColorAttr base{TColorDesired{}, TColorDesired{}};

        // The drawing logic now needs to be offset by the client area's top-left corner,
        // and clipped by the client area's size.
        TPoint clientSize = client.b - client.a;

        for (int r = 0; r <= 17; ++r)
        {
            const int y0 = r * cellH;
            if (y0 >= clientSize.y)
                break;

            for (int c = 0; c <= 17; ++c)
            {
                const int w = (c == 17 ? nameW : cellW);
                const int x0 = (c == 17 ? 17 * cellW : c * cellW);
                if (x0 >= clientSize.x)
                    break;

                TColorAttr attr = base;
                char text[24] = {0};

                if (r == 0 && c == 0)
                {
                    attr = base;
                }
                else if (r == 0 && c <= 16)
                {
                    attr = fgHeader;
                    std::snprintf(text, sizeof(text), "FG%02d", c - 1);
                }
                else if (c == 0 && r <= 16)
                {
                    attr = bgHeader;
                    std::snprintf(text, sizeof(text), "BG%02d", r - 1);
                }
                else if (r >= 1 && r <= 16 && c >= 1 && c <= 16)
                {
                    const int bg = r - 1;
                    const int fg = c - 1;
                    attr = TColorAttr{TColorBIOS(fg), TColorBIOS(bg)};
                    const bool good = isReadablePair(fg, bg);
                    std::snprintf(text, sizeof(text), "%02d%c%02d", bg, (good ? '#' : '-'), fg);
                }
                else if (c == 17 && r == 0)
                {
                    attr = namesHdr;
                    std::snprintf(text, sizeof(text), "BG Names");
                }
                else if (r == 17 && c == 0)
                {
                    attr = namesHdr;
                    std::snprintf(text, sizeof(text), "FG Names");
                }
                else if (c == 17 && r >= 1 && r <= 16)
                {
                    const int bg = r - 1;
                    const int fg = legibleFg(bg);
                    attr = TColorAttr{TColorBIOS(fg), TColorBIOS(bg)};
                    std::snprintf(text, sizeof(text), "%s", colorName(bg));
                }
                else if (r == 17 && c >= 1 && c <= 16)
                {
                    const int fg = c - 1;
                    attr = TColorAttr{TColorBIOS(fg), TColorBIOS(7)};
                    std::snprintf(text, sizeof(text), "%s", colorName(fg));
                }
                else if (r == 17 && c == 17)
                {
                    attr = namesHdr;
                    std::snprintf(text, sizeof(text), "Names");
                }

                for (int ly = 0; ly < cellH; ++ly)
                {
                    const int yy = y0 + ly;
                    if (yy >= clientSize.y)
                        break;
                    const int ww = std::min(w, clientSize.x - x0);
                    if (ww <= 0)
                        break;

                    b.moveChar(0, ' ', attr, ww);
                    if (text[0])
                        b.moveStr(0, text, attr);
                    writeLine(client.a.x + x0, client.a.y + yy, ww, 1, b);
                }
            }
        }
    }

private:
    static constexpr int cellW = 5, nameW = 12, cellH = 1;
    const TColorAttr fgHeader{TColorBIOS(0), TColorBIOS(7)};
    const TColorAttr bgHeader{TColorBIOS(15), TColorBIOS(1)};
    const TColorAttr namesHdr{TColorBIOS(0), TColorBIOS(7)};
    static float luminance(int bios)
    {
        static const float L[16] = {0.00f, 0.07f, 0.15f, 0.22f, 0.15f, 0.22f, 0.18f, 0.60f, 0.35f, 0.50f, 0.70f, 0.80f, 0.70f, 0.80f, 0.90f, 1.00f};
        return L[((bios % 16) + 16) % 16];
    }
    static float contrastRatio(int fg, int bg)
    {
        float L1 = luminance(fg), L2 = luminance(bg);
        if (L1 < L2)
            std::swap(L1, L2);
        return (L1 + 0.05f) / (L2 + 0.05f);
    }
    static bool isRed(int c)
    {
        c = (c & 0xF);
        return c == 4 || c == 12;
    }
    static bool isGreenish(int c)
    {
        c = (c & 0xF);
        return c == 2 || c == 10 || c == 6 || c == 14;
    }
    static bool isReadablePair(int fg, int bg)
    {
        fg &= 0xF;
        bg &= 0xF;
        if (fg == bg)
            return false;
        const float cr = contrastRatio(fg, bg);
        if ((isRed(fg) && isGreenish(bg)) || (isRed(bg) && isGreenish(fg)))
            return cr >= 7.0f;
        return cr >= 4.5f;
    }
    static int legibleFg(int bg) { return luminance(bg) >= 0.5f ? 0 : 15; }
    static const char *colorName(int c)
    {
        static const char *names[16] = {"Black", "Blue", "Green", "Cyan", "Red", "Magenta", "Brown", "LightGray", "DarkGray", "LightBlue", "LightGreen", "LightCyan", "LightRed", "LightMagenta", "Yellow", "White"};
        return names[((c % 16) + 16) % 16];
    }
};

class DemoWindow : public TWindow
{
public:
    TView *toolbar{}, *leftPane{}, *right{}, *status{};
    const bool framed_;

    DemoWindow(const TRect &r, bool framed)
        : TWindow(r, "GridLayout Demo", wnNoNumber),
          TWindowInit(&TWindow::initFrame),
          framed_(framed)
    {
        flags |= wfGrow | wfMove | wfClose | wfZoom;
        toolbar = new ColorPane(0x2E, "toolbar", framed_);
        right = new ColorPane(0x4E, "right", framed_);
        status = new ColorPane(0x2E, "status", framed_);
        leftPane = new ColorGridView(TRect(0, 0, 1, 1));

        this->insert(toolbar);
        this->insert(leftPane);
        this->insert(right);
        this->insert(status);

        relayout();
    }

    void changeBounds(const TRect &b) override
    {
        TWindow::changeBounds(b);
        relayout();
    }

private:
    void relayout()
    {
        GridLayout g;
        g.cols = {E(), F(30)};
        g.rows = {F(3), E(), F(framed_ ? 3 : 1)};

        if (framed_)
        {
            g.insets = {2, 2, 2, 2};
            g.gapX = 1;
            g.gapY = 1;
        }
        else
        {
            g.insets = {1, 1, 1, 1};
        }

        Table t = {
            {TableCell{toolbar, /*colspan*/ 2}},
            {TableCell{leftPane}, TableCell{right}},
            {TableCell{status, /*colspan*/ 2}}};

        buildFromMatrix(g, t);
        g.apply(*this);
    }
};

class DemoApp : public TApplication
{
public:
    DemoApp() : TProgInit(&DemoApp::initStatusLine, &DemoApp::initMenuBar, &DemoApp::initDeskTop) {}

    static TMenuBar *initMenuBar(TRect r)
    {
        r.b.y = r.a.y + 1;
        auto *menu = new TMenu(*new TMenuItem("~Q~uit", cmQuit, kbAltX, hcNoContext, 0));
        return new TMenuBar(r, menu);
    }

    static TStatusLine *initStatusLine(TRect r)
    {
        r.a.y = r.b.y - 1;
        return new TStatusLine(r, *new TStatusDef(0, 0, nullptr));
    }

    void run() override
    {
        TRect r = deskTop->getExtent();
        auto *win1 = new DemoWindow(r, true);
        deskTop->insert(win1);
        auto *win2 = new DemoWindow(r, false);
        deskTop->insert(win2);
        TApplication::run();
    }
};

int main()
{
    DemoApp app;
    app.run();
    return 0;
}
