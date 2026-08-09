#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

#include "sp404/PadInfo.h"

namespace sp404 {

struct Pad {
    char bank = 'A';       // 'A'..'J'
    int indexInBank = 1;   // 1..12
    PadInfo info;
    std::optional<std::filesystem::path> samplePath;

    // e.g. "A1", "J12"
    std::string label() const;
};

struct Bank {
    static constexpr int padCount = 12;

    char name = 'A';
    std::array<Pad, padCount> pads;
};

} // namespace sp404
