// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5 {

// Sends the packaged Lapy one-shot helper for this title to the local ELF
// loader and completes its prepare/response exchange. On success the process
// owns a private root credential, which exposes /dev to the sandboxed title.
bool LapyElevate();

} // namespace ps5
