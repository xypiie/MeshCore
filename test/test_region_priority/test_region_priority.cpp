#include <gtest/gtest.h>

#include <string>
#include <vector>
#include "helpers/RegionPriorityCLI.h"

namespace {

class RegionPriority : public ::testing::Test {
protected:
  TransportKeyStore keys;
  RegionMap regions{keys};

  void SetUp() override {
    for (auto name : {"scope-a", "scope-b"}) {
      auto region = regions.putRegion(name, 0);
      ASSERT_NE(nullptr, region);
      region->flags = 0;
    }
  }

  std::string cli(const char* text, bool supported = true) {
    char command[160], reply[160];
    snprintf(command, sizeof(command), "%s", text);
    EXPECT_TRUE(RegionPriorityCLI::handle(regions, command, reply,
                                          sizeof(reply), supported));
    return reply;
  }

  mesh::Packet scopedPacket(const char* name) {
    mesh::Packet packet;
    packet.header = ROUTE_TYPE_TRANSPORT_FLOOD |
                    (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT);
    packet.payload_len = 4;
    packet.payload[0] = 0xB0;
    packet.payload[1] = 0x11;
    packet.payload[2] = 0x22;
    packet.payload[3] = 0x33;
    TransportKey key;
    EXPECT_EQ(1, regions.getTransportKeysFor(*regions.findByName(name), &key, 1));
    packet.transport_codes[0] = key.calcTransportCode(&packet);
    packet.transport_codes[1] = 0;
    return packet;
  }

  std::vector<uint8_t> saved(MemoryFileSystem& fs) {
    EXPECT_TRUE(regions.save(&fs));
    auto file = fs.open("/regions2");
    std::vector<uint8_t> bytes(file.available());
    EXPECT_EQ(bytes.size(), file.read(bytes.data(), bytes.size()));
    return bytes;
  }
};

TEST_F(RegionPriority, DefaultsToDisabled) {
  EXPECT_EQ(nullptr, regions.getPriorityRegion());
  EXPECT_EQ(0, regions.getPriorityPercent());
  auto packet = scopedPacket("scope-a");
  EXPECT_FALSE(regions.isPriorityPacket(&packet));
  EXPECT_EQ(" priority is off", cli("region prio"));
}

TEST_F(RegionPriority, CliSelectsAndReportsOnePreferredRegion) {
  EXPECT_EQ(0u, cli("region prio scope-a 10").find("OK -"));
  EXPECT_EQ("scope-a", std::string(regions.getPriorityRegion()->name));
  EXPECT_EQ(10, regions.getPriorityPercent());
  EXPECT_EQ(" priority is scope-a, reserve 10%", cli("region prio"));
  EXPECT_EQ(0u, cli("region prio scope-b 50").find("OK -"));
  EXPECT_EQ("scope-b", std::string(regions.getPriorityRegion()->name));
  EXPECT_EQ(50, regions.getPriorityPercent());
}

TEST_F(RegionPriority, ZeroDisablesOnlyTheNamedRegionsPriority) {
  cli("region prio scope-a 10");
  cli("region prio scope-b 0");
  EXPECT_EQ(10, regions.getPriorityPercent());
  cli("region prio scope-a 0");
  EXPECT_EQ(nullptr, regions.getPriorityRegion());
  EXPECT_EQ(0, regions.getPriorityPercent());
}

TEST_F(RegionPriority, RejectsMalformedCommandsWithoutChangingPolicy) {
  cli("region prio scope-a 10");
  for (auto command : {"region prio scope-a", "region prio scope-a -1",
                       "region prio scope-a 51", "region prio scope-a 99", "region prio scope-a 10junk",
                       "region prio scope-a nan", "region prio scope-a 10.5",
                       "region prio scope-a 10 extra", "region prio scope-a 99999999999999999999",
                       "region prio Unknown 10", "region prio * 10",
                       "region prio scope 10"}) {
    SCOPED_TRACE(command);
    EXPECT_EQ(0u, cli(command).find("Err -"));
    EXPECT_EQ(10, regions.getPriorityPercent());
    ASSERT_NE(nullptr, regions.getPriorityRegion());
    EXPECT_EQ("scope-a", std::string(regions.getPriorityRegion()->name));
  }
}

TEST_F(RegionPriority, OnlyHandlesItsOwnCommandAndSupportedRoles) {
  char command[] = "region priority scope-a 10";
  char reply[160] = "untouched";
  EXPECT_FALSE(RegionPriorityCLI::handle(regions, command, reply, sizeof(reply), true));
  EXPECT_STREQ("untouched", reply);
  EXPECT_EQ(0u, cli("region prio scope-a 10", false).find("Err -"));
  EXPECT_EQ(nullptr, regions.getPriorityRegion());
}

TEST_F(RegionPriority, MatchesRealTransportCodesAndRejectsTampering) {
  cli("region prio #scope-a 10");
  auto preferred = scopedPacket("scope-a");
  auto other = scopedPacket("scope-b");
  EXPECT_EQ(0xFACE, preferred.transport_codes[0]);
  EXPECT_TRUE(regions.isPriorityPacket(&preferred));
  EXPECT_FALSE(regions.isPriorityPacket(&other));
  preferred.payload[3] ^= 1;
  EXPECT_FALSE(regions.isPriorityPacket(&preferred));
}

TEST_F(RegionPriority, DirectAndUnscopedPacketsNeverClaimTheReservation) {
  cli("region prio scope-a 10");
  for (auto route : {ROUTE_TYPE_FLOOD, ROUTE_TYPE_DIRECT, ROUTE_TYPE_TRANSPORT_DIRECT}) {
    auto packet = scopedPacket("scope-a");
    packet.header = route | (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT);
    EXPECT_FALSE(regions.isPriorityPacket(&packet));
  }
}

TEST_F(RegionPriority, DeniedRegionsAndMissingPrivateKeysCannotBeSelected) {
  regions.findByName("scope-a")->flags = REGION_DENY_FLOOD;
  EXPECT_EQ(0u, cli("region prio scope-a 10").find("Err -"));
  auto private_region = regions.putRegion("$private", 0);
  ASSERT_NE(nullptr, private_region);
  private_region->flags = 0;
  EXPECT_EQ(0u, cli("region prio $private 10").find("Err -"));
  EXPECT_EQ(nullptr, regions.getPriorityRegion());
}

TEST_F(RegionPriority, DenyingThePreferredRegionSuspendsPriority) {
  cli("region prio scope-a 10");
  auto packet = scopedPacket("scope-a");
  regions.findByName("scope-a")->flags = REGION_DENY_FLOOD;
  EXPECT_EQ(0, regions.getPriorityPercent());
  EXPECT_FALSE(regions.isPriorityPacket(&packet));
  EXPECT_EQ(" priority is scope-a, reserve 10% (flood denied)", cli("region prio"));
  regions.findByName("scope-a")->flags = 0;
  EXPECT_EQ(10, regions.getPriorityPercent());
  EXPECT_TRUE(regions.isPriorityPacket(&packet));
}

TEST_F(RegionPriority, PersistsWithoutChangingTheExistingRecordLayout) {
  cli("region prio scope-a 10");
  regions.setHomeRegion(regions.findByName("scope-b"));
  regions.setDefaultRegion(regions.findByName("scope-a"));
  regions.getWildcard().flags = REGION_DENY_FLOOD;
  MemoryFileSystem fs;
  auto bytes = saved(fs);
  ASSERT_EQ(10u + 2 * 164, bytes.size());  // legacy /regions2 layout
  EXPECT_EQ(regions.getPriorityRegion()->id, bytes[0] | (bytes[1] << 8));
  EXPECT_EQ(10, bytes[2]);
  TransportKeyStore loaded_keys;
  RegionMap loaded(loaded_keys);
  ASSERT_TRUE(loaded.load(&fs));
  ASSERT_NE(nullptr, loaded.getPriorityRegion());
  EXPECT_EQ("scope-a", std::string(loaded.getPriorityRegion()->name));
  EXPECT_EQ(10, loaded.getPriorityPercent());
  EXPECT_EQ("scope-b", std::string(loaded.getHomeRegion()->name));
  EXPECT_EQ("scope-a", std::string(loaded.getDefaultRegion()->name));
  EXPECT_EQ(REGION_DENY_FLOOD, loaded.getWildcard().flags);
}

TEST_F(RegionPriority, LegacyHeadersAndInvalidPriorityHeadersLoadDisabled) {
  MemoryFileSystem fs;
  auto bytes = saved(fs);
  for (auto header : {std::vector<uint8_t>{0, 0, 0},
                      std::vector<uint8_t>{1, 0, 51},
                      std::vector<uint8_t>{0xFF, 0xFF, 10}}) {
    std::copy(header.begin(), header.end(), bytes.begin());
    auto file = fs.open("/regions2", "w");
    ASSERT_EQ(bytes.size(), file.write(bytes.data(), bytes.size()));
    RegionMap loaded(keys);
    ASSERT_TRUE(loaded.load(&fs));
    EXPECT_EQ(2, loaded.getCount());
    EXPECT_EQ(nullptr, loaded.getPriorityRegion());
    EXPECT_EQ(0, loaded.getPriorityPercent());
  }
}

TEST_F(RegionPriority, RemovingTheSelectedRegionClearsPriority) {
  cli("region prio scope-a 10");
  ASSERT_TRUE(regions.removeRegion(*regions.findByName("scope-a")));
  EXPECT_EQ(nullptr, regions.getPriorityRegion());
  EXPECT_EQ(0, regions.getPriorityPercent());
}

TEST_F(RegionPriority, BulkReloadPreservesPriorityByStableRegionId) {
  cli("region prio scope-a 10");
  RegionMap temporary(keys);
  temporary.resetFrom(regions);
  auto old = regions.findByName("scope-a");
  auto rebuilt = temporary.putRegion(old->name, old->parent, old->id);
  ASSERT_NE(nullptr, rebuilt);
  rebuilt->flags = old->flags;
  EXPECT_EQ(10, temporary.getPriorityPercent());
  ASSERT_NE(nullptr, temporary.getPriorityRegion());
  EXPECT_EQ(old->id, temporary.getPriorityRegion()->id);
  temporary.clear();
  EXPECT_EQ(nullptr, temporary.getPriorityRegion());
  EXPECT_EQ(0, temporary.getPriorityPercent());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
