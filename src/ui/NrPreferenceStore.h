#pragma once
#include "../common/SettingsReader.h"
#include "../common/JsonScalar.h"
#include "../common/NrParameterEdit.h"
#include "ProfileDeletion.h"
#include <filesystem>
#include <string>

namespace DXL {
inline bool UpdateNrPreferenceText(std::string& text, std::string_view key, double value) {
    uint32_t packed = 0;
    if (!EncodeNrEdit(key, value, packed)) return false;
    const auto set = [&](std::string_view name, std::string_view literal) {
        std::string_view raw;
        bool found = false;
        if (!ProfileDeletion::ObjectField(text, name, raw, found)) return false;
        if (found) text.replace(size_t(raw.data() - text.data()), raw.size(), literal);
        else {
            const auto open = text.find('{');
            const bool empty = text.find_first_not_of(" \t\r\n", open + 1) == text.find_last_of('}');
            text.insert(open + 1, "\n" + JsonScalar::Quote(name) + ": " + std::string(literal) + (empty ? "" : ","));
        }
        return true;
    };
    const auto literal = NrEditSpecs[packed >> 24].boolean ? std::string(value != 0 ? "true" : "false") : std::to_string(value);
    return set(key, literal) && (key != "nrForceCompatibility" || value == 0 || set("nrProcessingEnabled", "true"));
}

// The params overlay survives a game crash and takes precedence over the
// launcher's model. Read it before displaying a recoverable per-game switch.
inline std::string ReadForcedNrPreferences(const std::filesystem::path& profiles) {
    std::string json = "{";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(profiles, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto name = it->path().filename().u8string();
        std::string file(name.begin(), name.end());
        constexpr std::string_view suffix = ".params.json";
        if (!file.ends_with(suffix)) continue;
        file.resize(file.size() - suffix.size());
        if (file == "default" || file.empty()) continue;
        SettingsReader settings;
        if (!settings.Load(it->path())) continue;
        const bool value = settings.GetBool("nrForceCompatibility", false);
        if (value != settings.GetBool("nrForceCompatibility", true)) continue;
        if (json.size() > 1) json += ',';
        json += JsonScalar::Quote(file + ".json") + ':' + (value ? "true" : "false");
    }
    return json + '}';
}
}
