// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 CAM transport adapted from darkxex/SwanStationPS5 src/cdscsi.c,
// which follows Al-Azif's ps-disc-detect and PS5SX2.

#include <ymir/media/host_cd.hpp>
#include <ymir/media/scsi.hpp>

#include "host_cd_ps5_service.hpp"

#include <sys/ioctl.h>
#include <sys/cdio.h>
#include <sys/types.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <unistd.h>

namespace ymir::media::host {

struct CAMDevice {
    CAMDevice(int fd)
        : fd(fd) {}
    ~CAMDevice() {
        if (cdFd >= 0) {
            close(cdFd);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    int fd;
    int cdFd = -1;
    std::string passPath;
    path_id_t path{};
    target_id_t target{};
    lun_id_t lun{};
    bool service = false;
    DriveState driveState = DriveState::Unknown;
    bool mediumChanged = false;
    std::chrono::steady_clock::time_point nextPoll{};
};

namespace {

    constexpr std::string_view kDrivePrefix = "/dev/cd";
    constexpr std::string_view kPassPrefix = "/dev/pass";
    constexpr uint32 kMaxTransfer = 64 * 1024;
    std::mutex g_drivesMutex;
    std::vector<HostDriveInfo> g_drives;

    DeviceHandle OpenService() {
        const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd < 0) {
            return kInvalidDeviceHandle;
        }
        auto connection = std::make_unique<CAMDevice>(fd);
        const auto address = ps5disc::LocalAddress();
        if (!ps5disc::ConfigureSocket(fd, 15) ||
            connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
            return kInvalidDeviceHandle;
        }
        std::array<uint8, ps5disc::kHello.size()> hello{};
        if (!ps5disc::Receive(fd, hello) || hello != ps5disc::kHello) {
            return kInvalidDeviceHandle;
        }
        connection->service = true;
        return connection.release();
    }

    bool ReadThroughService(CAMDevice &device, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size,
                            uint32 &sense) {
        if (device.fd < 0 || !ps5disc::IsSupportedCommand(cdb, buffer.size())) {
            return false;
        }
        ps5disc::Request request{};
        ps5disc::Write32(request.data(), buffer.size());
        ps5disc::Write32(request.data() + 4, cdb.size());
        std::ranges::copy(cdb, request.begin() + 8);

        ps5disc::Response response{};
        if (ps5disc::Send(device.fd, request) && ps5disc::Receive(device.fd, response)) {
            const auto success = ps5disc::Read32(response.data());
            const auto replySense = ps5disc::Read32(response.data() + 4);
            const auto replySize = ps5disc::Read32(response.data() + 8);
            if (replySense <= 0xFFFFFF &&
                ((success == 0 && replySize == 0) ||
                 (success == 1 && replySense == 0 && replySize <= buffer.size() &&
                  (buffer.empty() || replySize != 0))) &&
                ps5disc::Receive(device.fd, buffer.first(replySize))) {
                size = replySize;
                sense = replySense;
                return success == 1;
            }
        }
        close(device.fd);
        device.fd = -1;
        return false;
    }

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

    // Convert the cd(4) TOC into the standard MMC reply consumed by Ymir.
    bool ReadDeviceTOC(int fd, std::span<uint8> buffer, uint32 &size) {
        ioc_toc_header header{};
        if (ioctl(fd, CDIOREADTOCHEADER, &header) < 0 || header.starting_track == 0 ||
            header.ending_track < header.starting_track || header.ending_track > 99) {
            return false;
        }
        const size_t count = header.ending_track - header.starting_track + 2; // includes lead-out
        const size_t total = 4 + count * 8;
        if (buffer.size() < total) {
            return false;
        }
        std::array<cd_toc_entry, 100> entries{};
        ioc_read_toc_entry request{};
        request.address_format = CD_LBA_FORMAT;
        request.starting_track = header.starting_track;
        request.data_len = count * sizeof(cd_toc_entry);
        request.data = entries.data();
        if (ioctl(fd, CDIOREADTOCENTRYS, &request) < 0) {
            return false;
        }
        std::fill_n(buffer.begin(), total, 0);
        buffer[0] = (total - 2) >> 8;
        buffer[1] = (total - 2) & 0xff;
        buffer[2] = header.starting_track;
        buffer[3] = header.ending_track;
        for (size_t i = 0; i < count; ++i) {
            const auto &entry = entries[i];
            if (entry.track != (i + 1 == count ? 0xaa : header.starting_track + i)) {
                return false;
            }
            const auto offset = 4 + 8 * i;
            buffer[offset + 1] = (entry.addr_type << 4) | entry.control;
            buffer[offset + 2] = entry.track;
            std::memcpy(buffer.data() + offset + 4, entry.addr.addr, 4); // already big endian
        }
        size = total;
        return true;
    }

    bool SendCAMCommand(CAMDevice &device, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size,
                        uint32 &sense) {
        union ccb request{};
        if (cdb.empty() || cdb.size() > sizeof(request.csio.cdb_io.cdb_bytes) || buffer.size() > kMaxTransfer) {
            return false;
        }

        if (device.cdFd >= 0 && cdb[0] == scsi::op::kReadTOC &&
            std::ranges::equal(cdb, scsi::op::MakeReadTOC(buffer.size())) && ReadDeviceTOC(device.cdFd, buffer, size)) {
            return true;
        }
        if (device.cdFd >= 0 && cdb[0] == scsi::op::kRead10 && cdb.size() == 10 && buffer.size() == 2048) {
            const auto lba = ps5disc::Read32(cdb.data() + 2);
            if (std::ranges::equal(cdb, scsi::op::MakeRead10(lba, 1)) &&
                pread(device.cdFd, buffer.data(), buffer.size(), static_cast<off_t>(lba) * 2048) == 2048) {
                size = 2048;
                return true;
            }
        }

        const unsigned timeout = cdb[0] == scsi::op::kReadTOC ? 10000 : 3000;
        cam_fill_csio(&request.csio, 1, nullptr, (buffer.empty() ? CAM_DIR_NONE : CAM_DIR_IN) | CAM_DEV_QFRZDIS,
                      MSG_SIMPLE_Q_TAG, buffer.data(), buffer.size(), SSD_FULL_SIZE, cdb.size(), timeout);
        request.ccb_h.path_id = device.path;
        request.ccb_h.target_id = device.target;
        request.ccb_h.target_lun = device.lun;
        std::memcpy(request.csio.cdb_io.cdb_bytes, cdb.data(), cdb.size());

        if (ioctl(device.fd, CAMIOCOMMAND, &request) < 0) {
            return false;
        }
        if ((request.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP || request.csio.resid > buffer.size()) {
            sense = ReadSense(request.csio);
            return false;
        }
        size = buffer.size() - request.csio.resid;
        return buffer.empty() || size != 0;
    }

    bool SendCommand(CAMDevice &device, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size,
                     uint32 &sense) {
        size = 0;
        sense = 0;
        const bool ok = device.service ? ReadThroughService(device, cdb, buffer, size, sense)
                                       : SendCAMCommand(device, cdb, buffer, size, sense);
        if (!ok && ((sense >> 8) == 0x0628 || (sense >> 8) == 0x023a)) {
            device.mediumChanged = true;
        }
        return ok;
    }

    DeviceHandle OpenPass(const std::string &path, const ccb_hdr *address = nullptr) {
        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
            fd = open(path.c_str(), O_RDONLY);
        }
        if (fd < 0) {
            return kInvalidDeviceHandle;
        }
        auto device = std::make_unique<CAMDevice>(fd);
        union ccb request{};
        if (!address && ioctl(fd, CAMGETPASSTHRU, &request) < 0) {
            return kInvalidDeviceHandle;
        }
        const auto &cam = address ? *address : request.ccb_h;
        device->path = cam.path_id;
        device->target = cam.target_id;
        device->lun = cam.target_lun;

        std::array<uint8, 36> inquiry{};
        uint32 size{};
        if (!SendSCSIInCommand(device.get(), scsi::op::MakeInquiry(inquiry.size()), inquiry, size) ||
            size < 32 || (inquiry[0] & 0x1f) != 5) {
            return kInvalidDeviceHandle;
        }
        const std::string_view model{reinterpret_cast<const char *>(inquiry.data() + 8), 24};
        if (model.find("PS-SYSTEM") != std::string_view::npos) {
            return kInvalidDeviceHandle;
        }
        device->passPath = path;
        return device.release();
    }

} // namespace

// Resolve the cd unit on the CAM transport, as SwanStation does, before trying cd(4).
static DeviceHandle OpenCDUnit(const std::string &path, unsigned unit, int xpt) {
    union ccb request{};
    request.ccb_h.func_code = XPT_GDEVLIST;
    std::memcpy(request.cgdl.periph_name, "cd", sizeof("cd"));
    request.cgdl.unit_number = unit;
    if (xpt >= 0 && ioctl(xpt, CAMGETPASSTHRU, &request) == 0 && request.cgdl.status != CAM_GDEVLIST_ERROR &&
        std::strncmp(request.cgdl.periph_name, "pass", sizeof(request.cgdl.periph_name)) == 0) {
        const auto passPath = std::string(kPassPrefix) + std::to_string(request.cgdl.unit_number);
        if (auto *device = OpenPass(passPath)) {
            return device;
        }
    }
    const int cd = open(path.c_str(), O_RDONLY);
    if (cd >= 0) {
        union ccb mapping{};
        mapping.ccb_h.func_code = XPT_GDEVLIST;
        if (ioctl(cd, CAMGETPASSTHRU, &mapping) == 0 && mapping.cgdl.status != CAM_GDEVLIST_ERROR &&
            std::strncmp(mapping.cgdl.periph_name, "pass", sizeof(mapping.cgdl.periph_name)) == 0) {
            const auto passPath = std::string(kPassPrefix) + std::to_string(mapping.cgdl.unit_number);
            if (auto *device = OpenPass(passPath, &mapping.ccb_h)) {
                device->cdFd = cd;
                return device;
            }
        }
        close(cd);
    }
    return kInvalidDeviceHandle;
}

static DeviceHandle OpenCAMDrive(const std::string &path) {
    const auto prefix = path.starts_with(kPassPrefix) ? kPassPrefix : kDrivePrefix;
    if (!path.starts_with(prefix)) {
        return kInvalidDeviceHandle;
    }
    unsigned unit{};
    const auto end = path.data() + path.size();
    const auto parsed = std::from_chars(path.data() + prefix.size(), end, unit);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return kInvalidDeviceHandle;
    }
    if (prefix == kPassPrefix) {
        return OpenPass(path);
    }
    const int xpt = open("/dev/xpt0", O_RDWR);
    auto *device = OpenCDUnit(path, unit, xpt);
    if (xpt >= 0) {
        close(xpt);
    }
    return device;
}

DeviceHandle OpenCDDrive(std::string path) {
    return path == ps5disc::kDrivePath ? OpenService() : OpenCAMDrive(path);
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
    // Polling more often than this interrupts USB transfers and buys nothing:
    // disc removal is also reported by the read path.
    handle->nextPoll = now + 1000ms;

    if (handle->fd < 0 || handle->mediumChanged) {
        handle->mediumChanged = false;
        return handle->driveState = DriveState::NoDisc;
    }

    constexpr std::array<uint8, 6> testUnitReady{};
    uint32 size{}, sense{};
    if (SendCommand(*handle, testUnitReady, {}, size, sense)) {
        return handle->driveState = DriveState::MediaPresent;
    }

    handle->mediumChanged = false;
    if (handle->fd < 0) {
        return handle->driveState = DriveState::NoDisc;
    }
    if ((sense >> 8) == 0x023a) {
        return handle->driveState = sense == 0x023a02 ? DriveState::TrayOpen : DriveState::NoDisc;
    }
    // Spin-up, unit attention and transport failures do not report an empty tray.
    return handle->driveState = DriveState::Unknown;
}

static std::vector<HostDriveInfo> EnumerateCAMDrives() {
    std::vector<HostDriveInfo> drives;
    const int xpt = open("/dev/xpt0", O_RDWR);
    for (unsigned unit = 0; unit < 32; ++unit) {
        std::string path = std::string(kDrivePrefix) + std::to_string(unit);
        if (std::unique_ptr<CAMDevice> device{OpenCDUnit(path, unit, xpt)}) {
            drives.push_back({std::move(path), device->passPath, PollDriveState(device.get())});
        }
    }
    if (xpt >= 0) {
        close(xpt);
    }
    // Some USB bridges register a pass node without a cd unit, even when another cd unit exists.
    for (unsigned unit = 0; unit < 32; ++unit) {
        std::string path = std::string(kPassPrefix) + std::to_string(unit);
        if (std::ranges::find(drives, path, &HostDriveInfo::altPath) != drives.end()) {
            continue;
        }
        if (std::unique_ptr<CAMDevice> device{OpenPass(path)}) {
            drives.push_back({std::move(path), {}, PollDriveState(device.get())});
        }
    }
    return drives;
}

namespace ps5disc {

DeviceHandle OpenServiceDrive(const std::string &path) {
    if (!path.empty()) {
        return OpenCAMDrive(path);
    }
    const auto drives = EnumerateCAMDrives();
    for (const auto &drive : drives) {
        std::unique_ptr<CAMDevice> device{OpenCAMDrive(drive.path)};
        if (!device) {
            continue;
        }
        union ccb inquiry{};
        inquiry.ccb_h.func_code = XPT_PATH_INQ;
        inquiry.ccb_h.path_id = device->path;
        inquiry.ccb_h.target_id = device->target;
        inquiry.ccb_h.target_lun = device->lun;
        const bool isUSB = ioctl(device->fd, CAMIOCOMMAND, &inquiry) == 0 &&
                           (inquiry.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP &&
                           inquiry.cpi.transport == XPORT_USB;
        if (isUSB || (drives.size() == 1 && drive.driveState == DriveState::MediaPresent)) {
            return device.release();
        }
    }
    return kInvalidDeviceHandle;
}

bool ExecuteRead(DeviceHandle handle, std::span<const uint8> cdb, std::span<uint8> buffer, uint32 &size,
                 uint32 &sense) {
    size = 0;
    sense = 0;
    return IsSupportedCommand(cdb, buffer.size()) && SendCAMCommand(*handle, cdb, buffer, size, sense);
}

} // namespace ps5disc

std::vector<HostDriveInfo> EnumerateHostCDDrives() {
    auto drives = EnumerateCAMDrives();
    // Keep the service as a last-resort entry even when a CAM unit exists. The
    // title's sandbox can make the unit visible but still deny opening it, while
    // the service runs with the permissions of the process that loaded it.
    if (std::unique_ptr<CAMDevice> service{OpenService()}) {
        drives.push_back({std::string(ps5disc::kDrivePath), {}, PollDriveState(service.get())});
    }
    const std::lock_guard lock{g_drivesMutex};
    return g_drives = std::move(drives);
}

std::vector<HostDriveInfo> GetEnumeratedHostCDDrives() {
    const std::lock_guard lock{g_drivesMutex};
    return g_drives;
}

} // namespace ymir::media::host
