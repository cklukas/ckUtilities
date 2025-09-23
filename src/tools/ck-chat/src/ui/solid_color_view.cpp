#include "solid_color_view.hpp"

SolidColorView::SolidColorView(const TRect &bounds)
    : TView(bounds)
{
    options &= ~ofSelectable;
}

void SolidColorView::draw()
{
    // 1: blue, 2: green, 3: cyan, 4: red, 5: magenta, 6: brown, 7: light gray
    TColorAttr attr{TColorBIOS(0x1), TColorBIOS(0x7)};
    TDrawBuffer buffer;
    for (int y = 0; y < size.y; ++y)
    {
        buffer.moveChar(0, ' ', attr, size.x);
        writeLine(0, y, size.x, 1, buffer);
    }
}