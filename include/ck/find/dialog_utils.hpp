#pragma once

#include <initializer_list>

class TSItem;

namespace ck::find
{

TSItem *makeItemList(std::initializer_list<const char *> labels);

} // namespace ck::find

