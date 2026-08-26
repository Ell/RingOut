#include "Core/NetPlay/Sc2RollbackProfile.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace
{
void Check(bool value, const char* message)
{
  if (!value)
  {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}
}  // namespace

int main()
{
  const auto known = NetPlay::FindSc2RollbackProfile(
      "GRSEAF", "0ad25684426e6e04ee92a1d7919eec08d8d1528af8513472c44dd2eb20ea7ac5");
  Check(known.has_value(), "supported DOL has a discovery identity");
  Check(known->status == NetPlay::Sc2RollbackProfileStatus::DiscoveryOnly,
        "uncertified profile is explicitly discovery-only");
  Check(!NetPlay::IsSc2RollbackProfileCertified(*known),
        "discovery profile cannot activate selective rollback");
  Check(!NetPlay::FindSc2RollbackProfile("GRSEAF", "wrong").has_value(), "wrong DOL fails closed");
  Check(!NetPlay::FindSc2RollbackProfile("GRSEPS", known->dol_sha256).has_value(),
        "wrong region fails closed");

  constexpr std::array<NetPlay::RollbackRegionSnapshotRing::Region, 1> regions{{{0x1000, 0x2000}}};
  NetPlay::Sc2RollbackProfile certified{
      .id = "test-certified",
      .disc_id = "GRSEAF",
      .dol_sha256 = known->dol_sha256,
      .version = 2,
      .status = NetPlay::Sc2RollbackProfileStatus::Certified,
      .hooks = {.update_begin = 1,
                .update_end = 2,
                .input_read = 3,
                .render_begin = 4,
                .audio_dispatch = 5,
                .persistence_dispatch = 6},
      .state_regions = regions,
  };
  Check(NetPlay::IsSc2RollbackProfileCertified(certified),
        "complete certified profile can activate");
  certified.hooks.audio_dispatch = 0;
  Check(!NetPlay::IsSc2RollbackProfileCertified(certified),
        "missing side-effect hook fails certification");
  std::cout << "SC2 rollback profile tests passed\n";
  return 0;
}
