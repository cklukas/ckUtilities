#pragma once

#include "../tvision_include.hpp"

class SolidColorView : public TView
{
public:
    SolidColorView(const TRect &bounds);
    virtual void draw() override;
};
