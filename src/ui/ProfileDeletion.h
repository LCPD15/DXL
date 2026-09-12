#pragma once
#include "LaunchArguments.h"
#include <string>
#include <string_view>

namespace DXL::ProfileDeletion {

// Read only direct object members. Nested settings and strings containing field
// names must never select a different application for a driver operation.
struct JsonReader {
    std::string_view text;
    size_t pos = 0;
    void Space() { while (pos < text.size() && std::string_view(" \t\r\n").find(text[pos]) != std::string_view::npos) ++pos; }
    bool Take(char c) { Space(); if (pos == text.size() || text[pos] != c) return false; ++pos; return true; }
    bool String(std::string& value) { Space(); return LaunchArguments::ReadString(text, pos, value); }
    bool Value(unsigned depth = 0) {
        Space();
        if (pos == text.size() || depth > 32) return false;
        const char first = text[pos];
        if (first == '"') { std::string ignored; return String(ignored); }
        if (first == '{' || first == '[') {
            ++pos;
            const char end = first == '{' ? '}' : ']';
            if (Take(end)) return true;
            do {
                if (first == '{') { std::string key; if (!String(key) || !Take(':')) return false; }
                if (!Value(depth + 1)) return false;
                if (Take(end)) return true;
            } while (Take(','));
            return false;
        }
        for (auto literal : {std::string_view("true"), std::string_view("false"), std::string_view("null")}) {
            if (text.substr(pos, literal.size()) == literal) { pos += literal.size(); return true; }
        }
        if (text[pos] == '-') ++pos;
        if (pos == text.size()) return false;
        if (text[pos] == '0') ++pos;
        else {
            if (text[pos] < '1' || text[pos] > '9') return false;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
        }
        if (pos < text.size() && text[pos] == '.') {
            const size_t start = ++pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (pos == start) return false;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
            if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
            const size_t start = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (pos == start) return false;
        }
        return true;
    }
    bool End() { Space(); return pos == text.size(); }
};

inline bool ObjectField(std::string_view text, std::string_view field,
    std::string_view& value, bool& found) {
    JsonReader reader{text};
    found = false;
    value = {};
    if (!reader.Take('{')) return false;
    if (reader.Take('}')) return reader.End();
    do {
        std::string key;
        if (!reader.String(key) || !reader.Take(':')) return false;
        reader.Space();
        const size_t start = reader.pos;
        if (!reader.Value()) return false;
        if (key == field) {
            if (found) return false;
            found = true;
            value = text.substr(start, reader.pos - start);
        }
        if (reader.Take('}')) return reader.End();
    } while (reader.Take(','));
    return false;
}

inline bool StringField(std::string_view text, std::string_view name,
    std::string& value, bool required) {
    std::string_view raw;
    bool found = false;
    if (!ObjectField(text, name, raw, found) || (!found && required)) return false;
    value.clear();
    if (!found) return true;
    JsonReader reader{raw};
    return reader.String(value) && reader.End();
}

inline bool ResolveExePath(std::string_view settings, std::string_view profileId, std::string& exePath) {
    exePath.clear();
    if (profileId.empty() || profileId == "default") return false;
    std::string_view profiles;
    bool found = false;
    if (!ObjectField(settings, "profiles", profiles, found) || !found) return false;
    JsonReader reader{profiles};
    if (!reader.Take('[') || reader.Take(']')) return false;
    bool matched = false;
    do {
        reader.Space();
        const size_t start = reader.pos;
        if (!reader.Value()) return false;
        const auto profile = profiles.substr(start, reader.pos - start);
        std::string id;
        if (!StringField(profile, "id", id, true)) return false;
        if (id == profileId) {
            if (matched || !StringField(profile, "exePath", exePath, false)) return false;
            matched = true;
        }
        if (reader.Take(']')) return matched && reader.End();
    } while (reader.Take(','));
    return false;
}

// A delete request can only consume the model saved by its immediately
// preceding explicit confirmation. A failed save cannot use an old EXE path.
struct SavedRequest {
    double sequence = 0;
    std::string settings;
    void Record(double request, bool ok, std::string_view model) {
        sequence = request;
        settings = ok ? std::string(model) : std::string();
    }
    bool Take(double request, std::string_view profileId, std::string& exePath) {
        if (request <= 0 || request != sequence) return false;
        sequence = 0;
        const bool ok = ResolveExePath(settings, profileId, exePath);
        settings.clear();
        return ok;
    }
};
} // namespace DXL::ProfileDeletion
