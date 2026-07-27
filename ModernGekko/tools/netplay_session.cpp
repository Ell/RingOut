// netplay_session.cpp — LOCAL STUB (single-player build).
//
// The original 649-line implementation is built against an extended
// NetPlayClient API (SetReady, SetLocalControllerCount,
// GetAssignedControllerCount, GetPlayersSnapshot, GetWiimoteMappingSnapshot,
// GetInputWaitTelemetry, GetButtonPressCount, ...) that lives only in an
// unpushed local RecompCore fork and is absent from the public vendored
// Dolphin. Netplay therefore cannot be compiled from the public repositories.
//
// Since SoulCalibur is being brought up single-player, this stub replaces the
// lobby with a graceful "unavailable" path so moderngekko-run / moderngekko-port
// link and run. The original is preserved at work/out/netplay_session.cpp.orig.

#include "netplay_session.hpp"

#include <cstdio>

namespace moderngekko::frontend {

int RunNetplayLobby(RuntimeConfig /*runtime_config*/,
                    ConfigResult /*frontend_config*/,
                    NetplayOptions /*options*/) {
  std::fprintf(stderr,
               "moderngekko: netplay is unavailable in this build "
               "(requires an unpushed RecompCore NetPlay API).\n");
  return static_cast<int>(NetplayExitCode::Failed);
}

} // namespace moderngekko::frontend
