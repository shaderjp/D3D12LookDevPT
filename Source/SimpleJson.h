#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace cld
{
struct JsonValue
{
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::unordered_map<std::string, JsonValue> object;
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& text);
    JsonValue Parse();

private:
    JsonValue ParseValue();
    JsonValue ParseObject();
    JsonValue ParseArray();
    JsonValue ParseLiteral(const char* literal, JsonValue::Type type, bool boolean);
    JsonValue ParseNumber();
    std::string ParseString();
    void ConsumeDigits();
    void SkipWhitespace();
    void Expect(char expected);
    bool TryConsume(char expected);

    const std::string& m_text;
    std::size_t m_position = 0;
};

const JsonValue* FindMember(const JsonValue& value, const char* name);
std::string JsonStringOr(const JsonValue& value, const char* name, const std::string& fallback = {});
double JsonNumberOr(const JsonValue& value, const char* name, double fallback);
bool JsonBoolOr(const JsonValue& value, const char* name, bool fallback);
std::array<float, 3> JsonFloat3Or(const JsonValue& value, const char* name, const std::array<float, 3>& fallback);
std::array<float, 4> JsonFloat4Or(const JsonValue& value, const char* name, const std::array<float, 4>& fallback);
std::string EscapeJson(const std::string& text);
std::string JsonValueToJson(const JsonValue& value);
std::string TrimAscii(std::string text);
}
