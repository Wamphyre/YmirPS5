// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace ps5 {

// Makes sure the local optical service is accepting connections. When the
// title runs inside a sandbox that cannot open the CAM devices, the service is
// sent to the console's ELF loader so that it runs with the loader's process
// permissions instead of the title's.
bool EnsureDiscService(void (*report)(const char *));

} // namespace ps5
