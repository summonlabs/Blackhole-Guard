#pragma once

/// Blackhole Guard -- Summon Software Labs infrastructure runtime.
///
/// Umbrella header. Include this for the complete public surface, or include the
/// individual headers under blackhole/ for a narrower dependency.

#include "blackhole/version.hpp"

#include "blackhole/core/bitops.hpp"
#include "blackhole/core/bounded.hpp"
#include "blackhole/core/canonical.hpp"
#include "blackhole/core/checked.hpp"
#include "blackhole/core/hash.hpp"
#include "blackhole/core/ids.hpp"
#include "blackhole/core/nonce.hpp"
#include "blackhole/core/outcome.hpp"
#include "blackhole/core/path.hpp"
#include "blackhole/core/time.hpp"

#include "blackhole/domain/classification.hpp"
#include "blackhole/domain/evidence.hpp"
#include "blackhole/domain/generation.hpp"
#include "blackhole/domain/policy.hpp"
#include "blackhole/domain/scope.hpp"

#include "blackhole/evidence/ledger.hpp"

#include "blackhole/diagnosis/classifier.hpp"

#include "blackhole/authority/authority.hpp"

#include "blackhole/localize/localizer.hpp"

#include "blackhole/persist/journal.hpp"
#include "blackhole/persist/record.hpp"
#include "blackhole/persist/snapshot.hpp"
#include "blackhole/persist/store.hpp"

#include "blackhole/protocol/frame.hpp"
#include "blackhole/protocol/messages.hpp"
#include "blackhole/protocol/session.hpp"

#include "blackhole/service/client.hpp"
#include "blackhole/service/engine.hpp"
#include "blackhole/service/net.hpp"
#include "blackhole/service/server.hpp"
