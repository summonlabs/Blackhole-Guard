#pragma once

#include <cstdint>
#include <string_view>

namespace bhg {

/// Every externally visible result of this runtime is expressed as one of these
/// codes. UNKNOWN / STALE / CONFLICT / INVALID / UNSUPPORTED are first-class and
/// are never silently folded into Ok or into an ordinary "not found".
enum class Outcome : std::uint8_t {
  Ok = 0,

  // Indeterminacy: the runtime refuses to make a positive claim.
  NoEvidence = 1,
  Unknown = 2,
  Stale = 3,
  Conflict = 4,
  Indeterminate = 5,
  SearchLimitReached = 6,

  // Input/contract rejection.
  Invalid = 7,
  Unsupported = 8,
  Malformed = 9,
  Oversized = 10,
  Duplicate = 11,
  Replayed = 12,
  Regressed = 13,
  NotFound = 14,
  Exhausted = 15,
  Rejected = 16,

  // Durability / authority lifecycle.
  Corrupt = 17,
  Interrupted = 18,
  Fenced = 19,
  Revoked = 20,
  Expired = 21,
  Unavailable = 22,
};

constexpr std::string_view to_string(Outcome o) noexcept {
  switch (o) {
    case Outcome::Ok: return "Ok";
    case Outcome::NoEvidence: return "NoEvidence";
    case Outcome::Unknown: return "Unknown";
    case Outcome::Stale: return "Stale";
    case Outcome::Conflict: return "Conflict";
    case Outcome::Indeterminate: return "Indeterminate";
    case Outcome::SearchLimitReached: return "SearchLimitReached";
    case Outcome::Invalid: return "Invalid";
    case Outcome::Unsupported: return "Unsupported";
    case Outcome::Malformed: return "Malformed";
    case Outcome::Oversized: return "Oversized";
    case Outcome::Duplicate: return "Duplicate";
    case Outcome::Replayed: return "Replayed";
    case Outcome::Regressed: return "Regressed";
    case Outcome::NotFound: return "NotFound";
    case Outcome::Exhausted: return "Exhausted";
    case Outcome::Rejected: return "Rejected";
    case Outcome::Corrupt: return "Corrupt";
    case Outcome::Interrupted: return "Interrupted";
    case Outcome::Fenced: return "Fenced";
    case Outcome::Revoked: return "Revoked";
    case Outcome::Expired: return "Expired";
    case Outcome::Unavailable: return "Unavailable";
  }
  return "Invalid";
}

/// Only Outcome::Ok is affirmative. Nothing else may ever be treated as success.
constexpr bool is_affirmative(Outcome o) noexcept { return o == Outcome::Ok; }

/// True when the outcome denotes "we could not decide", as opposed to a definite
/// negative or a contract violation.
constexpr bool is_indeterminate(Outcome o) noexcept {
  switch (o) {
    case Outcome::NoEvidence:
    case Outcome::Unknown:
    case Outcome::Stale:
    case Outcome::Conflict:
    case Outcome::Indeterminate:
    case Outcome::SearchLimitReached:
      return true;
    default:
      return false;
  }
}

/// True when the outcome denotes refused trust in the inputs or in durable state.
constexpr bool is_untrusted(Outcome o) noexcept {
  switch (o) {
    case Outcome::Invalid:
    case Outcome::Malformed:
    case Outcome::Oversized:
    case Outcome::Corrupt:
    case Outcome::Unsupported:
      return true;
    default:
      return false;
  }
}

constexpr bool is_valid_outcome(std::uint8_t raw) noexcept {
  return raw <= static_cast<std::uint8_t>(Outcome::Unavailable);
}

}  // namespace bhg
