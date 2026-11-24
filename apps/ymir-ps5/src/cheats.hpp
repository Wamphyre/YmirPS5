// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ymir {
struct Saturn;
namespace media {
struct SaturnHeader;
}
} // namespace ymir

namespace ps5 {

class Cheats {
    struct Operation {
        uint32_t address;
        uint16_t value;
        bool byte;
    };

public:
    struct Entry {
        std::string_view name;
        std::optional<Operation> condition;
        std::vector<Operation> writes;
        bool enabled = false;
    };

    void Load(const ymir::media::SaturnHeader *header);
    void Toggle(size_t index);
    void Apply(ymir::Saturn &saturn) const;

    std::span<const Entry> Entries() const {
        return m_entries;
    }

    size_t EnabledCount() const {
        return m_enabled;
    }

private:
    std::vector<Entry> m_entries;
    size_t m_enabled = 0;
};

} // namespace ps5
