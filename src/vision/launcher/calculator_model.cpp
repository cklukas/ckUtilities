#include "calculator_model.hpp"

#include <charconv>
#include <cmath>
#include <system_error>

namespace ck::vision
{
namespace
{

class ExpressionParser
{
public:
    explicit ExpressionParser(std::string_view text) : text_(text) {}

    CalculatorEvaluation parse()
    {
        const auto value = parse_sum();
        skip_spaces();
        if (!value || position_ != text_.size())
            return {false, "Error"};
        if (!std::isfinite(*value))
            return {false, "Error"};

        char output[64]{};
        const auto [end, error] = std::to_chars(std::begin(output), std::end(output), *value,
                                                std::chars_format::general, 12);
        if (error != std::errc{})
            return {false, "Error"};
        return {true, std::string(output, end)};
    }

private:
    std::optional<double> parse_sum()
    {
        auto value = parse_product();
        while (value)
        {
            skip_spaces();
            if (!consume('+') && !consume('-'))
                break;
            const char operation = text_[position_ - 1];
            const auto right = parse_product();
            if (!right)
                return std::nullopt;
            *value = operation == '+' ? *value + *right : *value - *right;
        }
        return value;
    }

    std::optional<double> parse_product()
    {
        auto value = parse_postfix();
        while (value)
        {
            skip_spaces();
            if (!consume('*') && !consume('/'))
                break;
            const char operation = text_[position_ - 1];
            const auto right = parse_postfix();
            if (!right || (operation == '/' && *right == 0.0))
                return std::nullopt;
            *value = operation == '*' ? *value * *right : *value / *right;
        }
        return value;
    }

    std::optional<double> parse_postfix()
    {
        auto value = parse_primary();
        while (value)
        {
            skip_spaces();
            if (!consume('%'))
                break;
            *value /= 100.0;
        }
        return value;
    }

    std::optional<double> parse_primary()
    {
        skip_spaces();
        if (consume('+'))
            return parse_primary();
        if (consume('-'))
        {
            const auto value = parse_primary();
            return value ? std::optional<double>{-*value} : std::nullopt;
        }
        if (consume('('))
        {
            const auto value = parse_sum();
            skip_spaces();
            return value && consume(')') ? value : std::nullopt;
        }

        const std::size_t first = position_;
        bool has_digit = false;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
        {
            has_digit = true;
            ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.')
        {
            ++position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            {
                has_digit = true;
                ++position_;
            }
        }
        if (!has_digit)
            return std::nullopt;

        double value = 0.0;
        const auto [end, error] = std::from_chars(text_.data() + first, text_.data() + position_, value);
        return error == std::errc{} && end == text_.data() + position_ ? std::optional<double>{value}
                                                                        : std::nullopt;
    }

    void skip_spaces()
    {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t'))
            ++position_;
    }

    bool consume(char expected)
    {
        if (position_ >= text_.size() || text_[position_] != expected)
            return false;
        ++position_;
        return true;
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

} // namespace

CalculatorEvaluation CalculatorModel::evaluate(std::string_view expression)
{
    return ExpressionParser(expression).parse();
}

} // namespace ck::vision
