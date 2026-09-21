#include "blackhole/core/ids.hpp"

#include <cstdio>

namespace bhg {

namespace {

std::string hex16(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

std::string hex_dec(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
  return std::string(buf);
}

}  // namespace

std::string to_text(NodeId v) { return "node:" + hex16(v.value()); }
std::string to_text(LinkId v) { return "link:" + hex16(v.value()); }
std::string to_text(PathId v) { return "path:" + hex16(v.value()); }
std::string to_text(EvidenceSourceId v) { return "src:" + hex16(v.value()); }
std::string to_text(FenceId v) { return "fence:" + hex16(v.value()); }
std::string to_text(DecisionId v) { return "dec:" + hex16(v.value()); }
std::string to_text(SessionId v) { return "sess:" + hex16(v.value()); }
std::string to_text(BootId v) { return "boot:" + hex16(v.value()); }
std::string to_text(IncarnationId v) { return "inc:" + hex16(v.value()); }
std::string to_text(PathGeneration v) { return "pg" + hex_dec(v.value()); }
std::string to_text(TopologyGeneration v) { return "tg" + hex_dec(v.value()); }
std::string to_text(LinkStateGeneration v) { return "lg" + hex_dec(v.value()); }
std::string to_text(CoordinatorEpoch v) { return "ep" + hex_dec(v.value()); }
std::string to_text(PolicyVersion v) { return "pol:" + hex16(v.value()); }

}  // namespace bhg
