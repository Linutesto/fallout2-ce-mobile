#ifndef FALLOUT_NPC_BRAIN_JSON_H_
#define FALLOUT_NPC_BRAIN_JSON_H_
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
namespace fallout { namespace brainJson {
    struct JsonValue {
        enum class Type {
            Null,
            Bool,
            Number,
            String,
            Array,
            Object,
        };

        Type type = Type::Null;
        bool boolean = false;
        double number = 0;
        std::string string;
        std::vector<JsonValue> array;
        std::vector<std::pair<std::string, JsonValue>> object;

        const JsonValue* get(const char* key) const
        {
            if (type != Type::Object) {
                return nullptr;
            }
            for (const auto& entry : object) {
                if (entry.first == key) {
                    return &entry.second;
                }
            }
            return nullptr;
        }
    };

    class JsonParser {
    public:
        JsonParser(const std::string& text)
            : text_(text)
        {
        }

        bool parse(JsonValue* value)
        {
            if (value == nullptr || text_.size() > 256 * 1024) return false;
            *value = JsonValue();
            pos_ = 0;
            if (!parseValue(value, 0)) {
                return false;
            }
            skipSpace();
            return pos_ == text_.size();
        }

    private:
        static constexpr int kMaxDepth = 32;

        const std::string& text_;
        size_t pos_ = 0;

        void skipSpace()
        {
            while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
                pos_++;
            }
        }

        bool consume(const char* literal)
        {
            size_t length = strlen(literal);
            if (text_.compare(pos_, length, literal) != 0) {
                return false;
            }
            pos_ += length;
            return true;
        }

        static void appendUtf8(std::string& out, unsigned int codepoint)
        {
            if (codepoint < 0x80) {
                out += static_cast<char>(codepoint);
            } else if (codepoint < 0x800) {
                out += static_cast<char>(0xC0 | (codepoint >> 6));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint < 0x10000) {
                out += static_cast<char>(0xE0 | (codepoint >> 12));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (codepoint >> 18));
                out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        }

        bool parseHex4(unsigned int* value)
        {
            if (pos_ + 4 > text_.size()) {
                return false;
            }
            unsigned int result = 0;
            for (int index = 0; index < 4; index++) {
                char ch = text_[pos_++];
                result <<= 4;
                if (ch >= '0' && ch <= '9') {
                    result |= ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    result |= ch - 'a' + 10;
                } else if (ch >= 'A' && ch <= 'F') {
                    result |= ch - 'A' + 10;
                } else {
                    return false;
                }
            }
            *value = result;
            return true;
        }

        bool parseString(std::string* out)
        {
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return false;
            }
            pos_++;
            while (pos_ < text_.size()) {
                char ch = text_[pos_++];
                if (ch == '"') {
                    return true;
                }
                if (static_cast<unsigned char>(ch) < 0x20) {
                    return false;
                }
                if (ch != '\\') {
                    *out += ch;
                    continue;
                }
                if (pos_ >= text_.size()) {
                    return false;
                }
                char escape = text_[pos_++];
                switch (escape) {
                case '"':
                case '\\':
                case '/':
                    *out += escape;
                    break;
                case 'b':
                    *out += '\b';
                    break;
                case 'f':
                    *out += '\f';
                    break;
                case 'n':
                    *out += '\n';
                    break;
                case 'r':
                    *out += '\r';
                    break;
                case 't':
                    *out += '\t';
                    break;
                case 'u': {
                    unsigned int codepoint;
                    if (!parseHex4(&codepoint)) {
                        return false;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        unsigned int low;
                        if (!consume("\\u") || !parseHex4(&low) || low < 0xDC00 || low > 0xDFFF) {
                            return false;
                        }
                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(*out, codepoint);
                    break;
                }
                default:
                    return false;
                }
            }
            return false;
        }

        bool parseValue(JsonValue* value, int depth)
        {
            if (depth > kMaxDepth) {
                return false;
            }
            skipSpace();
            if (pos_ >= text_.size()) {
                return false;
            }

            char ch = text_[pos_];
            if (ch == '{') {
                pos_++;
                value->type = JsonValue::Type::Object;
                skipSpace();
                if (pos_ < text_.size() && text_[pos_] == '}') {
                    pos_++;
                    return true;
                }
                for (;;) {
                    skipSpace();
                    std::string key;
                    if (!parseString(&key)) {
                        return false;
                    }
                    skipSpace();
                    if (!consume(":")) {
                        return false;
                    }
                    JsonValue child;
                    if (!parseValue(&child, depth + 1)) {
                        return false;
                    }
                    value->object.emplace_back(std::move(key), std::move(child));
                    skipSpace();
                    if (consume(",")) {
                        continue;
                    }
                    return consume("}");
                }
            } else if (ch == '[') {
                pos_++;
                value->type = JsonValue::Type::Array;
                skipSpace();
                if (pos_ < text_.size() && text_[pos_] == ']') {
                    pos_++;
                    return true;
                }
                for (;;) {
                    JsonValue child;
                    if (!parseValue(&child, depth + 1)) {
                        return false;
                    }
                    value->array.push_back(std::move(child));
                    skipSpace();
                    if (consume(",")) {
                        continue;
                    }
                    return consume("]");
                }
            } else if (ch == '"') {
                value->type = JsonValue::Type::String;
                return parseString(&value->string);
            } else if (consume("true")) {
                value->type = JsonValue::Type::Bool;
                value->boolean = true;
                return true;
            } else if (consume("false")) {
                value->type = JsonValue::Type::Bool;
                return true;
            } else if (consume("null")) {
                value->type = JsonValue::Type::Null;
                return true;
            } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                // CE: strtod alone accepts non-JSON numbers (01, 1., hex, infinity).
                size_t begin = pos_;
                if (text_[pos_] == '-') pos_++;
                if (pos_ >= text_.size()) return false;
                if (text_[pos_] == '0') pos_++;
                else {
                    if (text_[pos_] < '1' || text_[pos_] > '9') return false;
                    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
                }
                if (pos_ < text_.size() && text_[pos_] == '.') {
                    size_t digits = ++pos_;
                    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
                    if (digits == pos_) return false;
                }
                if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
                    pos_++;
                    if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
                    size_t digits = pos_;
                    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') pos_++;
                    if (digits == pos_) return false;
                }
                std::string token = text_.substr(begin, pos_ - begin);
                double number = strtod(token.c_str(), nullptr);
                if (!std::isfinite(number)) return false;
                value->type = JsonValue::Type::Number;
                value->number = number;
                return true;
            }
            return false;
        }
    };

    inline void jsonAppendString(std::string& out, const std::string& text)
    {
        out += '"';
        for (unsigned char ch : text) {
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20 || ch >= 0x7F) {
                    // The game's text is cp1252: anything else is sent as
                    // its code point.
                    char escape[8];
                    unsigned int codepoint = ch;
                    if (ch >= 0x80 && ch < 0xA0) {
                        static const unsigned short kCp1252[32] = {
                            0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                            0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
                        };
                        codepoint = kCp1252[ch - 0x80];
                    }
                    snprintf(escape, sizeof(escape), "\\u%04x", codepoint);
                    out += escape;
                } else {
                    out += static_cast<char>(ch);
                }
                break;
            }
        }
        out += '"';
    }

    inline void jsonAppendKey(std::string& out, const char* key)
    {
        jsonAppendString(out, key);
        out += ':';
    }

    inline void jsonAppendStringArray(std::string& out, const std::vector<std::string>& values)
    {
        out += '[';
        for (size_t index = 0; index < values.size(); index++) {
            if (index != 0) {
                out += ',';
            }
            jsonAppendString(out, values[index]);
        }
        out += ']';
    }

} }
#endif
