#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace localflow::core {

// Splits UTF-8 text at extended grapheme boundaries while measuring the limit
// in UTF-16 code units, matching the insertion contract used by LocalFlow.
// A single grapheme that exceeds the limit is emitted intact.
[[nodiscard]] std::vector<std::string> chunk_text_utf16(
    const std::string& text,
    std::size_t max_utf16_units_per_chunk = 20);

[[nodiscard]] std::size_t utf16_length(const std::string& utf8_text) noexcept;

}  // namespace localflow::core

