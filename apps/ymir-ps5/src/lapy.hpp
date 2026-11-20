// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5 {

enum class LapyStatus {
    Granted,
    HelperMissing,
    LoaderUnavailable,
    Rejected,
    ProtocolError,
    TransportError,
};

// Sends the packaged Lapy one-shot helper for this title to the local ELF
// loader and completes its prepare/response exchange. On success the process
// owns a private root credential, which exposes /dev to the sandboxed title.
LapyStatus LapyElevate(void (*log)(const char *message));

const char *DescribeLapy(LapyStatus status);

} // namespace ps5
