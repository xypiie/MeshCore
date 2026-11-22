#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "helpers/FloodTest.h"
#include "helpers/StaticPoolPacketManager.h"

namespace {

TEST(FloodCommand, ParsesOnlyLocalBoundedRequests) {
  FloodTest::Request request{};
  char reply[157] = "untouched";
  EXPECT_EQ(FloodTest::NotHandled, FloodTest::parse("flooding", true, request, reply, sizeof(reply)));
  EXPECT_STREQ("untouched", reply);
  for (auto text : {"flood 60 scope-a", "flood 60", "flood status", "flood stop"}) {
    EXPECT_EQ(FloodTest::Invalid, FloodTest::parse(text, false, request, reply, sizeof(reply)));
  }
  for (auto name : {"scope-a", "#test_zone", "$private", "123"}) {
    char command[100];
    snprintf(command, sizeof(command), "flood 60 %s", name);
    ASSERT_EQ(FloodTest::Start, FloodTest::parse(command, true, request, reply, sizeof(reply)));
    EXPECT_STREQ(name, request.region);
    EXPECT_EQ(60, request.seconds);
  }
  ASSERT_EQ(FloodTest::Start, FloodTest::parse(
      "flood --continuous 7200", true, request, reply, sizeof(reply)));
  EXPECT_EQ(7200u, request.seconds);
  EXPECT_FALSE(request.paced);
  EXPECT_EQ(FloodTest::Status, FloodTest::parse("flood status", true, request, reply, sizeof(reply)));
  EXPECT_EQ(FloodTest::Stop, FloodTest::parse("flood stop", true, request, reply, sizeof(reply)));
  for (auto text : {"flood", "flood 0", "flood 7201", "flood -1", "flood nan",
                    "flood 1.5", "flood 10x", "flood 999999999999", "flood 4294967297",
                    "flood 1 scope-a extra", "flood 1 abcdefghijklmnopqrstuvwxyz12345",
                    "flood scope-a 60", "flood status 1", "flood stop 1"}) {
    SCOPED_TRACE(text);
    EXPECT_EQ(FloodTest::Invalid, FloodTest::parse(text, true, request, reply, sizeof(reply)));
  }
}

TEST(FloodCommand, OmittingTheRegionClearsAPreviousScope) {
  FloodTest::Request request{};
  char reply[157];
  ASSERT_EQ(FloodTest::Start, FloodTest::parse("flood 60 scope-a", true, request, reply, sizeof(reply)));
  EXPECT_STREQ("scope-a", request.region);
  ASSERT_EQ(FloodTest::Start, FloodTest::parse("flood 60", true, request, reply, sizeof(reply)));
  EXPECT_STREQ("", request.region);
  EXPECT_EQ(60, request.seconds);
  ASSERT_EQ(FloodTest::Start, FloodTest::parse("flood   1   ", true, request, reply, sizeof(reply)));
  EXPECT_STREQ("", request.region);
  EXPECT_EQ(1, request.seconds);
}

class Clock : public mesh::MillisecondClock {
public:
  uint32_t now = 1000;
  unsigned long getMillis() override { return now; }
};

class Radio : public mesh::Radio {
  Clock& clock;
  uint32_t start = 0;
  uint32_t airtime = 0;
  bool sending = false;
public:
  bool busy = false, succeeds = true, completes = true;
  bool airtime_by_length = false;
  uint32_t fixed_airtime_ms = 50;
  std::vector<uint32_t> starts;
  std::vector<mesh::Packet> frames;
  explicit Radio(Clock& value) : clock(value) { }
  int recvRaw(uint8_t*, int) override { return 0; }
  uint32_t getEstAirtimeFor(int len) override {
    return airtime_by_length ? len : fixed_airtime_ms;
  }
  float packetScore(float, int) override { return 1; }
  bool isInRecvMode() const override { return !sending; }
  bool isReceiving() override { return busy; }
  bool startSendRaw(const uint8_t* bytes, int length) override {
    if (!succeeds) return false;
    mesh::Packet packet;
    if (!packet.readFrom(bytes, length)) return false;
    frames.push_back(packet);
    starts.push_back(clock.now);
    start = clock.now;
    airtime = getEstAirtimeFor(length);
    sending = true;
    return true;
  }
  bool isSendComplete() override { return sending && completes && clock.now - start >= airtime; }
  void onSendFinished() override { sending = false; }
};

class Dispatcher : public mesh::Dispatcher {
  FloodTest& job;
protected:
  mesh::DispatcherAction onRecvPacket(mesh::Packet*) override { return ACTION_RELEASE; }
  void logTx(mesh::Packet* packet, int) override { job.finished(packet, true, _ms->getMillis(), _radio->getEstAirtimeFor(packet->getRawLength())); }
  void logTxFail(mesh::Packet* packet, int) override { job.finished(packet, false, _ms->getMillis()); }
  float getAirtimeBudgetFactor() const override { return factor; }
  int getDutyCycleBurstPercent() const override { return burst; }
  unsigned long getDutyCycleWindowMs() const override { return window; }
public:
  float factor = 1;
  int burst = -1;
  uint32_t window = 3600000;
  Dispatcher(Radio& radio, Clock& clock, StaticPoolPacketManager& packets, FloodTest& flood)
    : mesh::Dispatcher(radio, clock, packets), job(flood) { }
};

class FloodProducer : public ::testing::Test {
protected:
  Clock clock;
  Radio radio{clock};
  StaticPoolPacketManager packets{32};
  FloodTest job;
  Dispatcher dispatcher{radio, clock, packets, job};
  bool scoped = true;

  void SetUp() override { dispatcher.begin(); }
  void queue() {
    if (!job.canQueue(clock.now) || packets.getFreeCount() <= 1) return;
    auto packet = dispatcher.obtainNewPacket();
    ASSERT_NE(nullptr, packet);
    packet->header = (scoped ? ROUTE_TYPE_TRANSPORT_FLOOD : ROUTE_TYPE_FLOOD) |
                     (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT);
    packet->setPathHashSizeAndCount(1, 0);
    packet->payload_len = MAX_PACKET_PAYLOAD;
    job.payload(packet->payload, packet->payload_len);
    packet->transport_codes[0] = scoped ? 0xBEEF : 0;
    packet->transport_codes[1] = 0;
    job.queued(packet);
    dispatcher.sendPacket(packet, 1);
  }
  void advance(uint32_t duration, uint32_t step = 1) {
    for (uint32_t i = 0; i < duration; i += step) {
      clock.now += step;
      job.expire(clock.now, packets);
      dispatcher.loop();
      job.expire(clock.now, packets);
      queue();
    }
  }
};

TEST_F(FloodProducer, StopsAtTheWallClockDeadlineAndDrainsTheActiveFrame) {
  ASSERT_TRUE(job.start(clock.now, 1, 1234));
  advance(1200);
  EXPECT_FALSE(job.busy());
  ASSERT_FALSE(radio.frames.empty());
  EXPECT_EQ(job.generated(), job.sent());
  EXPECT_TRUE(job.takeReport());
  EXPECT_FALSE(job.takeReport());
  for (size_t i = 0; i < radio.frames.size(); ++i) {
    const auto& packet = radio.frames[i];
    EXPECT_LT(radio.starts[i], 2000u);
    EXPECT_EQ(PAYLOAD_TYPE_RAW_CUSTOM, packet.getPayloadType());
    EXPECT_EQ(ROUTE_TYPE_TRANSPORT_FLOOD, packet.getRouteType());
    EXPECT_EQ(MAX_PACKET_PAYLOAD, packet.payload_len);
    EXPECT_EQ(0xBEEF, packet.transport_codes[0]);
    EXPECT_EQ(0, memcmp(packet.payload, "MC-DUTY", 7));
    if (i) EXPECT_NE(0, memcmp(packet.payload, radio.frames[i - 1].payload, MAX_PACKET_PAYLOAD));
  }
  EXPECT_EQ(32, packets.getFreeCount());
}

TEST_F(FloodProducer, UnscopedRequestsTransmitRawFramesWithoutTransportCodes) {
  FloodTest::Request request{};
  char reply[157];
  ASSERT_EQ(FloodTest::Start, FloodTest::parse("flood 1", true, request, reply, sizeof(reply)));
  scoped = request.region[0] != 0;
  ASSERT_TRUE(job.start(clock.now, request.seconds, 1234));
  advance(1200);
  ASSERT_FALSE(radio.frames.empty());
  for (const auto& packet : radio.frames) {
    EXPECT_EQ(ROUTE_TYPE_FLOOD, packet.getRouteType());
    EXPECT_FALSE(packet.hasTransportCodes());
    EXPECT_EQ(PAYLOAD_TYPE_RAW_CUSTOM, packet.getPayloadType());
    EXPECT_EQ(2 + MAX_PACKET_PAYLOAD, packet.getRawLength());
  }
  EXPECT_FALSE(job.busy());
  EXPECT_EQ(32, packets.getFreeCount());
}

TEST_F(FloodProducer, DoesNotAllocateAnotherFrameWhileOneIsQueuedOrTransmitting) {
  ASSERT_TRUE(job.start(clock.now, 60, 1));
  queue();
  EXPECT_FALSE(job.canQueue(clock.now));
  EXPECT_FALSE(job.start(clock.now, 1, 2));
  advance(20);
  EXPECT_EQ(1u, job.generated());
  EXPECT_EQ(31, packets.getFreeCount());
  job.stop(clock.now, packets);
  EXPECT_TRUE(job.busy());
  EXPECT_FALSE(job.start(clock.now, 1, 2));
  advance(100);
  EXPECT_FALSE(job.busy());
  EXPECT_TRUE(job.start(clock.now, 1, 2));
}

TEST_F(FloodProducer, CancellationRemovesOnlyTheQueuedTestFrame) {
  auto ordinary = dispatcher.obtainNewPacket();
  ASSERT_NE(nullptr, ordinary);
  ordinary->header = ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  ordinary->setPathHashSizeAndCount(1, 0);
  ordinary->payload_len = 4;
  memset(ordinary->payload, 0, 4);
  dispatcher.sendPacket(ordinary, 0);
  ASSERT_TRUE(job.start(clock.now, 1, 1));
  queue();
  EXPECT_EQ(2, packets.getOutboundTotal());
  job.stop(clock.now, packets);
  EXPECT_EQ(1, packets.getOutboundTotal());
  EXPECT_EQ(ordinary, packets.getOutboundByIdx(0));
  EXPECT_FALSE(job.busy());
}

TEST_F(FloodProducer, DeadlineCancelsTrafficWaitingForTheChannel) {
  radio.busy = true;
  ASSERT_TRUE(job.start(clock.now, 1, 1));
  advance(1200);
  EXPECT_TRUE(radio.starts.empty());
  EXPECT_FALSE(job.busy());
  EXPECT_EQ(32, packets.getFreeCount());
}

TEST_F(FloodProducer, OriginalBudgetRemainsInControlAndIsNotResetByStartingAJob) {
  dispatcher.factor = 9;
  dispatcher.window = 1000;  // Short observation window only in this test.
  dispatcher.begin();
  const auto budget = dispatcher.getRemainingTxBudget();
  ASSERT_TRUE(job.start(clock.now, 5, 1));
  EXPECT_EQ(budget, dispatcher.getRemainingTxBudget());
  advance(5200);
  EXPECT_GT(job.sent(), 0u);
  EXPECT_LT(job.sent(), 30u);
  EXPECT_FALSE(job.busy());
}

TEST_F(FloodProducer, WaitsForTheFullAirtimeOfTheQueuedFrame) {
  radio.airtime_by_length = true;
  dispatcher.factor = 9;
  dispatcher.window = 2000;  // A full bucket holds 200 ms in this test.
  dispatcher.begin();
  ASSERT_TRUE(job.start(clock.now, 5, 1));
  advance(1700);

  ASSERT_EQ(1u, radio.starts.size());
  EXPECT_EQ(1, packets.getOutboundTotal());
  EXPECT_EQ(190, radio.frames.front().getRawLength());

  advance(400);
  ASSERT_EQ(2u, radio.starts.size());
  // The first 190 ms frame leaves 10 ms. Refilling the missing 180 ms
  // requires 1800 ms of silence at 10%.
  EXPECT_GE(radio.starts[1] - radio.starts[0], 1990u);
  EXPECT_LE(radio.starts[1] - radio.starts[0], 1992u);
}

TEST_F(FloodProducer, AdmitsAShortFrameUsingItsOwnAirtime) {
  radio.airtime_by_length = true;
  dispatcher.factor = 9;
  dispatcher.window = 1000;  // A full bucket holds less than half a max-size frame.
  dispatcher.begin();
  ASSERT_TRUE(job.start(clock.now, 1, 1));
  queue();  // Queue a long, lower-priority frame before the short one.

  auto packet = dispatcher.obtainNewPacket();
  ASSERT_NE(nullptr, packet);
  packet->header = ROUTE_TYPE_DIRECT | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
  packet->setPathHashSizeAndCount(1, 0);
  packet->payload_len = 4;
  memset(packet->payload, 0, 4);
  dispatcher.sendPacket(packet, 0);

  advance(20);
  ASSERT_EQ(1u, radio.frames.size());
  EXPECT_EQ(PAYLOAD_TYPE_ACK, radio.frames.front().getPayloadType());
  EXPECT_EQ(1, packets.getOutboundTotal());
}

TEST_F(FloodProducer, FailedStartsAndTimeoutsReleaseTheOutstandingFrame) {
  radio.succeeds = false;
  ASSERT_TRUE(job.start(clock.now, 1, 1));
  advance(20);
  EXPECT_GT(job.failed(), 0u);
  job.stop(clock.now, packets);
  radio.succeeds = true;
  radio.completes = false;
  ASSERT_TRUE(job.start(clock.now, 1, 2));
  advance(1200);
  EXPECT_GT(job.failed(), 0u);
  EXPECT_FALSE(job.busy());
  EXPECT_EQ(32, packets.getFreeCount());
}

TEST_F(FloodProducer, PacedFramesWaitOneAirtime) {
  ASSERT_TRUE(job.start(clock.now, 7200, 1));
  EXPECT_EQ(7200000u, job.duration());
  advance(400);
  ASSERT_GE(radio.starts.size(), 3u);
  for (size_t i = 1; i < radio.starts.size(); ++i) {
    EXPECT_GE(radio.starts[i] - radio.starts[i - 1], 100u);
    EXPECT_LE(radio.starts[i] - radio.starts[i - 1], 102u);
  }
}

TEST_F(FloodProducer, ContinuousModeQueuesImmediatelyAfterTx) {
  ASSERT_TRUE(job.start(clock.now, 1, 1, false));
  advance(250);
  ASSERT_GE(radio.starts.size(), 3u);
  EXPECT_LT(radio.starts[1] - radio.starts[0], 100u);
}

TEST_F(FloodProducer, DurationValidationAndMillisWrap) {
  EXPECT_FALSE(job.start(clock.now, 0, 1));
  EXPECT_FALSE(job.start(clock.now, 7201, 1));
  EXPECT_TRUE(job.start(UINT32_MAX - 499, 1, 1));
  mesh::Packet packet;
  job.queued(&packet);
  job.finished(&packet, true, UINT32_MAX - 9, 20);
  EXPECT_FALSE(job.canQueue(9));
  EXPECT_TRUE(job.canQueue(10));
  EXPECT_FALSE(job.expire(0, packets));
  EXPECT_TRUE(job.expire(500, packets));
  EXPECT_EQ(1000u, job.elapsed(500));
}

TEST_F(FloodProducer, FullBurstStaysWithinEveryRollingHourUnderTwoHoursOfLoad) {
  constexpr uint32_t hour = 3600000;
  constexpr uint32_t cap = 360000;
  constexpr uint32_t airtime = 1000;
  dispatcher.factor = 9;
  dispatcher.burst = 100;
  dispatcher.begin();
  radio.fixed_airtime_ms = airtime;
  const uint32_t start = clock.now;
  ASSERT_TRUE(job.start(start, 7200, 1, false));
  advance(2 * hour + airtime, 10);

  ASSERT_GT(radio.starts.size(), 690u);
  EXPECT_LT(radio.starts.front() - start, 100u);
  // A maximum can only occur where a window endpoint meets a packet edge.
  for (uint32_t tx : radio.starts) {
    for (uint32_t edge : {tx, tx + airtime, tx + hour, tx + airtime + hour}) {
      uint32_t used = 0;
      for (uint32_t candidate : radio.starts) {
        const int64_t left = std::max<int64_t>(candidate, (int64_t)edge - hour);
        const int64_t right = std::min<int64_t>(candidate + airtime, edge);
        if (right > left) used += right - left;
      }
      ASSERT_LE(used, cap) << "window ending at " << edge;
    }
  }

  advance(hour + 60000, 1000);
  EXPECT_EQ(cap, dispatcher.getRemainingTxBudget());
  const auto previous = radio.starts.size();
  ASSERT_TRUE(job.start(clock.now, 2, 2, false));
  advance(1500, 10);
  EXPECT_EQ(previous + 2, radio.starts.size());
}

TEST_F(FloodProducer, ZeroBurstPacesEachFrameAtTheConfiguredRate) {
  dispatcher.factor = 9;
  dispatcher.burst = 0;
  dispatcher.begin();
  ASSERT_TRUE(job.start(clock.now, 2, 1, false));
  advance(2000);
  ASSERT_EQ(4u, radio.starts.size());
  for (size_t i = 1; i < radio.starts.size(); ++i) {
    EXPECT_EQ(500u, radio.starts[i] - radio.starts[i - 1]);
  }
}

TEST_F(FloodProducer, HalfBurstSpends180SecondsThenPacesTheRemaining57Minutes) {
  constexpr uint32_t airtime = 1000;
  constexpr uint32_t burst_packets = 180;
  dispatcher.factor = 9;
  dispatcher.burst = 50;
  dispatcher.begin();
  radio.fixed_airtime_ms = airtime;
  const uint32_t start = clock.now;
  ASSERT_TRUE(job.start(start, 7200, 1, false));
  advance(190000, 10);
  ASSERT_EQ(burst_packets, radio.starts.size());
  for (uint32_t i = 1; i < burst_packets; ++i) {
    EXPECT_LE(radio.starts[i] - radio.starts[i - 1], airtime + 10);
  }
  advance(3590000 - 190000, 10);
  ASSERT_GT(radio.starts.size(), 350u);
  ASSERT_LT(radio.starts.size(), 360u);
  for (size_t i = burst_packets; i < radio.starts.size(); ++i) {
    EXPECT_EQ(19000u, radio.starts[i] - radio.starts[i - 1]);
  }
  EXPECT_GT(radio.starts.back() - start, 3500000u);

  const auto before_expiry = radio.starts.size();
  advance(180000, 10);
  // As old burst packets expire, their separate quota can be used again.
  EXPECT_GT(radio.starts.size() - before_expiry, 100u);
}

TEST(TxAirtimeWindow, KeepsTheOldestOverlappingBucketAcrossMillisWrap) {
  constexpr uint32_t start = UINT32_MAX - 500;
  mesh::TxAirtimeWindow window;
  window.begin(start, 60000);
  window.advance(start + 999);
  window.record(100, true);
  window.record(20);
  window.advance(start + 60998);
  EXPECT_EQ(120u, window.used());
  EXPECT_EQ(100u, window.burstUsed());
  window.advance(start + 61000);
  EXPECT_EQ(0u, window.used());
  EXPECT_EQ(0u, window.burstUsed());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
