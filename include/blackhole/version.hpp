#pragma once

// Blackhole Guard -- Summon Software Labs infrastructure runtime.
//
// Product proposition:
//   Given a path that is structurally legal but authoritative delivery evidence is
//   missing or contradictory, is traffic being blackholed now, where is the failure
//   localized, what authority must be fenced, and when may the path be restored?
//
// Systems boundary: this runtime owns detection/classification of delivery blackholes
// and bounded fencing *intent* for affected path authority. It does NOT compute routes,
// own congestion state, collect telemetry, or perform packet forwarding. Adjacent
// runtime owners are integrated only through explicit typed inputs, evidence,
// references and authority boundaries.

#include <cstdint>

namespace bhg {

inline constexpr std::uint16_t kVersionMajor = 1;
inline constexpr std::uint16_t kVersionMinor = 0;
inline constexpr std::uint16_t kVersionPatch = 1;

/// Durable/native format version. Bumped only on incompatible layout changes.
inline constexpr std::uint16_t kFormatVersion = 1;

/// Framed wire protocol version.
inline constexpr std::uint16_t kProtocolVersion = 1;

inline constexpr const char* kVersionString = "1.0.1";
inline constexpr const char* kProductName = "Blackhole Guard";
inline constexpr const char* kProductVendor = "Summon Software Labs";

}  // namespace bhg
