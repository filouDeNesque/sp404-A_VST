#include "sp404/Bank.h"

namespace sp404 {

std::string Pad::label() const {
    return std::string(1, bank) + std::to_string(indexInBank);
}

} // namespace sp404
