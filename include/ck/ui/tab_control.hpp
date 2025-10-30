#pragma once

#include <cstddef>
#include <string>
#include <vector>

#define Uses_TGroup
#define Uses_TPoint
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TRect
#define Uses_TKeys
#include <tvision/tv.h>

namespace ck::ui
{

class TabPageView : public TGroup
{
public:
    TabPageView(const TRect &bounds) noexcept;

    virtual void onActivated();
    virtual void onDeactivated();
};

class TabControl : public TGroup
{
public:
    struct Tab
    {
        std::string title;
        TabPageView *page = nullptr;
        unsigned short command = 0;
    };

    TabControl(const TRect &bounds, unsigned short tabHeight = 1) noexcept;

    ~TabControl() override;

    void addTab(const std::string &title, TabPageView *page, unsigned short command = 0);
    TabPageView *createTab(const std::string &title, unsigned short command = 0);
    bool selectByCommand(unsigned short command);
    void nextTab();
    void previousTab();

    void selectTab(std::size_t index);
    std::size_t currentIndex() const noexcept;
    std::size_t tabCount() const noexcept;

    void handleEvent(TEvent &event) override;
    void draw() override;
    void changeBounds(const TRect &bounds) override;
    void shutDown() override;

private:
    void layoutPage(TabPageView &page);
    void updatePagesBounds();
    void selectNext();
    void selectPrevious();

    std::vector<Tab> m_tabs;
    std::size_t m_current = 0;
    unsigned short m_tabHeight = 1;
};

} // namespace ck::ui
