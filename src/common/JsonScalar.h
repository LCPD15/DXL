#pragma once
#include <string>
#include <string_view>

namespace DXL::JsonScalar {
// Locate a scalar boundary without interpreting punctuation inside strings.
inline size_t End(std::string_view text, size_t start) noexcept {
    bool quoted = false, escaped = false;
    for (size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == ',' || ch == '}' || ch == '\r' || ch == '\n') return i;
    }
    return text.size();
}
inline std::string Quote(std::string_view value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += char(ch); }
        else if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += char(ch);
    }
    return out + '"';
}
inline bool Decode(std::string_view value, std::string& out) {
    out.clear();
    if (value.empty() || value.front() != '"') return false;
    const auto appendCodepoint = [&](unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
        else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
    };
    const auto readHex = [&](size_t at, unsigned& cp) {
        if (at + 4 > value.size()) return false;
        cp = 0;
        for (size_t n = 0; n < 4; ++n) {
            const char c = value[at+n];
            const unsigned digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : 16;
            if (digit > 15) return false;
            cp = cp * 16 + digit;
        }
        return true;
    };
    for (size_t i = 1; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 32) return false;
        if (c != '\\') { out += c; continue; }
        if (++i >= value.size()) return false;
        switch (value[i]) {
        case '"': case '\\': case '/': out += value[i]; break;
        case 'n': out += '\n'; break; case 'r': out += '\r'; break; case 't': out += '\t'; break;
        case 'b': out += '\b'; break; case 'f': out += '\f'; break;
        case 'u': {
            unsigned cp = 0;
            if (!readHex(i+1, cp)) return false;
            i += 4;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (i+6 >= value.size() || value[i+1] != '\\' || value[i+2] != 'u') return false;
                unsigned lo = 0;
                if (!readHex(i+3, lo) || lo < 0xdc00 || lo > 0xdfff) return false;
                cp = 0x10000 + ((cp-0xd800) << 10) + lo-0xdc00; i += 6;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
            appendCodepoint(cp); break;
        }
        default: return false;
        }
    }
    return false;
}
} // namespace DXL::JsonScalar
