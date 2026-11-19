// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 CAM transport adapted from darkxex/SwanStationPS5 src/cdscsi.c,
// which follows Al-Azif's ps-disc-detect and PS5SX2.

#include <ymir/media/host_cd.hpp>
#include <ymir/media/scsi.hpp>

#include <sys/ioctl.h>
#include <sys/types.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace ymir::media::host {

struct CAMDevice {
    CAMDevice(int fd)
        : fd(fd) {}
    ~CAMDevice() {
        close(fd);
    }

    int fd;
    path_id_t path;
    target_id_t target;
    lun_id_t lun;
    DriveState driveState = DriveState::Unknown;
    bool mediumChanged = false;
    std::chrono::steady_clock::time_point nextPoll{};
};

namespace {

    constexpr std::string_view kDrivePrefix = "/dev/cd";
    constexpr uint32 kMaxTransfer = 64 * 1024;
    std::mutex g_drivesMutex;
    std::vector<HostDriveInfo> g_drives;

    uint32 ReadSense(const ccb_scsiio &request) {
        if ((request.ccb_h.status & CAM_AUTOSNS_VALID) == 0) {
            return 0;
        }
        const auto *sense = reinterpret_cast<const uint8 *>(&request.sense_data);
        switch (sense[0] & 0x7f) {
        case 0x70:
        case 0x71: return ((sense[2] & 0x0f) << 16) | (sense[12] << 8) | sense[13];
        case 0x72:
        case 0x73: return ((sense[1] & 0x0f) << 16) | (sense[2] << 8) | sense[3];
        default: return 0;
        }
    }

    bool SendCommand(CAMDevice &device, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size,
                     uint32 &sense) {
        size = 0;
        sense = 0;
        union ccb request{};
        if (cdb.empty() || cdb.size() > sizeof(request.csio.cdb_io.cdb_bytes) || buffer.size() > kMaxTransfer) {
            return false;
        }

        cam_fill_csio(&request.csio, 1, nullptr, (buffer.empty() ? CAM_DIR_NONE : CAM_DIR_IN) | CAM_DEV_QFRZDIS,
                      MSG_SIMPLE_Q_TAG, buffer.data(), buffer.size(), SSD_FULL_SIZE, cdb.size(), 3000);
        request.ccb_h.path_id = device.path;
        request.ccb_h.target_id = device.target;
        request.ccb_h.target_lun = device.lun;
        std::memcpy(request.csio.cdb_io.cdb_bytes, cdb.data(), cdb.size());

        if (ioctl(device.fd, CAMIOCOMMAND, &request) < 0) {
            return false;
        }
        if ((request.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP || request.csio.resid > buffer.size()) {
            sense = ReadSense(request.csio);
            if ((sense >> 8) == 0x0628 || (sense >> 8) == 0x023a) {
                device.mediumChanged = true;
            }
            return false;
        }
        size = buffer.size() - request.csio.resid;
        return buffer.empty() || size != 0;
    }

} // namespace

DeviceHandle OpenCDDrive(std::string path) {
    if (!path.starts_with(kDrivePrefix)) {
        return kInvalidDeviceHandle;
    }
    unsigned unit{};
    const auto end = path.data() + path.size();
    const auto parsed = std::from_chars(path.data() + kDrivePrefix.size(), end, unit);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return kInvalidDeviceHandle;
    }

    const int xpt = open("/dev/xpt0", O_RDWR | O_CLOEXEC);
    if (xpt < 0) {
        return kInvalidDeviceHandle;
    }
    union ccb request{};
    request.ccb_h.func_code = XPT_GDEVLIST;
    std::memcpy(request.cgdl.periph_name, "cd", sizeof("cd"));
    request.cgdl.unit_number = unit;
    const int result = ioctl(xpt, CAMGETPASSTHRU, &request);
    close(xpt);
    if (result < 0 || std::strncmp(request.cgdl.periph_name, "pass", sizeof(request.cgdl.periph_name)) != 0) {
        return kInvalidDeviceHandle;
    }

    char passPath[32];
    std::snprintf(passPath, sizeof(passPath), "/dev/pass%u", request.cgdl.unit_number);
    const int fd = open(passPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return kInvalidDeviceHandle;
    }
    auto device = std::make_unique<CAMDevice>(fd);
    request = {};
    if (ioctl(fd, CAMGETPASSTHRU, &request) < 0) {
        return kInvalidDeviceHandle;
    }
    device->path = request.ccb_h.path_id;
    device->target = request.ccb_h.target_id;
    device->lun = request.ccb_h.target_lun;

    std::array<uint8, 36> inquiry{};
    uint32 size{};
    if (!SendSCSIInCommand(device.get(), scsi::op::MakeInquiry(inquiry.size()), inquiry, size) ||
        (inquiry[0] & 0x1f) != 5) {
        return kInvalidDeviceHandle;
    }
    return device.release();
}

void CloseDeviceHandle(DeviceHandle handle) {
    delete handle;
}

bool SendSCSIInCommand(DeviceHandle handle, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size) {
    if (handle->mediumChanged) {
        size = 0;
        return false;
    }
    uint32 sense{};
    return SendCommand(*handle, cdb, buffer, size, sense);
}

DriveState PollDriveState(DeviceHandle handle) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::steady_clock::now();
    if (now < handle->nextPoll && !handle->mediumChanged) {
        return handle->driveState;
    }
    handle->nextPoll = now + 250ms;

    if (handle->mediumChanged) {
        handle->mediumChanged = false;
        return handle->driveState = DriveState::NoDisc;
    }

    constexpr std::array<uint8, 6> testUnitReady{};
    uint32 size{}, sense{};
    if (SendCommand(*handle, testUnitReady, {}, size, sense)) {
        return handle->driveState = DriveState::MediaPresent;
    }

    handle->mediumChanged = false;
    return handle->driveState = sense == 0x023a02 ? DriveState::TrayOpen : DriveState::NoDisc;
}

std::vector<HostDriveInfo> EnumerateHostCDDrives() {
    std::vector<HostDriveInfo> drives;
    for (unsigned unit = 0; unit < 16; ++unit) {
        std::string path = std::string(kDrivePrefix) + std::to_string(unit);
        struct stat status{};
        if (stat(path.c_str(), &status) < 0 || !S_ISCHR(status.st_mode)) {
            continue;
        }
        if (std::unique_ptr<CAMDevice> device{OpenCDDrive(path)}) {
            drives.push_back({std::move(path), {}, PollDriveState(device.get())});
        }
    }
    const std::lock_guard lock{g_drivesMutex};
    return g_drives = std::move(drives);
}

std::vector<HostDriveInfo> GetEnumeratedHostCDDrives() {
    const std::lock_guard lock{g_drivesMutex};
    return g_drives;
}

} // namespace ymir::media::host
