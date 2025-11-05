#include "ck/ui/tab_control.hpp"

#include "ck/commands/common.hpp"

#include <algorithm>

namespace ck::ui
{

namespace
{

TRect pageBoundsFor(const TRect &bounds, unsigned short tabHeight)
{
    TRect pageBounds = bounds;
    pageBounds.a.y += tabHeight;
    return pageBounds;
}

} // namespace

TabPageView::TabPageView(const TRect &bounds) noexcept
    : TGroup(bounds)
{
    growMode = gfGrowHiX | gfGrowHiY;
}

void TabPageView::onActivated()
{
}

void TabPageView::onDeactivated()
{
}

void TabPageView::draw()
{
    const TRect extent = getExtent();
    const int width = extent.b.x - extent.a.x;
    const int height = extent.b.y - extent.a.y;

    TDrawBuffer buffer;
    const ushort color = getColor(1);
    for (int row = 0; row < height; ++row)
    {
        buffer.moveChar(0, ' ', color, width);
        writeLine(0, row, width, 1, buffer);
    }

    TGroup::draw();
}

TabControl::TabControl(const TRect &bounds, unsigned short tabHeight) noexcept
    : TGroup(bounds),
      m_tabHeight(std::max<unsigned short>(1, tabHeight))
{
    growMode = gfGrowHiX | gfGrowHiY;
    options |= ofSelectable;
}

TabControl::~TabControl() = default;

void TabControl::addTab(const std::string &title, TabPageView *page, unsigned short command)
{
    if (!page)
        return;

    layoutPage(*page);
    insert(page);
    page->hide();

    if (!command)
    {
        // Caller can bind specific commands later; zero means no direct command.
    }

    m_tabs.push_back(Tab{title, page, command});

    if (m_tabs.size() == 1)
        selectTab(0);
}

TabPageView *TabControl::createTab(const std::string &title, unsigned short command)
{
    TRect bounds = pageBoundsFor(getExtent(), m_tabHeight);
    auto *page = new TabPageView(bounds);
    addTab(title, page, command);
    return page;
}

void TabControl::selectTab(std::size_t index)
{
    if (index >= m_tabs.size())
        return;

    if (!m_tabs.empty() && m_current < m_tabs.size())
    {
        Tab &currentTab = m_tabs[m_current];
        if (currentTab.page)
        {
            currentTab.page->onDeactivated();
            currentTab.page->hide();
        }
    }

    m_current = index;

    Tab &nextTab = m_tabs[m_current];
    if (nextTab.page)
    {
        layoutPage(*nextTab.page);
        nextTab.page->show();
        nextTab.page->onActivated();
        setCurrent(nextTab.page, enterSelect);
    }
    drawView();
}

std::size_t TabControl::currentIndex() const noexcept
{
    return m_current;
}

std::size_t TabControl::tabCount() const noexcept
{
    return m_tabs.size();
}

void TabControl::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        if (event.message.command == ck::commands::common::TabNext)
        {
            selectNext();
            clearEvent(event);
            return;
        }
        if (event.message.command == ck::commands::common::TabPrevious)
        {
            selectPrevious();
            clearEvent(event);
            return;
        }
        if (selectByCommand(event.message.command))
        {
            clearEvent(event);
            return;
        }
    }
    else if (event.what == evKeyDown)
    {
        if (event.keyDown.keyCode == kbCtrlTab)
        {
            selectNext();
            clearEvent(event);
            return;
        }
        if (event.keyDown.keyCode == (kbCtrlShift | kbTab))
        {
            selectPrevious();
            clearEvent(event);
            return;
        }
    }

    TGroup::handleEvent(event);
}

void TabControl::draw()
{
    const TRect extent = getExtent();
    const int width = extent.b.x - extent.a.x;

    TDrawBuffer buffer;
    const ushort baseColor = getColor(1);
    const ushort highlightColor = getColor(2);

    for (unsigned short row = 0; row < m_tabHeight; ++row)
    {
        buffer.moveChar(0, ' ', baseColor, width);
        if (row == 0)
        {
            int x = 1;
            for (std::size_t i = 0; i < m_tabs.size(); ++i)
            {
                const std::string &title = m_tabs[i].title;
                const bool active = (i == m_current);
                const ushort color = active ? highlightColor : baseColor;
                if (x >= width - 1)
                    break;

                std::string label;
                if (active)
                {
                    label.push_back('[');
                    label += title;
                    label.push_back(']');
                }
                else
                {
                    label.push_back(' ');
                    label += title;
                    label.push_back(' ');
                }
                label.push_back(' ');

                const int room = width - x - 1;
                if (room <= 0)
                    break;

                buffer.moveStr(x, label.c_str(), color, room);
                const int drawn = std::min<int>(static_cast<int>(label.size()), room);
                x += drawn;
            }
        }
        writeLine(0, row, width, 1, buffer);
    }

    // Draw separator line beneath tabs.
    if (m_tabHeight > 0)
    {
        buffer.moveChar(0, 0xcd, baseColor, width);
        writeLine(0, m_tabHeight - 1, width, 1, buffer);
    }

    TGroup::draw();
}

void TabControl::changeBounds(const TRect &bounds)
{
    TGroup::changeBounds(bounds);
    updatePagesBounds();
}

void TabControl::shutDown()
{
    for (auto &tab : m_tabs)
    {
        if (tab.page)
            tab.page = nullptr;
    }
    m_tabs.clear();
    TGroup::shutDown();
}

void TabControl::layoutPage(TabPageView &page)
{
    TRect area = pageBoundsFor(getExtent(), m_tabHeight);
    page.locate(area);
}

void TabControl::updatePagesBounds()
{
    for (auto &tab : m_tabs)
    {
        if (tab.page)
            layoutPage(*tab.page);
    }
}

bool TabControl::selectByCommand(unsigned short command)
{
    if (!command)
        return false;

    for (std::size_t i = 0; i < m_tabs.size(); ++i)
    {
        if (m_tabs[i].command == command)
        {
            selectTab(i);
            return true;
        }
    }
    return false;
}

void TabControl::selectNext()
{
    if (m_tabs.empty())
        return;
    const std::size_t nextIndex = (m_current + 1) % m_tabs.size();
    selectTab(nextIndex);
}

void TabControl::selectPrevious()
{
    if (m_tabs.empty())
        return;
    const std::size_t nextIndex = (m_current == 0) ? (m_tabs.size() - 1) : (m_current - 1);
    selectTab(nextIndex);
}

void TabControl::nextTab()
{
    selectNext();
}

void TabControl::previousTab()
{
    selectPrevious();
}

} // namespace ck::ui
