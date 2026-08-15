#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace macha {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Json parse() {
        skipWs();
        Json value = parseValue();
        skipWs();
        if (pos_ != text_.size()) throw JsonError("trailing JSON data");
        return value;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;

    void skipWs() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' || text_[pos_] == '\t')) ++pos_;
    }

    char peek() const {
        if (pos_ >= text_.size()) throw JsonError("unexpected end of JSON");
        return text_[pos_];
    }

    bool take(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    void expect(std::string_view s) {
        if (text_.substr(pos_, s.size()) != s) throw JsonError("invalid JSON token");
        pos_ += s.size();
    }

    Json parseValue() {
        skipWs();
        switch (peek()) {
            case 'n': expect("null"); return Json(nullptr);
            case 't': expect("true"); return Json(true);
            case 'f': expect("false"); return Json(false);
            case '"': return Json(parseString());
            case '[': return parseArray();
            case '{': return parseObject();
            default: return parseNumber();
        }
    }

    unsigned parseHex4() {
        if (pos_ + 4 > text_.size()) throw JsonError("bad unicode escape");
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else throw JsonError("bad unicode escape");
        }
        return code;
    }

    static void appendUtf8(std::string& out, unsigned code) {
        if (code <= 0x7f) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code <= 0x10ffff) {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            throw JsonError("unicode codepoint out of range");
        }
    }

    std::string parseString() {
        if (!take('"')) throw JsonError("expected string");
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) {
                throw JsonError("unescaped control character in string");
            }
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) throw JsonError("bad escape");
            char e = text_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned code = parseHex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (pos_ + 2 > text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
                            throw JsonError("high surrogate without low surrogate");
                        }
                        pos_ += 2;
                        const unsigned low = parseHex4();
                        if (low < 0xdc00 || low > 0xdfff) {
                            throw JsonError("invalid low surrogate");
                        }
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else if (code >= 0xdc00 && code <= 0xdfff) {
                        throw JsonError("unexpected low surrogate");
                    }
                    appendUtf8(out, code);
                    break;
                }
                default: throw JsonError("unsupported escape");
            }
        }
        throw JsonError("unterminated string");
    }

    Json parseArray() {
        take('[');
        Json::Array out;
        skipWs();
        if (take(']')) return Json(std::move(out));
        while (true) {
            out.push_back(parseValue());
            skipWs();
            if (take(']')) break;
            if (!take(',')) throw JsonError("expected ',' in array");
        }
        return Json(std::move(out));
    }

    Json parseObject() {
        take('{');
        Json::Object out;
        skipWs();
        if (take('}')) return Json(std::move(out));
        while (true) {
            skipWs();
            auto key = parseString();
            skipWs();
            if (!take(':')) throw JsonError("expected ':' in object");
            auto [it, inserted] = out.emplace(std::move(key), parseValue());
            (void)it;
            if (!inserted) throw JsonError("duplicate object key");
            skipWs();
            if (take('}')) break;
            if (!take(',')) throw JsonError("expected ',' in object");
        }
        return Json(std::move(out));
    }

    Json parseNumber() {
        const auto start = pos_;
        const bool negative = take('-');
        if (take('0')) {
        } else {
            if (pos_ >= text_.size() || text_[pos_] < '1' || text_[pos_] > '9') {
                throw JsonError("invalid number");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }

        bool floating = false;
        if (take('.')) {
            floating = true;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                throw JsonError("invalid number");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            floating = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < '0' || text_[pos_] > '9') {
                throw JsonError("invalid exponent");
            }
            while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }

        const auto part = text_.substr(start, pos_ - start);
        if (!floating) {
            if (negative) {
                std::int64_t value = 0;
                const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
                if (result.ec == std::errc{}) return Json(value);
            } else {
                std::uint64_t value = 0;
                const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
                if (result.ec == std::errc{}) return Json(value);
            }
        }

        const std::string owned(part);
        char* end = nullptr;
        const double value = std::strtod(owned.c_str(), &end);
        if (end != owned.c_str() + owned.size() || !std::isfinite(value)) {
            throw JsonError("invalid number");
        }
        return Json(value);
    }

};

void appendEscaped(std::string& out, std::string_view value) {
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xf]);
                    out.push_back(hex[c & 0xf]);
                } else out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

void dumpValue(std::string& out, const Json& value) {
    if (value.isNull()) { out += "null"; return; }
    if (value.isBool()) { out += value.asBool() ? "true" : "false"; return; }
    if (value.isInt64()) {
        out += std::to_string(value.asInt64());
        return;
    }
    if (value.isUInt64()) {
        out += std::to_string(value.asUInt64());
        return;
    }
    if (value.isNumber()) {
        const double number = value.asNumber();
        if (!std::isfinite(number)) throw JsonError("cannot encode non-finite JSON number");
        std::ostringstream ss;
        ss << std::setprecision(17) << number;
        out += ss.str();
        return;
    }
    if (value.isString()) { appendEscaped(out, value.asString()); return; }
    if (value.isArray()) {
        out.push_back('[');
        bool first = true;
        for (const auto& v : value.asArray()) {
            if (!first) out.push_back(',');
            first = false;
            dumpValue(out, v);
        }
        out.push_back(']');
        return;
    }
    out.push_back('{');
    bool first = true;
    for (const auto& [k, v] : value.asObject()) {
        if (!first) out.push_back(',');
        first = false;
        appendEscaped(out, k);
        out.push_back(':');
        dumpValue(out, v);
    }
    out.push_back('}');
}

} // namespace

Json Json::parse(std::string_view text) { return Parser(text).parse(); }
std::string Json::dump() const { std::string out; dumpValue(out, *this); return out; }
bool Json::isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::isBool() const noexcept { return std::holds_alternative<bool>(value_); }
bool Json::isNumber() const noexcept {
    return std::holds_alternative<std::int64_t>(value_) ||
           std::holds_alternative<std::uint64_t>(value_) ||
           std::holds_alternative<double>(value_);
}
bool Json::isInt64() const noexcept { return std::holds_alternative<std::int64_t>(value_); }
bool Json::isUInt64() const noexcept { return std::holds_alternative<std::uint64_t>(value_); }
bool Json::isString() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::isArray() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::isObject() const noexcept { return std::holds_alternative<Object>(value_); }
bool Json::asBool() const { return std::get<bool>(value_); }
double Json::asNumber() const {
    if (auto p = std::get_if<std::int64_t>(&value_)) return static_cast<double>(*p);
    if (auto p = std::get_if<std::uint64_t>(&value_)) return static_cast<double>(*p);
    return std::get<double>(value_);
}
std::int64_t Json::asInt64() const {
    if (auto p = std::get_if<std::int64_t>(&value_)) return *p;
    if (auto p = std::get_if<std::uint64_t>(&value_)) {
        if (*p > static_cast<std::uint64_t>(INT64_MAX)) throw JsonError("integer out of range");
        return static_cast<std::int64_t>(*p);
    }
    const auto value = std::get<double>(value_);
    if (!std::isfinite(value) || std::floor(value) != value ||
        value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        value > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw JsonError("number is not an int64");
    }
    return static_cast<std::int64_t>(value);
}
std::uint64_t Json::asUInt64() const {
    if (auto p = std::get_if<std::uint64_t>(&value_)) return *p;
    if (auto p = std::get_if<std::int64_t>(&value_)) {
        if (*p < 0) throw JsonError("negative integer where unsigned expected");
        return static_cast<std::uint64_t>(*p);
    }
    const auto value = std::get<double>(value_);
    if (!std::isfinite(value) || std::floor(value) != value || value < 0 ||
        value > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        throw JsonError("number is not a uint64");
    }
    return static_cast<std::uint64_t>(value);
}
const std::string& Json::asString() const { return std::get<std::string>(value_); }
const Json::Array& Json::asArray() const { return std::get<Array>(value_); }
Json::Array& Json::asArray() { return std::get<Array>(value_); }
const Json::Object& Json::asObject() const { return std::get<Object>(value_); }
Json::Object& Json::asObject() { return std::get<Object>(value_); }
const Json* Json::find(std::string_view key) const {
    if (!isObject()) return nullptr;
    auto it = asObject().find(key);
    return it == asObject().end() ? nullptr : &it->second;
}
Json* Json::find(std::string_view key) {
    if (!isObject()) return nullptr;
    auto it = asObject().find(key);
    return it == asObject().end() ? nullptr : &it->second;
}
Json& Json::operator[](std::string key) {
    if (!isObject()) value_ = Object{};
    return asObject()[std::move(key)];
}

} // namespace macha
