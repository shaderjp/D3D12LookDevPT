#include "SimpleJson.h"

#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace cld
{
JsonParser::JsonParser(const std::string& text)
    : m_text(text)
{
}

JsonValue JsonParser::Parse()
{
    JsonValue value = ParseValue();
    SkipWhitespace();
    if (m_position != m_text.size())
    {
        throw std::runtime_error("Unexpected trailing characters in JSON.");
    }
    return value;
}

JsonValue JsonParser::ParseValue()
{
    SkipWhitespace();
    if (m_position >= m_text.size())
    {
        throw std::runtime_error("Unexpected end of JSON.");
    }

    switch (m_text[m_position])
    {
    case '{': return ParseObject();
    case '[': return ParseArray();
    case '"':
    {
        JsonValue value;
        value.type = JsonValue::Type::String;
        value.string = ParseString();
        return value;
    }
    case 't': return ParseLiteral("true", JsonValue::Type::Bool, true);
    case 'f': return ParseLiteral("false", JsonValue::Type::Bool, false);
    case 'n': return ParseLiteral("null", JsonValue::Type::Null, false);
    default:
        if (m_text[m_position] == '-' || std::isdigit(static_cast<unsigned char>(m_text[m_position])))
        {
            return ParseNumber();
        }
        throw std::runtime_error("Invalid token in JSON.");
    }
}

JsonValue JsonParser::ParseObject()
{
    JsonValue value;
    value.type = JsonValue::Type::Object;
    Expect('{');
    SkipWhitespace();
    if (TryConsume('}'))
    {
        return value;
    }

    while (true)
    {
        SkipWhitespace();
        const std::string key = ParseString();
        SkipWhitespace();
        Expect(':');
        value.object[key] = ParseValue();
        SkipWhitespace();
        if (TryConsume('}'))
        {
            break;
        }
        Expect(',');
    }
    return value;
}

JsonValue JsonParser::ParseArray()
{
    JsonValue value;
    value.type = JsonValue::Type::Array;
    Expect('[');
    SkipWhitespace();
    if (TryConsume(']'))
    {
        return value;
    }

    while (true)
    {
        value.array.push_back(ParseValue());
        SkipWhitespace();
        if (TryConsume(']'))
        {
            break;
        }
        Expect(',');
    }
    return value;
}

JsonValue JsonParser::ParseLiteral(const char* literal, JsonValue::Type type, bool boolean)
{
    const std::size_t length = std::strlen(literal);
    if (m_text.compare(m_position, length, literal) != 0)
    {
        throw std::runtime_error("Invalid literal in JSON.");
    }
    m_position += length;

    JsonValue value;
    value.type = type;
    value.boolean = boolean;
    return value;
}

JsonValue JsonParser::ParseNumber()
{
    const std::size_t begin = m_position;
    if (m_text[m_position] == '-')
    {
        ++m_position;
    }
    ConsumeDigits();
    if (m_position < m_text.size() && m_text[m_position] == '.')
    {
        ++m_position;
        ConsumeDigits();
    }
    if (m_position < m_text.size() && (m_text[m_position] == 'e' || m_text[m_position] == 'E'))
    {
        ++m_position;
        if (m_position < m_text.size() && (m_text[m_position] == '+' || m_text[m_position] == '-'))
        {
            ++m_position;
        }
        ConsumeDigits();
    }

    JsonValue value;
    value.type = JsonValue::Type::Number;
    value.number = std::stod(m_text.substr(begin, m_position - begin));
    return value;
}

std::string JsonParser::ParseString()
{
    Expect('"');
    std::string result;
    while (m_position < m_text.size())
    {
        const char ch = m_text[m_position++];
        if (ch == '"')
        {
            return result;
        }
        if (ch != '\\')
        {
            result.push_back(ch);
            continue;
        }

        if (m_position >= m_text.size())
        {
            throw std::runtime_error("Unterminated escape in JSON string.");
        }

        const char escaped = m_text[m_position++];
        switch (escaped)
        {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u':
            for (int i = 0; i < 4; ++i)
            {
                if (m_position >= m_text.size() || !std::isxdigit(static_cast<unsigned char>(m_text[m_position])))
                {
                    throw std::runtime_error("Invalid unicode escape in JSON string.");
                }
                ++m_position;
            }
            result.push_back('?');
            break;
        default:
            throw std::runtime_error("Invalid escape in JSON string.");
        }
    }
    throw std::runtime_error("Unterminated string in JSON.");
}

void JsonParser::ConsumeDigits()
{
    bool consumed = false;
    while (m_position < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_position])))
    {
        consumed = true;
        ++m_position;
    }
    if (!consumed)
    {
        throw std::runtime_error("Invalid number in JSON.");
    }
}

void JsonParser::SkipWhitespace()
{
    while (m_position < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_position])))
    {
        ++m_position;
    }
}

void JsonParser::Expect(char expected)
{
    if (m_position >= m_text.size() || m_text[m_position] != expected)
    {
        throw std::runtime_error("Unexpected character in JSON.");
    }
    ++m_position;
}

bool JsonParser::TryConsume(char expected)
{
    if (m_position < m_text.size() && m_text[m_position] == expected)
    {
        ++m_position;
        return true;
    }
    return false;
}

const JsonValue* FindMember(const JsonValue& value, const char* name)
{
    if (value.type != JsonValue::Type::Object)
    {
        return nullptr;
    }
    const auto it = value.object.find(name);
    return it != value.object.end() ? &it->second : nullptr;
}

std::string JsonStringOr(const JsonValue& value, const char* name, const std::string& fallback)
{
    const JsonValue* member = FindMember(value, name);
    return member && member->type == JsonValue::Type::String ? member->string : fallback;
}

double JsonNumberOr(const JsonValue& value, const char* name, double fallback)
{
    const JsonValue* member = FindMember(value, name);
    return member && member->type == JsonValue::Type::Number ? member->number : fallback;
}

bool JsonBoolOr(const JsonValue& value, const char* name, bool fallback)
{
    const JsonValue* member = FindMember(value, name);
    return member && member->type == JsonValue::Type::Bool ? member->boolean : fallback;
}

std::array<float, 3> JsonFloat3Or(const JsonValue& value, const char* name, const std::array<float, 3>& fallback)
{
    const JsonValue* member = FindMember(value, name);
    if (!member || member->type != JsonValue::Type::Array || member->array.size() < 3)
    {
        return fallback;
    }

    std::array<float, 3> result = fallback;
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        if (member->array[i].type != JsonValue::Type::Number)
        {
            return fallback;
        }
        result[i] = static_cast<float>(member->array[i].number);
    }
    return result;
}

std::array<float, 4> JsonFloat4Or(const JsonValue& value, const char* name, const std::array<float, 4>& fallback)
{
    const JsonValue* member = FindMember(value, name);
    if (!member || member->type != JsonValue::Type::Array || member->array.size() < 4)
    {
        return fallback;
    }

    std::array<float, 4> result = fallback;
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        if (member->array[i].type != JsonValue::Type::Number)
        {
            return fallback;
        }
        result[i] = static_cast<float>(member->array[i].number);
    }
    return result;
}

std::string EscapeJson(const std::string& text)
{
    std::ostringstream escaped;
    for (const unsigned char ch : text)
    {
        switch (ch)
        {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (ch < 0x20)
            {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            }
            else
            {
                escaped << static_cast<char>(ch);
            }
            break;
        }
    }
    return escaped.str();
}

std::string JsonValueToJson(const JsonValue& value)
{
    std::ostringstream json;
    switch (value.type)
    {
    case JsonValue::Type::Null:
        json << "null";
        break;
    case JsonValue::Type::Bool:
        json << (value.boolean ? "true" : "false");
        break;
    case JsonValue::Type::Number:
        json << value.number;
        break;
    case JsonValue::Type::String:
        json << "\"" << EscapeJson(value.string) << "\"";
        break;
    case JsonValue::Type::Array:
        json << "[";
        for (std::size_t i = 0; i < value.array.size(); ++i)
        {
            if (i > 0)
            {
                json << ",";
            }
            json << JsonValueToJson(value.array[i]);
        }
        json << "]";
        break;
    case JsonValue::Type::Object:
    {
        json << "{";
        std::size_t index = 0;
        for (const auto& [key, member] : value.object)
        {
            if (index++ > 0)
            {
                json << ",";
            }
            json << "\"" << EscapeJson(key) << "\":" << JsonValueToJson(member);
        }
        json << "}";
        break;
    }
    }
    return json.str();
}

std::string TrimAscii(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    return text;
}
}
