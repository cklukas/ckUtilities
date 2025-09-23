#include "ck/find/dialog_utils.hpp"

#define Uses_TSItem
#include <tvision/tv.h>

namespace ck::find
{

TSItem *makeItemList(std::initializer_list<const char *> labels)
{
    TSItem *head = nullptr;
    TSItem **tail = &head;
    for (const char *text : labels)
    {
        *tail = new TSItem(text, nullptr);
        tail = &(*tail)->next;
    }
    return head;
}

} // namespace ck::find

