// SPDX-License-Identifier: GPL-3.0-or-later
// Client for the Lapy owned-root one-shot helper. The helper ELF was built
// for PPSA98567 with the pinned upstream PS5-Lapy-JB-Daemon source used by
// SwanStationPS5. Only the filesystem capability is requested; the resulting
// private root credential is what exposes /dev and the CAM device nodes.
#include "lapy.hpp"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cerrno>
#include <fstream>
#include <string_view>
#include <vector>

namespace ps5 {

namespace {

constexpr std::array kHelperPaths{
    std::string_view{"/app0/lapy.elf"},
    std::string_view{"/data/homebrew/PPSA98567/lapy.elf"},
};
constexpr std::uint16_t kElfLoaderPort = 9021;
constexpr std::streamsize kMaxHelperSize = 4 * 1024 * 1024;
constexpr std::uint32_t kMagic = 0x31564C45; // "ELV1"
constexpr std::uint16_t kVersion = 1;

enum class Kind : std::uint32_t {
    Request = 1,
    Prepare = 2,
    Prepared = 3,
    Response = 4,
};

enum class Status : std::uint32_t {
    Ok = 0,
    InvalidRequest = 1,
    UnsupportedVersion = 2,
    UnsupportedCapability = 3,
    TargetMismatch = 4,
    Unavailable = 5,
    PrepareFailed = 6,
    ApplyFailed = 7,
    RollbackFailed = 8,
    TransportError = 9,
    ProtocolError = 10,
    Timeout = 11,
};

struct Message {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t size = sizeof(Message);
    Kind kind = Kind::Request;
    std::uint32_t capability = 1; // filesystem
    std::uint32_t pid = 0;
    Status status = Status::Ok;
};
static_assert(sizeof(Message) == 24);

sockaddr_in ElfLoaderAddress() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kElfLoaderPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

bool SendAll(int fd, const void *data, size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    while (size != 0) {
        const auto count = send(fd, bytes, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= size_t(count);
    }
    return true;
}

bool ReceiveAll(int fd, void *data, size_t size) {
    auto *bytes = static_cast<std::uint8_t *>(data);
    while (size != 0) {
        const auto count = recv(fd, bytes, size, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes += count;
        size -= size_t(count);
    }
    return true;
}

bool Valid(const Message &message) {
    return message.magic == kMagic && message.version == kVersion && message.size == sizeof(Message) &&
           message.pid > 1 && message.capability == 1;
}

bool Matches(const Message &message, const Message &request, Kind kind) {
    return Valid(message) && message.kind == kind && message.pid == request.pid &&
           message.capability == request.capability;
}

bool ReadHelper(std::vector<std::uint8_t> &helper) {
    for (const auto path : kHelperPaths) {
        std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
        if (!file) {
            continue;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0 || size > kMaxHelperSize) {
            return false;
        }
        helper.resize(size_t(size));
        file.seekg(0);
        return bool(file.read(reinterpret_cast<char *>(helper.data()), size));
    }
    return false;
}

} // namespace

bool LapyElevate() {
    std::vector<std::uint8_t> helper;
    if (!ReadHelper(helper)) {
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }
    const timeval receiveTimeout{15, 0};
    const timeval sendTimeout{10, 0};
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const auto address = ElfLoaderAddress();
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
        close(fd);
        return false;
    }

    Message request{};
    request.pid = std::uint32_t(getpid());
    if (!SendAll(fd, helper.data(), helper.size()) || !SendAll(fd, &request, sizeof(request))) {
        close(fd);
        return false;
    }

    Message reply{};
    if (!ReceiveAll(fd, &reply, sizeof(reply))) {
        close(fd);
        return false;
    }
    if (Matches(reply, request, Kind::Response)) {
        close(fd);
        return reply.status == Status::Ok;
    }
    if (!Matches(reply, request, Kind::Prepare) || reply.status != Status::Ok) {
        close(fd);
        return false;
    }

    // The helper requires the target to clone its own credential now.
    Message prepared = request;
    prepared.kind = Kind::Prepared;
    prepared.status = seteuid(geteuid()) == 0 ? Status::Ok : Status::PrepareFailed;
    if (!SendAll(fd, &prepared, sizeof(prepared))) {
        close(fd);
        return false;
    }

    Message response{};
    if (!ReceiveAll(fd, &response, sizeof(response))) {
        close(fd);
        return false;
    }
    close(fd);
    return Matches(response, request, Kind::Response) && prepared.status == Status::Ok && response.status == Status::Ok;
}

} // namespace ps5
