#pragma once

#include <string>
#include <string_view>

namespace ck::vision
{

struct CalculatorEvaluation
{
    bool accepted = false;
    std::string text;
};

// A small, deterministic expression evaluator for the launcher calculator.
// It has no UI dependency, so the same arithmetic behavior is available to
// unit tests and any later presentation surface.
class CalculatorModel
{
public:
    static CalculatorEvaluation evaluate(std::string_view expression);
};

} // namespace ck::vision
