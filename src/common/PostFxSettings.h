#pragma once
#include <string>
#include <vector>

namespace DXL {
struct PostFxSetting {
    std::string file;
    bool enabled = false;
    std::vector<float> values; // Parameter order is part of each file's public contract.
};
}
