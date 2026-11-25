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

    bool UpdateDisc(const ymir::Saturn &saturn, bool reset = false);
    void Toggle(size_t index);
    void Apply(ymir::Saturn &saturn) const;

    bool HasIdentifiedDisc() const {
        return m_hasIdentifiedDisc;
    }

    std::span<const Entry> Entries() const {
        return m_entries;
    }

    size_t EnabledCount() const {
        return m_enabled;
    }

private:
    void Load(const ymir::media::SaturnHeader &header);

    std::vector<Entry> m_entries;
    uint64_t m_mediaChangeCount = 0;
    size_t m_enabled = 0;
    bool m_hasIdentifiedDisc = false;
};

} // namespace ps5
