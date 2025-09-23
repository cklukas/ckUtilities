#include "ck/find/search_model.hpp"

#include "ck/find/cli_buffer_utils.hpp"

namespace ck::find
{

SearchSpecification makeDefaultSpecification()
{
    SearchSpecification spec;
    copyToArray(spec.startLocation, ".");
    spec.enableActionOptions = true;
    spec.actionOptions.print = true;
    return spec;
}

} // namespace ck::find

