// SPDX-License-Identifier: GPL-3.0-or-later
#include "ymir/media/host_cd_ps5_service.hpp"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace host = ymir::media::host;
namespace disc = host::ps5disc;

namespace {

void Report(const char *message) {
    if (auto *file = std::fopen("/data/YmirPS5/disc-service.log", "a")) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    } else {
        std::fprintf(stderr, "YmirPS5 optical service: %s\n", message);
    }
}

void Serve(int client, host::DeviceHandle drive) {
    if (!disc::ConfigureSocket(client, 300) || !disc::Send(client, disc::kHello)) {
        return;
    }

    std::array<uint8, disc::kMaxRead> data;
    disc::Request request{};
    while (disc::Receive(client, request)) {
        const uint32 bytes = disc::Read32(request.data());
        const uint32 length = disc::Read32(request.data() + 4);
        if (length > disc::kMaxCDB || bytes > data.size()) {
            return;
        }
        const auto cdb = std::span{request}.subspan(8, length);
        if (!disc::IsReadCommand(cdb, bytes)) {
            return;
        }

        const auto output = std::span{data}.first(bytes);
        std::ranges::fill(output, 0);
        uint32 size{}, sense{};
        const bool success = disc::ExecuteRead(drive, cdb, output, size, sense);
        disc::Response response{};
        disc::Write32(response.data(), success);
        disc::Write32(response.data() + 4, sense);
        disc::Write32(response.data() + 8, success ? size : 0);
        if (!disc::Send(client, response) || (success && !disc::Send(client, output.first(size)))) {
            return;
        }
    }
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc > 2) {
        std::fprintf(stderr, "Usage: ymir-ps5-disc.self [/dev/cdN or /dev/passN]\n");
        return 1;
    }
    const std::string path = argc == 2 ? argv[1] : "";
    host::SetCDDriveLogCallback(Report);
    Report("Checking for optical CAM access");
    auto drive = disc::OpenServiceDrive(path);
    if (drive == host::kInvalidDeviceHandle) {
        Report("Could not open an optical drive in the service's environment");
        return 1;
    }

    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener < 0) {
        Report(std::strerror(errno));
        host::CloseDeviceHandle(drive);
        return 1;
    }
    const auto address = disc::LocalAddress();
    const int one = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0 ||
        bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0 || listen(listener, 2) < 0) {
        Report(std::strerror(errno));
        close(listener);
        host::CloseDeviceHandle(drive);
        return 1;
    }
    Report("Ready for YmirPS5 on loopback");

    for (;;) {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            Report(std::strerror(errno));
            break;
        }
        if (drive == host::kInvalidDeviceHandle) {
            drive = disc::OpenServiceDrive(path);
        }
        if (drive != host::kInvalidDeviceHandle) {
            Serve(client, drive);
            host::CloseDeviceHandle(drive);
            drive = host::kInvalidDeviceHandle;
        }
        close(client);
    }
    close(listener);
    if (drive != host::kInvalidDeviceHandle) {
        host::CloseDeviceHandle(drive);
    }
    return 1;
}
