#include <ymir/media/cd_device/cd_device_host.hpp>

#include <ymir/media/cd_utils.hpp>
#include <ymir/media/scsi.hpp>

#include <ymir/util/arith_ops.hpp>
#include <ymir/util/dev_log.hpp>
#include <ymir/util/thread_name.hpp>

// Async design notes:
// - TOC and SaturnHeader (collectively "disc info") are protected by a mutex
// - Worker thread keeps its own copy of the drive state as well as the TOC and SaturnHeader
// - An atomic flag is set whenever media is changed, either by detecting a drive state change during the polling loop
//   or through a notification
// - Another atomic flag is set once the TOC and SaturnHeader finish updating
// - PollDriveState() updates the drive state and copies over the TOC and SaturnHeader if changed.
//   - If the media presence changed or the dedicated flag is set, the OnMediaChanged callback is invoked.
// - PollDriveState() is invoked as part of the emulation loop. If the emulator is paused, any state changes will be
//   held pending until emulation resumes
// - The frontend may register a callback to receive immediate notifications on device state changes

namespace ymir::media {

namespace {

    constexpr uint32 kRawSectorSize = 2352;
    constexpr uint32 kRawSectorWithSubchannel = kRawSectorSize + 96;

    // Read enough sectors during a seek to cover the first few CD Block ticks.
    // The seek is reported as done only after this read completes, so the host
    // seek latency is absorbed by the emulated seek instead of blocking the
    // emulation thread on the first Play tick.
    constexpr uint32 kSeekPrimeSectors = 8;

#if defined(__PROSPERO__)
    // MMC READ CD returns the on-disc header and (for XA) the subheader before the payload.
    std::span<uint8> SectorUserData(std::span<uint8, kRawSectorSize> sector) {
        if (HasValidSyncBytes(sector)) {
            if (sector[15] == 1) {
                return sector.subspan<16, 2048>();
            }
            if (sector[15] == 2 && (sector[18] & 0x20) == 0) { // Mode 2 Form 1
                return sector.subspan<24, 2048>();
            }
        }
        return {};
    }
#endif

} // namespace

namespace grp {

    // -----------------------------------------------------------------------------
    // Dev log groups

    // Hierarchy:
    //
    // host
    //   cache

    struct host {
        static constexpr bool enabled = true;
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "CDDev-Host";
    };

    struct cache : public host {
        static constexpr devlog::Level level = devlog::level::debug;
        static constexpr std::string_view name = "CDDev-Host-Cache";
    };

} // namespace grp

HostCDDevice::HostCDDevice(std::string path, std::chrono::milliseconds timeout,
                           const CBOnMediaChanged &cbOnMediaChanged)
    : m_cbOnMediaChanged(cbOnMediaChanged)
    , m_path(path) {
    m_devHandle = host::OpenCDDrive(path);
    if (m_devHandle == host::kInvalidDeviceHandle) {
        return;
    }

    // TODO: subscribe to notifications for device and media state changes
    // - if media changed, mark disc info as dirty
    // - notify drive removal somehow

    m_workerThread = std::thread{[this] { WorkerThread(); }};

    // Wait for the specified amount of time for device to be ready
    using namespace std::chrono_literals;
    using clk = std::chrono::steady_clock;
    const auto deadline = clk::now() + timeout;
    while (clk::now() < deadline) {
        if (PollDriveState() != DriveState::Unknown) {
            break;
        }
        // Wait between attempts to avoid hammering the CPU
        std::this_thread::sleep_for(10ms);
    }

    if (m_driveState == DriveState::Unknown) {
        // Timed out
        Disconnect();
    }
}

HostCDDevice::~HostCDDevice() {
    Disconnect();
}

bool HostCDDevice::IsConnected() const {
    return m_devHandle != host::kInvalidDeviceHandle;
}

bool HostCDDevice::ReadPosition(uint32 frameAddress, DiscPosition &outPosition) {
    auto entry = GetCachedSector(frameAddress);
    if (entry == nullptr) {
        return false;
    }

    outPosition = entry->pos;
    return true;
}

bool HostCDDevice::IsSeekDone() const {
    return m_seekState.committedCount == m_seekState.requestedCount;
}

uint32 HostCDDevice::GetSeekFrameAddress() const {
    return m_seekState.frameAddress;
}

void HostCDDevice::HintStop() {
    EnqueueCommand(Command::StopPrefetcher());
}

DriveState HostCDDevice::PollDriveStateImpl() {
    auto &ts = m_threadState;

    if (ts.discInfoChanged.load(std::memory_order_acquire)) {
        std::unique_lock lock{ts.mtxDiscInfo};

        m_toc = ts.toc;
        m_header = ts.header;
        m_fs = ts.fs;
        m_driveState = ts.targetDriveState;
        ts.discInfoChanged.store(false, std::memory_order_release);
        lock.unlock();
        m_sectorCache.Flush();
        m_cbOnMediaChanged();
    }

    // Acquire the completion counter before reading the result it publishes.
    m_seekState.committedCount = ts.seekCounter.load(std::memory_order_acquire);
    m_seekState.frameAddress = ts.seekFAD.load(std::memory_order_relaxed);

    return m_driveState;
}

uint32 HostCDDevice::ReadSectorImpl(uint32 frameAddress, std::span<uint8, 2352> outSector, DiscPosition *outPosition) {
    auto entry = GetCachedSector(frameAddress);
    if (entry == nullptr) {
        return 0u;
    }

    std::copy_n(entry->sector.begin(), outSector.size(), outSector.begin());
    if (outPosition != nullptr) {
        *outPosition = entry->pos;
    }
    return outSector.size();
}

uint32 HostCDDevice::ReadSectorUserDataImpl(uint32 frameAddress, std::span<uint8, 2048> outSector) {
    auto entry = GetCachedSector(frameAddress);
    if (entry == nullptr) {
        return 0u;
    }

#if defined(__PROSPERO__)
    const auto data = SectorUserData(entry->sector);
    if (data.empty()) {
        return 0;
    }
    std::ranges::copy(data, outSector.begin());
#else
    std::copy_n(entry->sector.begin() + 0x10, outSector.size(), outSector.begin());
#endif
    return outSector.size();
}

void HostCDDevice::BeginSeekToFrameAddressImpl(uint32 frameAddress) {
    const uint32 target = bit::extract<0, 23>(frameAddress);
    ++m_seekState.requestedCount;
    EnqueueCommand(Command::SeekFrameAddress(m_seekState.requestedCount, frameAddress));
}

void HostCDDevice::BeginSeekToTrackIndexImpl(uint8 track, uint8 index) {
    const uint32 target = (1u << 31u) | (track << 8u) | index;
    ++m_seekState.requestedCount;
    EnqueueCommand(Command::SeekTrackIndex(m_seekState.requestedCount, track, index));
}

void HostCDDevice::Disconnect() {
    if (m_workerThread.joinable()) {
        EnqueueCommand(Command::Quit());
        m_workerThread.join();
    }
    // TODO: cleanup other resources (notification handles, etc.)
    if (m_devHandle != host::kInvalidDeviceHandle) {
        host::CloseDeviceHandle(m_devHandle);
        m_devHandle = host::kInvalidDeviceHandle;
    }
    ClearMainDiscInfo();
}

auto HostCDDevice::GetCachedSector(uint32 frameAddress) -> std::shared_ptr<SectorEntry> {
    // Look in LRU cache
    std::shared_ptr<SectorEntry> entry = m_sectorCache.Get(frameAddress);
    if (entry != nullptr) {
        devlog::trace<grp::cache>("{:06X} found in LRU cache", frameAddress);
        return entry;
    }

    // Look in prefetch cache
    entry = m_sectorPrefetchCache.Get(frameAddress);
    if (entry != nullptr) {
        // Found it; move to main LRU cache
        m_sectorCache.Put(frameAddress, entry);
        m_sectorPrefetchCache.Evict(frameAddress);
        EnqueueCommand(Command::WakeUp()); // unblocks the worker thread immediately
        devlog::trace<grp::cache>("{:06X} found in prefetch cache; LRU cache usage: {} of {}", frameAddress,
                                  m_sectorCache.Size(), m_sectorCache.GetCapacity());
        return entry;
    }

    // Not found in either cache
    return ExecuteInWorkerSync([&]() -> std::shared_ptr<SectorEntry> {
        // The worker may have finished prefetching this sector while the request
        // was waiting in the queue. Check again before disturbing the prefetcher.
        if (auto prefetched = m_sectorPrefetchCache.Get(frameAddress)) {
            m_sectorCache.Put(frameAddress, prefetched);
            m_sectorPrefetchCache.Evict(frameAddress);
            devlog::trace<grp::cache>("{:06X} found in prefetch cache after wait", frameAddress);
            return prefetched;
        }

        auto &ts = m_threadState;
        const TrackInfo *track = ts.toc.GetTrackInfoForFAD(frameAddress);
        const TrackInfo *prefetchTrack =
            ts.readSectors ? ts.toc.GetTrackInfoForFAD(ts.nextSector) : nullptr;
        // Restart the prefetcher only for a real discontinuity. A read behind
        // the current run (for example, a game revisiting an earlier sector)
        // must not throw away the look-ahead that is already buffered.
        const bool startAtTarget = !ts.readSectors || track != prefetchTrack || frameAddress >= ts.nextSector;
        if (startAtTarget) {
            StartPrefetcher(frameAddress);
        }
        devlog::trace<grp::cache>("{:06X} cache miss{}", frameAddress,
                                  startAtTarget ? " (prefetcher restarted)" : "");

        // Read sector
        auto missed = std::make_shared<SectorEntry>(frameAddress);
        if (!HostReadSectorForCache(frameAddress, *missed)) {
            devlog::trace<grp::cache>("{:06X} could not be read", frameAddress);
            return nullptr;
        }
        m_sectorCache.Put(frameAddress, missed);
        if (startAtTarget) {
            // The just-read sector is in the LRU cache, not in the prefetch
            // cache; continue from the next sector.
            ts.nextSector = frameAddress + 1;
        }
        devlog::trace<grp::cache>("{:06X} cached in LRU cache; usage: {} of {}", frameAddress, m_sectorCache.Size(),
                                  m_sectorCache.GetCapacity());
        return missed;
    });
}

void HostCDDevice::EnqueueCommand(Command &&cmd) {
    m_workQueue.enqueue(m_ptokWorkQueue, cmd);
}

template <typename TFn>
    requires std::is_void_v<std::invoke_result_t<TFn>>
void HostCDDevice::ExecuteInWorker(TFn &&fn) {
    EnqueueCommand(Command::InvokeFunction([&] { fn(); }));
}

template <typename TFn>
    requires(!std::is_void_v<std::invoke_result_t<TFn>>)
typename std::invoke_result_t<TFn> HostCDDevice::ExecuteInWorkerSync(TFn &&fn) {
    using TReturn = std::invoke_result_t<TFn>;
    std::promise<TReturn> promise{};
    std::future<TReturn> result = promise.get_future();
    EnqueueCommand(Command::InvokeFunction([&] { promise.set_value(fn()); }));
    return result.get();
}

template <typename TFn>
    requires(!std::is_void_v<std::invoke_result_t<TFn>>)
std::future<typename std::invoke_result_t<TFn>> HostCDDevice::ExecuteInWorkerAsync(TFn &&fn) {
    using TReturn = std::invoke_result_t<TFn>;
    auto promise = std::make_shared<std::promise<TReturn>>();
    std::future<TReturn> result = promise->get_future();
    EnqueueCommand(Command::InvokeFunction([=] { promise->set_value(fn()); }));
    return result;
}

void HostCDDevice::WorkerThread() {
    util::SetCurrentThreadName(fmt::format("{} worker", m_path).c_str());

    using namespace std::chrono_literals;

    auto &ts = m_threadState;

    ts.driveState = host::PollDriveState(m_devHandle);
    InitializeWorkerDiscInfo();

    Command cmd{};
    bool running = true;
    while (running) {
        // Keep enough look-ahead for 2x CD data and audio. At 2x, 512 sectors
        // cover about 3.4 seconds (6.8 seconds for 1x audio), which absorbs USB
        // bridge latency spikes and short seeks without draining the cache.
        static constexpr uint32 kSectorPrefetchCapacity = 512;
        const size_t prefetched = m_sectorPrefetchCache.Size();
        // After a seek only a small prime is buffered. Refill it in a small
        // batch first so the next sectors arrive before the prime drains; once
        // the cache is warmed up, use full-size USB transfers.
        const uint32 sectorFetchSize = prefetched < 16 ? std::min(8u, m_maxReadBatch) : m_maxReadBatch;

        // Refill only when a full batch fits. Reading one sector at a time as
        // soon as one is consumed multiplies USB command overhead and causes
        // exactly the stutter this cache is meant to avoid.
        const bool shouldReadSectors =
            // actively reading sectors
            ts.readSectors &&
            // there's room for a whole batch
            prefetched + sectorFetchSize <= kSectorPrefetchCapacity;

        const bool dequeued = shouldReadSectors ? m_workQueue.try_dequeue(m_ctokWorkQueue, cmd)
                                                : m_workQueue.wait_dequeue_timed(m_ctokWorkQueue, cmd, 1s);

        if (dequeued) {
            using CmdType = Command::Type;
            switch (cmd.type) {
            case CmdType::WakeUp: /* no-op on purpose */ break;
            case CmdType::SeekFrameAddress: //
            {
                const auto seekData = std::get<Command::SeekData>(cmd.data);
                // No need to issue SEEK command to host device for this; the frame address is (obviously) known

                const uint32 frameAddress = std::max(seekData.target.frameAddress, 150u);
                SetSeekResult(seekData.counter, frameAddress);
                break;
            }
            case CmdType::SeekTrackIndex: //
            {
                const auto seekData = std::get<Command::SeekData>(cmd.data);
                const TrackInfo *trackInfo = ts.toc.GetTrackInfoForNumber(seekData.target.track);
                if (trackInfo != nullptr) {
                    if (seekData.target.index == 1) {
                        // Trivial case: seek to start of track; target frame address is known from TOC
                        SetSeekResult(seekData.counter, trackInfo->startFrameAddress);
                    } else {
                        const uint8 track = seekData.target.track;
                        const uint8 index = seekData.target.index;

                        if (auto it = ts.indexFADs.find({track, index}); it != ts.indexFADs.end()) {
                            // Retrieve cached result
                            SetSeekResult(seekData.counter, it->second);
                        } else {
                            // Binary search for index between the track's start and end FADs.
                            // Include start of next track in upper bound in case the index is out of range.
                            uint32 lbFAD = trackInfo->startFrameAddress;
                            uint32 ubFAD = trackInfo->endFrameAddress + 1u;
                            uint32 frameAddress = (lbFAD + ubFAD) >> 1u;

                            std::array<uint8, 2352> sectorBuffer{};
                            DiscPosition pos{};
                            while (lbFAD != ubFAD) {
                                if (!HostReadSectorAndPosition(frameAddress, sectorBuffer, pos, true)) {
                                    // Read failed
                                    frameAddress = 0xFFFFFF;
                                    break;
                                }

                                // Adjust bounds
                                const uint8 posTrack = util::from_bcd(pos.track);
                                const uint8 posIndex = util::from_bcd(pos.index);
                                if (posTrack > track || (posTrack == track && posIndex >= index)) {
                                    ubFAD = frameAddress;
                                } else if (posTrack < track || (posTrack == track && posIndex < index)) {
                                    lbFAD = frameAddress + 1u;
                                }
                                frameAddress = (lbFAD + ubFAD) >> 1u;
                            }

                            SetSeekResult(seekData.counter, frameAddress);

                            // Cache result
                            if (frameAddress != 0xFFFFFF) {
                                ts.indexFADs.insert({{track, index}, frameAddress});
                            }
                        }
                    }
                } else {
                    // Assume seek to lead-out area
                    SetSeekResult(seekData.counter, ts.toc.GetLeadOutFrameAddress());
                }
                break;
            }
            case CmdType::StopPrefetcher: StopPrefetcher(); break;
            case CmdType::InvokeFunction: std::get<Command::FunctionData>(cmd.data).function(); break;
            case CmdType::InvokeFunctionWithResult: std::get<Command::FunctionData>(cmd.data).function(); break;
            case CmdType::Quit: running = false; break;
            }
        }

        // Read and cache sectors + positions if requested
        if (!running) {
            break;
        }
        if (shouldReadSectors && ts.readSectors) {
            uint32 numSectors;
            if (HostPrefetchSectors(ts.nextSector, sectorFetchSize, numSectors)) {
                ts.nextSector += numSectors;
            } else {
                // Could not read any sectors or reached a terminal condition; stop reading
                ts.readSectors = false;
            }
        }

        const DriveState prevDriveState = ts.driveState;
        ts.driveState = host::PollDriveState(m_devHandle);

        if (prevDriveState != ts.driveState) {
            InitializeWorkerDiscInfo();
        }
    }

    ClearWorkerDiscInfo();
}

void HostCDDevice::InitializeWorkerDiscInfo() {
    auto &ts = m_threadState;

    std::unique_lock lock{ts.mtxDiscInfo};

    StopPrefetcher();
    m_maxReadBatch = kDefaultReadBatch;
#if defined(__PROSPERO__)
    m_useRawSubchannel = true;
#endif
    if (ts.driveState == DriveState::MediaPresent) {
        ts.toc.Clear();
        ts.toc.LoadFrom(HostReadTOC());
        std::array<uint8, 2352> headerSector{};
        DiscPosition pos{};
        if (HostReadSectorAndPosition(150u, headerSector, pos)) {
#if defined(__PROSPERO__)
            if (!ts.header.ReadFrom(SectorUserData(headerSector))) {
                ts.header.Invalidate();
            }
#else
            ts.header.ReadFrom(std::span{headerSector}.subspan(0x10));
#endif
        } else {
            ts.header.Invalidate();
        }

        // USB drives spin data CDs slowly until asked for speed. Saturn FMV
        // needs 2x continuously, so request the drive's maximum read speed.
        // Send this after the TOC/header reads: some bridges reject commands
        // sent while the unit is still spinning up. The transport layer logs
        // the result so a rejected SET CD SPEED is visible in usb.log.
        const auto setSpeed = scsi::op::MakeSetCDSpeed();
        uint32 speedReadSize{};
        if (!host::SendSCSIInCommand(m_devHandle, setSpeed, {}, speedReadSize)) {
            devlog::warn<grp::host>("Could not set the host drive to its maximum read speed");
        }

        if (ts.fs.Read(m_fsReader)) {
            devlog::info<grp::host>("Filesystem built successfully");
        } else {
            devlog::warn<grp::host>("Failed to build filesystem");
        }
    } else {
        ts.header.Invalidate();
        ts.toc.Clear();
        ts.fs.Clear();
        devlog::info<grp::host>("Disc absent - filesystem cleared");
    }
    ts.indexFADs.clear();
    ts.targetDriveState = ts.driveState;
    ts.discInfoChanged.store(true, std::memory_order_release);
}

void HostCDDevice::ClearMainDiscInfo() {
    m_driveState = DriveState::Unknown;
    m_header.Invalidate();
    m_toc.Clear();
    m_fs.Clear();
    m_sectorCache.Flush();
}

void HostCDDevice::ClearWorkerDiscInfo() {
    auto &ts = m_threadState;

    std::unique_lock lock{ts.mtxDiscInfo};
    m_sectorPrefetchCache.Flush();
    ts.header.Invalidate();
    ts.toc.Clear();
    ts.fs.Clear();
    ts.indexFADs.clear();
    ts.targetDriveState = ts.driveState;
    ts.discInfoChanged.store(true, std::memory_order_release);
}

void HostCDDevice::SetSeekResult(uint32 counter, uint32 frameAddress) {
    auto &ts = m_threadState;
    if (frameAddress != 0xFFFFFF) {
        StartPrefetcher(frameAddress);

        // Prime the cache before reporting the seek as done. This turns the
        // physical seek and first USB read into emulated seek latency; the CD
        // Block keeps polling IsSeekDone() without blocking the emulation
        // thread. Without this, the first Play tick always took the synchronous
        // cache-miss path and stalled the whole frame.
        uint32 primed = 0;
        if (HostPrefetchSectors(frameAddress, kSeekPrimeSectors, primed)) {
            ts.nextSector = frameAddress + primed;
        }
        // If priming failed, leave nextSector at the target so the worker can
        // retry once the drive is ready.
    }

    // Publish the target and completion together after attempting to prime the cache.
    ts.seekFAD.store(std::max(frameAddress, 150u), std::memory_order_relaxed);
    ts.seekCounter.store(counter, std::memory_order_release);
}

void HostCDDevice::StartPrefetcher(uint32 frameAddress) {
    auto &ts = m_threadState;
    ts.nextSector = frameAddress;
    ts.readSectors = true;
    m_sectorPrefetchCache.Flush();
    devlog::trace<grp::cache>("Prefetcher started at {:06X}", frameAddress);
}

void HostCDDevice::StopPrefetcher() {
    auto &ts = m_threadState;
    ts.readSectors = false;
    m_sectorPrefetchCache.Flush();
    devlog::trace<grp::cache>("Prefetcher stopped");
}

std::vector<TOCEntry> HostCDDevice::HostReadTOC() const {
#if defined(__PROSPERO__)
    // Use the same allocation length that SwanStationPS5 uses for USB bridges.
    std::array<uint8, 2048> buffer{};
#else
    std::vector<uint8> buffer(8);
#endif

    // Build Read TOC command
    auto cdb = scsi::op::MakeReadTOC(buffer.size());
    uint32 readSize{};

    if (!host::SendSCSIInCommand(m_devHandle, cdb, buffer, readSize) || readSize < 4) {
        return {};
    }

    const auto bufferLength = util::ReadBE<uint16>(&buffer[0]);
    const size_t totalSize = bufferLength + 2u;
    if (totalSize < 20 || totalSize > 4 + (99 + 1) * 8 || (totalSize - 4) % 8 != 0) {
        return {};
    }
#if !defined(__PROSPERO__)
    // Redo the request with a buffer large enough to fit the data.
    buffer.resize(totalSize);
    util::WriteBE<uint16>(&cdb[7], buffer.size());
    if (!host::SendSCSIInCommand(m_devHandle, cdb, buffer, readSize)) {
        return {};
    }
#endif
    if (readSize < totalSize || util::ReadBE<uint16>(buffer.data()) != bufferLength) {
        return {};
    }

    const uint8 firstTrackNum = buffer[2];
    const uint8 lastTrackNum = buffer[3];
    if (firstTrackNum == 0 || lastTrackNum < firstTrackNum || lastTrackNum > 99 ||
        totalSize != 4 + (lastTrackNum - firstTrackNum + 2u) * 8) {
        return {};
    }

    // Convert to TOCEntry list
    std::vector<TOCEntry> toc{};

    // Point A0 - first data track
    {
        auto &tocEntry = toc.emplace_back();
        tocEntry.controlADR = 0x41;
        tocEntry.trackNum = 0x00;
        tocEntry.pointOrIndex = 0xA0;
        tocEntry.min = 0x00;
        tocEntry.sec = 0x00;
        tocEntry.frame = 0x00;
        tocEntry.zero = 0x00;
        tocEntry.amin = util::to_bcd(firstTrackNum);
        tocEntry.asec = 0x00;
        tocEntry.aframe = 0x00;
    }

    // Point A1 - last data track
    {
        auto &tocEntry = toc.emplace_back();
        tocEntry.controlADR = 0x41;
        tocEntry.trackNum = 0x00;
        tocEntry.pointOrIndex = 0xA1;
        tocEntry.min = 0x00;
        tocEntry.sec = 0x00;
        tocEntry.frame = 0x00;
        tocEntry.zero = 0x00;
        tocEntry.amin = util::to_bcd(lastTrackNum);
        tocEntry.asec = 0x00;
        tocEntry.aframe = 0x00;
    }

    // Point A2 - start of leadout track
    // Filled in the loop below
    toc.emplace_back();

    // Tracks
    size_t pos = 4;
    uint32 previousLBA = 0;
    while (pos + 8 <= totalSize) {
        const uint8 *trackData = &buffer[pos];

        const uint8 control = bit::extract<0, 3>(trackData[1]);
        const uint8 adr = bit::extract<4, 7>(trackData[1]);
        const uint8 trackNum = trackData[2];
        const uint8 expectedTrack = pos + 8 == totalSize ? 0xAA : firstTrackNum + (pos - 4) / 8;
        const uint32 lba = util::ReadBE<uint32>(&trackData[4]);
        if (trackNum != expectedTrack || lba >= 100u * 60 * 75 - 150 || (pos > 4 && lba <= previousLBA)) {
            return {};
        }
        previousLBA = lba;
        const uint32 fad = lba + 150u;

        if (trackNum == 0xAA) {
            auto &leadoutEntry = toc[2];
            leadoutEntry.controlADR = 0x41;
            leadoutEntry.trackNum = 0x00;
            leadoutEntry.pointOrIndex = 0xA2;
            leadoutEntry.min = 0x00;
            leadoutEntry.sec = 0x00;
            leadoutEntry.frame = 0x00;
            leadoutEntry.zero = 0x00;
            leadoutEntry.amin = util::to_bcd(fad / 75 / 60);
            leadoutEntry.asec = util::to_bcd(fad / 75 % 60);
            leadoutEntry.aframe = util::to_bcd(fad % 75);
        } else {
            const uint32 relFAD = 0; // TODO: find in image
            auto &tocEntry = toc.emplace_back();
            tocEntry.controlADR = (control << 4u) | adr;
            tocEntry.trackNum = 0x00;
            tocEntry.pointOrIndex = util::to_bcd(trackNum);
            tocEntry.min = util::to_bcd(relFAD / 75 / 60);
            tocEntry.sec = util::to_bcd(relFAD / 75 % 60);
            tocEntry.frame = util::to_bcd(relFAD % 75);
            tocEntry.zero = 0x00;
            tocEntry.amin = util::to_bcd(fad / 75 / 60);
            tocEntry.asec = util::to_bcd(fad / 75 % 60);
            tocEntry.aframe = util::to_bcd(fad % 75);
        }
        pos += 8;
    }

    return toc;
}

bool HostCDDevice::HostReadSectors(uint32 frameAddress, uint32 sectorCount, std::span<uint8> buffer,
                                   uint32 &sectorSize, bool needSubchannel) {
    if (frameAddress < 150 || sectorCount == 0) {
        return false;
    }

    uint8 format = 0xf8;
#if defined(__PROSPERO__)
    if (const TrackInfo *track = m_threadState.toc.GetTrackInfoForFAD(frameAddress);
        track != nullptr && !bit::test<6>(track->controlADR)) {
        // Some USB bridges reject sync/header field requests for CD-DA.
        format = 0x10;
    }
#endif

    const auto read = [&](uint8 subchannel, uint32 stride) {
        uint32 bytesRead{};
        const auto cdb = scsi::op::MakeReadCD(frameAddress - 150, sectorCount, format, subchannel);
        const auto output = buffer.first(sectorCount * stride);
        if (!host::SendSCSIInCommand(m_devHandle, cdb, output, bytesRead) || bytesRead != output.size()) {
            return false;
        }
        sectorSize = stride;
        return true;
    };

#if defined(__PROSPERO__)
    // Raw P-W subchannel reads are optional and many USB bridges execute them
    // at 1x, far below the speed needed for FMV. Stream plain 2352-byte sectors
    // and synthesize positions from the TOC; only ask for real subchannel data
    // when a track index search needs it.
    if (needSubchannel && m_useRawSubchannel) {
        if (read(1, kRawSectorWithSubchannel)) {
            return true;
        }
        m_useRawSubchannel = false;
    }
    return read(0, kRawSectorSize);
#else
    (void)needSubchannel;
    return read(1, kRawSectorWithSubchannel);
#endif
}

DiscPosition HostCDDevice::DecodePosition(uint32 frameAddress, std::span<const uint8> sector) const {
#if defined(__PROSPERO__)
    if (sector.size() == kRawSectorSize) {
        const TrackInfo *track = m_threadState.toc.GetTrackInfoForFAD(frameAddress);
        if (track == nullptr) {
            return {};
        }
        const auto [relM, relS, relF] = FADToMSF(frameAddress - track->startFrameAddress);
        const auto [m, s, f] = FADToMSF(frameAddress);
        return {track->controlADR,
                util::to_bcd(track->number),
                0x01,
                util::to_bcd(relM),
                util::to_bcd(relS),
                util::to_bcd(relF),
                0,
                util::to_bcd(m),
                util::to_bcd(s),
                util::to_bcd(f)};
    }
#endif

    const auto subchannel = sector.subspan(kRawSectorSize);
    std::array<uint8, 12> subQ{};
    for (uint32 i = 0; i < subQ.size() * 8; ++i) {
        subQ[i >> 3u] |= bit::extract<6>(subchannel[i]) << (~i & 7u);
    }
    return {subQ[0], subQ[1], subQ[2], subQ[3], subQ[4], subQ[5], subQ[6], subQ[7], subQ[8], subQ[9]};
}

bool HostCDDevice::HostReadSectorAndPosition(uint32 frameAddress, std::span<uint8, 2352> outSector,
                                             DiscPosition &outPos, bool needSubchannel) {
    std::array<uint8, kRawSectorWithSubchannel> data{};
    uint32 sectorSize{};
    if (!HostReadSectors(frameAddress, 1, data, sectorSize, needSubchannel)) {
        return false;
    }
    std::copy_n(data.begin(), outSector.size(), outSector.begin());
    outPos = DecodePosition(frameAddress, std::span{data}.first(sectorSize));
    return true;
}

bool HostCDDevice::HostReadSectorUserData(uint32 frameAddress, std::span<uint8, 2048> outSector) {
    if (frameAddress < 150) {
        return false;
    }
#if defined(__PROSPERO__)
    // The same raw transfer used during play also works on USB bridges without cooked reads.
    std::array<uint8, kRawSectorWithSubchannel> raw{};
    uint32 sectorSize{};
    if (HostReadSectors(frameAddress, 1, raw, sectorSize)) {
        const auto data = SectorUserData(std::span{raw}.first<kRawSectorSize>());
        if (!data.empty()) {
            std::ranges::copy(data, outSector.begin());
            return true;
        }
    }
#endif
    const auto cdb = scsi::op::MakeRead10(frameAddress - 150u, 1);
    uint32 readSize{};
    return host::SendSCSIInCommand(m_devHandle, cdb, outSector, readSize) && readSize == outSector.size();
}

bool HostCDDevice::HostReadSectorForCache(uint32 frameAddress, SectorEntry &outEntry) {
    return HostReadSectorAndPosition(frameAddress, outEntry.sector, outEntry.pos);
}

bool HostCDDevice::HostPrefetchSectors(uint32 frameAddress, uint32 sectorCount, uint32 &outReadSectors) {
    const auto &toc = m_threadState.toc;
    const TrackInfo *track = toc.GetTrackInfoForFAD(frameAddress);
    const uint32 trackEnd = track != nullptr ? track->endFrameAddress : toc.GetEndFrameAddress();
    if (frameAddress > trackEnd) {
        return false;
    }

    // Keep the request within one track (the last FAD is inclusive) and within
    // the batch size accepted by the active transport.
    sectorCount = std::min(sectorCount, std::min(trackEnd - frameAddress + 1u, m_maxReadBatch));
    std::vector<uint8> data(kRawSectorWithSubchannel * sectorCount);
    uint32 sectorSize{};
    const uint32 initialCount = sectorCount;
    while (!HostReadSectors(frameAddress, sectorCount, data, sectorSize)) {
        if (sectorCount == 1) {
            return false;
        }
        // Some CAM bridges reject transfers above a size that is not exposed
        // through the SCSI layer. Work down to a single sector if necessary.
        sectorCount /= 2;
    }
    if (sectorCount < initialCount) {
        m_maxReadBatch = sectorCount;
    }

    for (uint32 i = 0; i < sectorCount; ++i) {
        const uint32 address = frameAddress + i;
        devlog::trace<grp::cache>("{:06X} prefetched, usage: {}", address, m_sectorPrefetchCache.Size());
        auto entry = m_sectorPrefetchCache.Emplace(address);
        const auto sector = std::span{data}.subspan(i * sectorSize, sectorSize);
        std::copy_n(sector.begin(), entry->sector.size(), entry->sector.begin());
        entry->pos = DecodePosition(address, sector);
    }

    outReadSectors = sectorCount;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

void HostCDDevice::SectorCache::SetCapacity(size_t capacity) {
    m_capacity = std::max<size_t>(1, capacity);

    // Erase excess items
    while (m_entryList.size() > capacity) {
        std::shared_ptr<SectorEntry> value = m_entryList.front();
        m_entryMap.erase(value->frameAddress);
        m_entryList.pop_front();
    }
}

size_t HostCDDevice::SectorCache::Size() const {
    return m_entryMap.size();
}

void HostCDDevice::SectorCache::Flush() {
    m_entryList.clear();
    m_entryMap.clear();
}

auto HostCDDevice::SectorCache::Get(uint32 frameAddress) -> std::shared_ptr<SectorEntry> {
    // Find cached item
    auto it = m_entryMap.find(frameAddress);
    if (it == m_entryMap.end()) {
        // No such item
        return nullptr;
    }

    // Update LRU order
    m_entryList.splice(m_entryList.begin(), m_entryList, it->second);

    return *it->second;
}

void HostCDDevice::SectorCache::Put(uint32 frameAddress, std::shared_ptr<SectorEntry> entry) {
    // Check for existing entry
    auto it = m_entryMap.find(frameAddress);
    if (it != m_entryMap.end()) {
        // Replace it
        *it->second = entry;
        m_entryList.splice(m_entryList.begin(), m_entryList, it->second);
        return;
    }

    // Check capacity
    if (m_entryMap.size() >= m_capacity) {
        // Make room for new entry
        const uint32 oldestKey = m_entryList.back()->frameAddress;
        m_entryList.pop_back();
        m_entryMap.erase(oldestKey);
    }

    // Add entry to the top of the LRU sequence
    m_entryList.emplace_front(entry);
    m_entryMap[frameAddress] = m_entryList.begin();
}

bool HostCDDevice::SectorCache::Evict(uint32 frameAddress) {
    auto it = m_entryMap.find(frameAddress);
    if (it == m_entryMap.end()) {
        return false;
    }

    m_entryList.erase(it->second);
    m_entryMap.erase(frameAddress);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------

size_t HostCDDevice::SectorPrefetchCache::Size() const {
    std::unique_lock lock{m_mtx};
    return m_entries.size();
}

void HostCDDevice::SectorPrefetchCache::Flush() {
    std::unique_lock lock{m_mtx};
    m_entries.clear();
}

auto HostCDDevice::SectorPrefetchCache::Get(uint32 frameAddress) -> std::shared_ptr<SectorEntry> {
    std::unique_lock lock{m_mtx};

    // Find cached item
    auto it = m_entries.find(frameAddress);
    if (it == m_entries.end()) {
        // No such item
        return nullptr;
    }

    return it->second;
}

auto HostCDDevice::SectorPrefetchCache::Emplace(uint32 frameAddress) -> std::shared_ptr<SectorEntry> {
    std::unique_lock lock{m_mtx};

    auto entry = std::make_shared<SectorEntry>(frameAddress);

    // Check for existing entry
    auto it = m_entries.find(frameAddress);
    if (it != m_entries.end()) {
        // Replace it
        it->second.swap(entry);
        return entry;
    }

    // Add to cache
    m_entries[frameAddress] = entry;
    return entry;
}

bool HostCDDevice::SectorPrefetchCache::Evict(uint32 frameAddress) {
    std::unique_lock lock{m_mtx};
    return m_entries.erase(frameAddress) > 0;
}

// ---------------------------------------------------------------------------------------------------------------------

bool HostCDDevice::FilesystemReader::HasDisc() const {
    return m_dev.m_threadState.driveState == DriveState::MediaPresent;
}

const TOC &HostCDDevice::FilesystemReader::GetTOC() const {
    return m_dev.m_threadState.toc;
}

bool HostCDDevice::FilesystemReader::ReadSectorUserData(uint32 frameAddress, std::span<uint8, 2048> outSector) {
    return m_dev.HostReadSectorUserData(frameAddress, outSector);
}

} // namespace ymir::media
