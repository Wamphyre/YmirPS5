// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ymir/media/host_cd.hpp>
#include <ymir/media/scsi.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <span>
#include <string_view>

namespace ymir::media::host::ps5disc {

inline constexpr std::string_view kDrivePath = "Local USB optical service";
inline constexpr std::array<uint8, 8> kHello{'Y', 'M', 'I', 'R', 'D', 'I', 'S', 1};
inline constexpr uint16 kPort = 42957;
inline constexpr uint32 kMaxBatch = 16;
inline constexpr uint32 kMaxRead = kMaxBatch * 2448;
inline constexpr size_t kMaxCDB = 12;

// Request: transfer size (BE32), CDB length (BE32), then a CDB padded to 12 bytes.
// Response: success flag (BE32), sense code (BE32), bytes transferred (BE32), then the data.
using Request = std::array<uint8, 8 + kMaxCDB>;
using Response = std::array<uint8, 12>;

DeviceHandle OpenServiceDrive(const std::string &path);
bool ExecuteRead(DeviceHandle handle, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size,
                 uint32 &sense);

inline uint32 Read32(const uint8 *data) {
    return (uint32(data[0]) << 24) | (uint32(data[1]) << 16) | (uint32(data[2]) << 8) | data[3];
}

inline void Write32(uint8 *data, uint32 value) {
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

inline sockaddr_in LocalAddress() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

inline bool ConfigureSocket(int fd, int secondsToReceive) {
    const timeval receiveTimeout{secondsToReceive, 0};
    const timeval sendTimeout{5, 0};
    const int one = 1;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout)) == 0 &&
           setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

inline bool Receive(int fd, std::span<uint8> data) {
    while (!data.empty()) {
        const auto count = recv(fd, data.data(), data.size(), 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        data = data.subspan(count);
    }
    return true;
}

inline bool Send(int fd, std::span<const uint8> data) {
    while (!data.empty()) {
        const auto count = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        data = data.subspan(count);
    }
    return true;
}

inline bool IsReadCommand(std::span<const uint8> cdb, uint32 bytes) {
    if (cdb.empty() || bytes > kMaxRead) {
        return false;
    }
    switch (cdb.front()) {
    case 0x00: {
        constexpr std::array<uint8, 6> testUnitReady{};
        return bytes == 0 && std::ranges::equal(cdb, testUnitReady);
    }
    case scsi::op::kReadTOC:
        return bytes == 2048 && std::ranges::equal(cdb, scsi::op::MakeReadTOC(bytes));
    case scsi::op::kRead10:
        return bytes == 2048 && cdb.size() == 10 &&
               std::ranges::equal(cdb, scsi::op::MakeRead10(Read32(cdb.data() + 2), 1));
    case scsi::op::kReadCD: {
        if (cdb.size() != 12) {
            return false;
        }
        const uint32 sectors = (uint32(cdb[6]) << 16) | (uint32(cdb[7]) << 8) | cdb[8];
        const uint8 format = cdb[9];
        const uint8 subchannel = cdb[10];
        return sectors >= 1 && sectors <= kMaxBatch && (format == 0xf8 || format == 0x10) && subchannel <= 1 &&
               bytes == sectors * (subchannel == 1 ? 2448 : 2352) &&
               std::ranges::equal(cdb, scsi::op::MakeReadCD(Read32(cdb.data() + 2), sectors, format, subchannel));
    }
    default: return false;
    }
}

} // namespace ymir::media::host::ps5disc
