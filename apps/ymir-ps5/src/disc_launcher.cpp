// SPDX-License-Identifier: GPL-3.0-or-later
#include "disc_launcher.hpp"

#include <ymir/media/host_cd_ps5_service.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string_view>
#include <thread>
#include <vector>

namespace ps5 {

namespace {

namespace disc = ymir::media::host::ps5disc;

constexpr std::array kPayloadPaths{
    std::string_view{"/app0/tools/ymir-ps5-disc.self"},
    std::string_view{"/data/homebrew/PPSA98567/tools/ymir-ps5-disc.self"},
};
constexpr uint16 kElfLoaderPort = 9021;
constexpr std::streamsize kMaxPayloadSize = 64 * 1024 * 1024;

sockaddr_in ElfLoaderAddress() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kElfLoaderPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

std::ifstream OpenPayload() {
    for (const auto path : kPayloadPaths) {
        std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
        if (file) {
            return file;
        }
    }
    return {};
}

bool ServiceAnswers() {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }
    std::array<uint8, disc::kHello.size()> hello{};
    const auto address = disc::LocalAddress();
    const bool answered =
        disc::ConfigureSocket(fd, 1) &&
        connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
        disc::Receive(fd, hello) && hello == disc::kHello;
    close(fd);
    return answered;
}

bool SendPayloadToElfLoader(void (*report)(const char *)) {
    auto file = OpenPayload();
    if (!file) {
        report("Could not find the packaged optical service");
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size > kMaxPayloadSize) {
        report("The packaged optical service has an invalid size");
        return false;
    }
    std::vector<uint8> payload(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(payload.data()), size)) {
        report("Could not read the packaged optical service");
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        report("Could not create a socket for the local ELF loader");
        return false;
    }
    const auto address = ElfLoaderAddress();
    if (!disc::ConfigureSocket(fd, 3) ||
        connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
        report("The local ELF loader is unavailable; start the optical service manually");
        close(fd);
        return false;
    }
    const bool sent = disc::Send(fd, payload);
    close(fd);
    if (!sent) {
        report("Could not send the optical service to the local ELF loader");
        return false;
    }
    return true;
}

} // namespace

bool EnsureDiscService(void (*report)(const char *)) {
    if (ServiceAnswers()) {
        report("Optical service is already running");
        return true;
    }

    report("Optical service is not running; asking the local ELF loader to start it");
    if (!SendPayloadToElfLoader(report)) {
        return false;
    }

    for (unsigned attempt = 0; attempt < 40; ++attempt) {
        if (ServiceAnswers()) {
            report("Optical service is ready");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }

    report("The optical service started but did not answer; see /data/YmirPS5/disc-service.log");
    return false;
}

} // namespace ps5
