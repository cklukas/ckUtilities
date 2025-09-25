#ifndef TV_GRIDLAYOUT_HPP
#define TV_GRIDLAYOUT_HPP

// Single-header Turbo Vision grid layout with "table" (HTML-like) API.
// - Tracks: Auto (natural), Expanding(weight), Fixed(pixels)
// - Sugar API: define a Table matrix with colspan/rowspan like HTML
// - Short helpers: A() / E() / F(n)
// C++17+
//
// Usage (example):
//   using namespace gridlayout;
//   GridLayout g;
//   g.cols = { E(), F(30) };              // 2 columns: expanding, fixed 30
//   g.rows = { A(), E(), F(10) };         // 3 rows: auto, expanding, fixed 10
//   Table t = {
//       { TableCell{toolbar, .colspan=2} },
//       { TableCell{left}, TableCell{right, .rowspan=2} },
//       { TableCell{leftBottom} }
//   };
//   buildFromMatrix(g, t);
//   g.apply(parent);

#define Uses_MsgBox
#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TKeys
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_TProgram
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TWindow
#define Uses_TPalette
#define Uses_TView
#define Uses_TGroup
#define Uses_TPoint
#define Uses_TRect
#define Uses_TDrawBuffer
#define Uses_TScroller
#define Uses_TScrollBar
#define Uses_TEvent
#define Uses_TMenuBar
#define Uses_TButton

#include <tvision/tv.h>

#include <vector>
#include <algorithm>

namespace gridlayout
{

    // ---------- Core types ----------

    struct Insets
    {
        int l = 0, t = 0, r = 0, b = 0;
    };

    enum class TrackKind
    {
        Fixed,
        Expanding,
        Auto
    };
    enum class Axis
    {
        X,
        Y
    };

    struct Track
    {
        TrackKind kind;
        int value; // Fixed: size; Expanding: weight; Auto: ignored

        static Track Fixed(int n) { return {TrackKind::Fixed, n}; }
        static Track Expanding(int weight = 1) { return {TrackKind::Expanding, std::max(1, weight)}; }
        static Track Auto() { return {TrackKind::Auto, 0}; }
    };

    // Convenience constructors (readable code style)
    inline Track Fixed(int n) { return Track::Fixed(n); }
    inline Track Expanding(int w = 1) { return Track::Expanding(w); }
    inline Track Auto() { return Track::Auto(); }

    // Ultra-short aliases (optional)
    inline Track F(int n) { return Fixed(n); }
    inline Track E(int w = 1) { return Expanding(w); }
    inline Track A() { return Auto(); }

    // "Natural size" policy for Auto tracks.
    // Default: use each view's minimum size from sizeLimits.
    struct NaturalSize
    {
        int operator()(TView &v, Axis axis) const
        {
            TPoint min{0, 0}, max{0, 0};
            v.sizeLimits(min, max);
            return axis == Axis::X ? min.x : min.y;
        }
    };

    // Low-level cell: ties a view to a grid slot (numeric indices).
    struct GridCell
    {
        TView *view{};
        int col{}, row{}, colSpan{1}, rowSpan{1};
    };

    // The core grid layout engine.
    class GridLayout
    {
    public:
        std::vector<Track> cols;
        std::vector<Track> rows;
        std::vector<GridCell> cells;
        Insets insets; // default {0,0,0,0}
        int gapX{0}, gapY{0};
        NaturalSize natural; // strategy for Auto tracks

        void apply(TGroup &parent) const
        {
            TRect pr = parent.getExtent();
            // Inset by margins so we don’t paint over window frame.
            pr.a.x += insets.l;
            pr.a.y += insets.t;
            pr.b.x -= insets.r;
            pr.b.y -= insets.b;

            const int totalW = pr.b.x - pr.a.x;
            const int totalH = pr.b.y - pr.a.y;

            auto colW = computeTracks(Axis::X, cols, totalW);
            auto rowH = computeTracks(Axis::Y, rows, totalH);

            for (const auto &c : cells)
            {
                if (!c.view)
                    continue;
                auto r = cellRect(pr, colW, rowH, c.col, c.row, c.colSpan, c.rowSpan);
                c.view->locate(r);
            }
        }

    private:
        // -- replace the whole method with this --
        std::vector<int> computeTracks(Axis axis,
                                       const std::vector<Track> &tracks,
                                       int totalSpan) const
        {
            const int gap = (axis == Axis::X ? gapX : gapY);
            const int gaps = std::max(0, int(tracks.size()) - 1) * gap;
            int space = std::max(0, totalSpan - gaps);

            std::vector<int> size(tracks.size(), 0);
            std::vector<int> minAuto(tracks.size(), 0);

            // 1) Sum Expanding weights
            int starWeight = 0;
            for (const auto &t : tracks)
                if (t.kind == TrackKind::Expanding)
                    starWeight += t.value;

            // 2) Seed Fixed sizes
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].kind == TrackKind::Fixed)
                    size[i] = tracks[i].value;

            // 3) Probe Auto from cells (natural size policy)
            for (const auto &gc : cells)
            {
                const int idx = (axis == Axis::X ? gc.col : gc.row);
                if (idx < 0 || idx >= (int)tracks.size())
                    continue;
                if (tracks[idx].kind == TrackKind::Auto && gc.view)
                {
                    int nat = natural(*gc.view, axis);
                    size[idx] = std::max(size[idx], nat);
                    minAuto[idx] = std::max(minAuto[idx], nat);
                }
            }

            // 4) Sum non-expanding tracks exactly once (Fixed + Auto)
            int sumFixedAuto = 0;
            for (size_t i = 0; i < tracks.size(); ++i)
                if (tracks[i].kind != TrackKind::Expanding)
                    sumFixedAuto += size[i];

            // 5) Distribute leftover to Expanding, or shrink on deficit
            int leftover = space - sumFixedAuto;
            if (leftover >= 0 && starWeight > 0)
            {
                for (size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i].kind == TrackKind::Expanding)
                        size[i] = (leftover * tracks[i].value) / starWeight;
            }
            else if (leftover < 0)
            {
                int deficit = -leftover;

                // a) Shrink Expanding first (down to 0)
                int starTotal = 0;
                for (size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i].kind == TrackKind::Expanding)
                        starTotal += size[i];

                int take = std::min(deficit, starTotal);
                if (take > 0 && starTotal > 0)
                {
                    for (size_t i = 0; i < tracks.size(); ++i)
                        if (tracks[i].kind == TrackKind::Expanding)
                        {
                            int delta = (take * size[i]) / starTotal;
                            size[i] -= delta;
                        }
                    deficit -= take;
                }

                // b) Then shrink Auto down to minAuto (Fixed remains fixed)
                if (deficit > 0)
                {
                    int autoSlack = 0;
                    for (size_t i = 0; i < tracks.size(); ++i)
                        if (tracks[i].kind == TrackKind::Auto)
                            autoSlack += std::max(0, size[i] - minAuto[i]);

                    int take2 = std::min(deficit, autoSlack);
                    if (take2 > 0 && autoSlack > 0)
                    {
                        for (size_t i = 0; i < tracks.size(); ++i)
                            if (tracks[i].kind == TrackKind::Auto)
                            {
                                int slack = std::max(0, size[i] - minAuto[i]);
                                int delta = (take2 * slack) / autoSlack;
                                size[i] -= delta;
                            }
                        deficit -= take2;
                    }
                }
                // If still > 0: over-constrained; clipping may occur.
            }

            for (auto &x : size)
                x = std::max(0, x);
            return size;
        }

        // Helper to sum a slice [from, to)
        static int sumOf(const std::vector<int> &v, int from, int to)
        {
            int s = 0;
            for (int i = from; i < to && i < (int)v.size(); ++i)
                s += v[i];
            return s;
        }

    public:
        // Public helper to get a gap-aware rect (uses current gapX/gapY).
        TRect cellRect(const TRect &pr,
                       const std::vector<int> &cw,
                       const std::vector<int> &rh,
                       int c, int r, int cs, int rs) const
        {
            // Offsets with gaps
            int x0 = pr.a.x + sumOf(cw, 0, c) + c * gapX;
            int y0 = pr.a.y + sumOf(rh, 0, r) + r * gapY;
            int w = sumOf(cw, c, c + cs) + std::max(0, cs - 1) * gapX;
            int h = sumOf(rh, r, r + rs) + std::max(0, rs - 1) * gapY;
            return TRect{x0, y0, x0 + w, y0 + h};
        }
    };

    // ---------- Table (HTML-like) sugar API ----------

    // A matrix cell: like <td>, with optional colspan/rowspan.
    struct TableCell
    {
        TView *view = nullptr;
        int colspan = 1;
        int rowspan = 1;
        // (future: align, padding, etc.)
    };

    using Table = std::vector<std::vector<TableCell>>;

    // Build GridLayout::cells from a Table matrix.
    // Assumes GridLayout::cols/rows are already set.
    // Rows in 'table' map to row indices; columns map to column indices.
    // Spanned areas mark their covered cells implicitly (no need to place empties).
    inline void buildFromMatrix(GridLayout &g, const Table &table)
    {
        const int nRows = (int)table.size();
        const int nCols = (int)g.cols.size(); // authoritative column count

        if (nRows == 0 || nCols == 0)
            return;

        // Occupancy to skip cells covered by a previous span.
        std::vector<std::vector<bool>> covered(nRows, std::vector<bool>(nCols, false));

        for (int r = 0; r < nRows; ++r)
        {
            const auto &row = table[r];
            // We only inspect up to nCols logical columns.
            int logicalC = 0;
            for (int c = 0; c < (int)row.size() && logicalC < nCols; ++c)
            {
                // Find next free logical column if current is covered.
                while (logicalC < nCols && covered[r][logicalC])
                    ++logicalC;
                if (logicalC >= nCols)
                    break;

                const TableCell &tc = row[c];

                int cs = std::max(1, tc.colspan);
                int rs = std::max(1, tc.rowspan);

                // Clamp spans to grid bounds.
                if (logicalC + cs > nCols)
                    cs = nCols - logicalC;
                if (r + rs > nRows)
                    rs = nRows - r;

                // Mark covered area.
                for (int rr = r; rr < r + rs; ++rr)
                    for (int cc = logicalC; cc < logicalC + cs; ++cc)
                        if (!(rr == r && cc == logicalC))
                            covered[rr][cc] = true;

                if (tc.view)
                    g.cells.push_back(GridCell{tc.view, logicalC, r, cs, rs});

                // Advance to the next logical slot after this cell.
                logicalC += cs;
            }
        }
    }

} // namespace gridlayout

#endif // TV_GRIDLAYOUT_HPP
