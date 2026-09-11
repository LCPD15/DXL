#pragma once

#include <string>
#include <string_view>

namespace DXL::LaunchArguments {

inline void AppendUtf8(std::string& value, unsigned codepoint) {
    if (codepoint < 0x80) value += static_cast<char>(codepoint);
    else if (codepoint < 0x800) {
        value += static_cast<char>(0xc0 | (codepoint >> 6));
        value += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
        value += static_cast<char>(0xe0 | (codepoint >> 12));
        value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        value += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        value += static_cast<char>(0xf0 | (codepoint >> 18));
        value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        value += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

inline bool ReadHex4(std::string_view json, size_t& pos, unsigned& value) {
    value = 0;
    if (json.size() - pos < 4) return false;
    for (int i = 0; i < 4; ++i) {
        const char c = json[pos++];
        const int hex = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (hex < 0) return false;
        value = (value << 4) | static_cast<unsigned>(hex);
    }
    return true;
}

inline bool ReadString(std::string_view json, size_t& pos, std::string& value) {
    value.clear();
    if (pos >= json.size() || json[pos++] != '"') return false;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\') { value += c; continue; }
        if (pos == json.size()) return false;
        switch (json[pos++]) {
        case '"': value += '"'; break;
        case '\\': value += '\\'; break;
        case '/': value += '/'; break;
        case 'b': value += '\b'; break;
        case 'f': value += '\f'; break;
        case 'n': value += '\n'; break;
        case 'r': value += '\r'; break;
        case 't': value += '\t'; break;
        case 'u': {
            unsigned codepoint = 0;
            if (!ReadHex4(json, pos, codepoint)) return false;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (json.substr(pos, 2) != "\\u") return false;
                pos += 2;
                unsigned low = 0;
                if (!ReadHex4(json, pos, low) || low < 0xdc00 || low > 0xdfff) return false;
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) return false;
            if (codepoint == 0) return false; // CreateProcess uses NUL-terminated strings.
            AppendUtf8(value, codepoint);
            break;
        }
        default: return false;
        }
    }
    return false;
}

// Launch messages are flat. Walk complete string tokens so a quoted argument
// cannot masquerade as a field name, and decode JSON once before Win32 sees it.
inline std::string ReadField(std::string_view json, std::string_view name) {
    size_t pos = 0;
    while (pos < json.size()) {
        if (json[pos] != '"') { ++pos; continue; }
        std::string token;
        if (!ReadString(json, pos, token)) return {};
        size_t colon = json.find_first_not_of(" \t\r\n", pos);
        if (token != name || colon == std::string_view::npos || json[colon] != ':') continue;
        pos = json.find_first_not_of(" \t\r\n", colon + 1);
        std::string value;
        return ReadString(json, pos, value) ? value : std::string();
    }
    return {};
}

// args is already a Windows command-line tail. Preserve user quoting and do
// not route it through cmd.exe or quote the whole tail as one argument.
inline std::wstring CommandLine(std::wstring_view exePath, std::wstring_view args) {
    std::wstring result = L"\"";
    result += exePath;
    result += L'"';
    if (!args.empty()) { result += L' '; result += args; }
    return result;
}

} // namespace DXL::LaunchArguments
