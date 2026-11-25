// SPDX-License-Identifier: GPL-3.0-or-later

#include "cheats.hpp"

#include <ymir/sys/saturn.hpp>

#include <cmrc/cmrc.hpp>

#include <charconv>
#include <utility>

CMRC_DECLARE(ymir_ps5_assets);

namespace ps5 {

namespace {

std::string_view TakeField(std::string_view &line, char delimiter) {
    const auto end = line.find(delimiter);
    const auto field = line.substr(0, end);
    line.remove_prefix(end == line.npos ? line.size() : end + 1);
    return field;
}

bool ParseHex(std::string_view text, uint32_t &value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool IsWorkRAM(uint32_t address) {
    return (address >= 0x00200000 && address < 0x00300000) ||
           (address >= 0x06000000 && address < 0x06100000);
}

} // namespace

bool Cheats::UpdateDisc(const ymir::Saturn &saturn, bool reset) {
    const auto mediaChangeCount = saturn.GetMediaChangeCount();
    if (!reset && m_mediaChangeCount == mediaChangeCount) {
        return false;
    }
    m_mediaChangeCount = mediaChangeCount;
    m_entries.clear();
    m_enabled = 0;
    const auto &cd = saturn.GetCDInterface();
    const auto &header = cd.GetDiscHeader();
    m_hasIdentifiedDisc =
        cd.HasDisc() && header.IsValid() && !header.productNumber.empty() && !header.version.empty();
    if (m_hasIdentifiedDisc) {
        Load(header);
    }
    return true;
}

void Cheats::Load(const ymir::media::SaturnHeader &header) {
    const auto file = cmrc::ymir_ps5_assets::get_filesystem().open("cheats.tsv");
    std::string_view catalog{file.begin(), file.size()};
    if (TakeField(catalog, '\n') != "YMIRPS5_CHEATS_1") {
        return;
    }
    while (!catalog.empty()) {
        auto line = TakeField(catalog, '\n');
        if (TakeField(line, '\t') != header.productNumber || TakeField(line, '\t') != header.version ||
            TakeField(line, '\t') != header.releaseDate || TakeField(line, '\t') != header.deviceInfo) {
            continue;
        }
        uint32_t areas = 0;
        if (!ParseHex(TakeField(line, '\t'), areas) || areas != static_cast<uint32_t>(header.compatAreaCode)) {
            continue;
        }
        const auto name = TakeField(line, '\t');
        Entry entry{name, {}, {}};
        bool valid = true;
        while (!line.empty()) {
            const auto code = TakeField(line, ' ');
            uint32_t addressAndType{}, value{};
            if (code.size() != 12 || !ParseHex(code.substr(0, 8), addressAndType) ||
                !ParseHex(code.substr(8), value)) {
                valid = false;
                break;
            }
            const auto type = addressAndType >> 28;
            const auto address = addressAndType & 0x0FFFFFFF;
            const bool byte = type == 3;
            if (!IsWorkRAM(address) || (byte ? value > 0xFF : (address & 1) != 0)) {
                valid = false;
                break;
            }
            const Operation operation{address, static_cast<uint16_t>(value), byte};
            if (type == 0xD && !entry.condition && entry.writes.empty()) {
                entry.condition = operation;
            } else if (type == 1 || byte) {
                entry.writes.push_back(operation);
            } else {
                valid = false;
                break;
            }
        }
        if (valid && !name.empty() && !entry.writes.empty() && (!entry.condition || entry.writes.size() == 1)) {
            m_entries.push_back(std::move(entry));
        }
    }
}

void Cheats::Toggle(size_t index) {
    auto &entry = m_entries[index];
    entry.enabled = !entry.enabled;
    if (entry.enabled) {
        ++m_enabled;
    } else {
        --m_enabled;
    }
}

void Cheats::Apply(ymir::Saturn &saturn) const {
    if (m_enabled == 0) {
        return;
    }
    auto &master = saturn.masterSH2.GetProbe();
    auto &slave = saturn.slaveSH2.GetProbe();
    const bool updateSlaveCache = saturn.IsSH2CacheEmulationEnabled() && saturn.slaveSH2Enabled;
    for (const auto &entry : m_entries) {
        if (!entry.enabled ||
            (entry.condition && master.MemPeekWord(entry.condition->address, true) != entry.condition->value)) {
            continue;
        }
        for (const auto &write : entry.writes) {
            // The probes also update a cached copy of the address if the SH-2 already holds it.
            if (write.byte) {
                master.MemPokeByte(write.address, write.value, false);
                if (updateSlaveCache) {
                    slave.MemPokeByte(write.address, write.value, false);
                }
            } else {
                master.MemPokeWord(write.address, write.value, false);
                if (updateSlaveCache) {
                    slave.MemPokeWord(write.address, write.value, false);
                }
            }
        }
    }
}

} // namespace ps5
