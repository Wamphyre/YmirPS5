// SPDX-License-Identifier: GPL-3.0-or-later
// The unlock request follows SwanStationPS5's ps5_unlock.c (GPL-3.0-or-later):
// etaHEN, OnionHEN, LegacyJB and PS5SX2 Helper jailbreak a running title when
// it publishes {"PID":<pid>} under /download0. If none answers, the packaged
// Lapy one-shot helper performs the same elevation through the ELF loader.
// Either route gives this process a private root credential, which exposes
// /dev and the CAM nodes of a USB optical drive.
#include "unlock.hpp"

#include "lapy.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

namespace ps5 {

namespace {

constexpr const char *kRequests[]{
    "/download0/etahen_jailbreak",
    "/download0/onionhen_jailbreak",
};
constexpr const char *kCrashMarker = "/download0/YmirPS5_unlock_attempt";
constexpr unsigned kPollCount = 60;
constexpr auto kPollInterval = std::chrono::milliseconds{16};

std::ofstream g_log;

void Log(const char *message) {
    if (g_log) {
        g_log << message << '\n';
        g_log.flush();
    }
}

bool DataListable() {
    DIR *directory = opendir("/data");
    if (directory == nullptr) {
        return false;
    }
    closedir(directory);
    return true;
}

bool DevicesVisible() {
    return access("/dev/xpt0", F_OK) == 0;
}

bool Unlocked() {
    return DataListable() && DevicesVisible();
}

bool WaitUnlocked() {
    for (unsigned poll = 0; poll < kPollCount; ++poll) {
        if (Unlocked()) {
            return true;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    return false;
}

bool Publish(const char *path, int pid) {
    char staged[80];
    std::snprintf(staged, sizeof(staged), "%s.tmp", path);
    unlink(staged);
    unlink(path);
    const int fd = open(staged, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        return false;
    }
    fchmod(fd, 0666);
    char body[32];
    const int length = std::snprintf(body, sizeof(body), "{\"PID\":%d}\n", pid);
    const bool written = length > 0 && write(fd, body, size_t(length)) == length && fsync(fd) == 0;
    close(fd);
    if (!written || rename(staged, path) != 0) {
        unlink(staged);
        return false;
    }
    return true;
}

// etaHEN's legacy command server, when the file listener is not configured.
struct HijackerCommand {
    int magic = 0xDEADBEEF;
    int command = 5; // JAILBREAK_CMD
    int pid = 0;
    int result = -1337;
    char message1[0x500]{};
    char message2[0x500]{};
};

bool RequestOverTcp(int pid) {
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return false;
    }
    const timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(9028);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#if defined(__PROSPERO__)
    address.sin_len = sizeof(address);
#endif

    bool answered = false;
    if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0) {
        HijackerCommand command{};
        command.pid = pid;
        if (send(fd, &command, sizeof(command), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(command))) {
            size_t received = 0;
            while (received < sizeof(command)) {
                const auto count = recv(fd, reinterpret_cast<char *>(&command) + received,
                                        sizeof(command) - received, 0);
                if (count <= 0) {
                    break;
                }
                received += size_t(count);
            }
            answered = received >= 16 && (command.result == 0 || command.result == -1337);
        }
    }
    close(fd);
    return answered;
}

UnlockStatus RequestEtaHen() {
    if (seteuid(geteuid()) != 0) {
        Log("etaHEN: could not clone this process's credential");
        return UnlockStatus::CannotRequest;
    }
    if (access(kCrashMarker, F_OK) == 0) {
        // A previous launch died while asking. Skip one launch to avoid a loop.
        unlink(kCrashMarker);
        Log("etaHEN: skipped once after the previous request closed the app");
        return UnlockStatus::SkippedAfterCrash;
    }
    const int marker = open(kCrashMarker, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (marker >= 0) {
        fsync(marker);
        close(marker);
    }

    const int pid = static_cast<int>(getpid());
    UnlockStatus result = UnlockStatus::NoAnswer;
    for (unsigned round = 0; round < 3 && result != UnlockStatus::Granted; ++round) {
        unsigned published = 0;
        for (const char *path : kRequests) {
            published += Publish(path, pid) ? 1 : 0;
        }
        if (published == 0) {
            result = UnlockStatus::CannotRequest;
            break;
        }
        if (WaitUnlocked()) {
            result = UnlockStatus::Granted;
        } else if (round == 0 && RequestOverTcp(pid) && WaitUnlocked()) {
            result = UnlockStatus::Granted;
        }
        for (const char *path : kRequests) {
            unlink(path);
        }
    }
    unlink(kCrashMarker);
    return result;
}

} // namespace

UnlockStatus UnlockSandbox() {
    mkdir("/data/YmirPS5", 0777);
    g_log.open("/data/YmirPS5/unlock.log", std::ios::trunc);

    if (Unlocked()) {
        Log("Sandbox: already unlocked");
        return UnlockStatus::AlreadyUnlocked;
    }
    Log("Sandbox: /dev is hidden; requesting an on-demand jailbreak from etaHEN");

    UnlockStatus result = RequestEtaHen();
    if (result == UnlockStatus::Granted) {
        Log("etaHEN: jailbreak granted");
        return result;
    }
    Log(("etaHEN: " + std::string(DescribeUnlock(result))).c_str());

    Log("Lapy: trying the packaged owned-root helper through the local ELF loader");
    const auto lapy = LapyElevate(Log);
    if (lapy == LapyStatus::Granted) {
        if (WaitUnlocked()) {
            Log("Lapy: elevation granted");
            return UnlockStatus::LapyGranted;
        }
        Log("Lapy: elevation granted but /dev is still hidden");
    } else {
        Log((std::string("Lapy: ") + DescribeLapy(lapy)).c_str());
    }
    return UnlockStatus::NoAnswer;
}

const char *DescribeUnlock(UnlockStatus status) {
    switch (status) {
    case UnlockStatus::AlreadyUnlocked: return "already unlocked";
    case UnlockStatus::Granted: return "granted by etaHEN";
    case UnlockStatus::LapyGranted: return "granted by Lapy";
    case UnlockStatus::NoAnswer: return "no unlock service answered";
    case UnlockStatus::CannotRequest: return "could not write the unlock request";
    case UnlockStatus::SkippedAfterCrash: return "skipped once after the previous request closed the app";
    }
    return "unknown";
}

} // namespace ps5
