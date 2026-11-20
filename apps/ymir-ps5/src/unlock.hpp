// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5 {

enum class UnlockStatus {
    AlreadyUnlocked,
    Granted,
    LapyGranted,
    NoAnswer,
    CannotRequest,
    SkippedAfterCrash,
};

// Asks etaHEN, OnionHEN, LegacyJB or PS5SX2 Helper to lift this title's
// sandbox, then falls back to the packaged Lapy owned-root helper. Must be
// called before the emulator starts any threads. A successful unlock exposes
// /dev, including the CAM nodes of the USB optical reader.
UnlockStatus UnlockSandbox();

const char *DescribeUnlock(UnlockStatus status);

} // namespace ps5
