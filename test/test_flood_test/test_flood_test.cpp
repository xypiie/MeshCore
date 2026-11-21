#include <gtest/gtest.h>
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
  bool sending = false;
public:
  bool busy = false, succeeds = true, completes = true;
  std::vector<uint32_t> starts;
  std::vector<mesh::Packet> frames;
  explicit Radio(Clock& value) : clock(value) { }
  int recvRaw(uint8_t*, int) override { return 0; }
  uint32_t getEstAirtimeFor(int) override { return 50; }
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
    sending = true;
    return true;
  }
  bool isSendComplete() override { return sending && completes && clock.now - start >= 50; }
  void onSendFinished() override { sending = false; }
};

class Dispatcher : public mesh::Dispatcher {
  FloodTest& job;
protected:
  mesh::DispatcherAction onRecvPacket(mesh::Packet*) override { return ACTION_RELEASE; }
  void logTx(mesh::Packet* packet, int) override { job.finished(packet, true, _ms->getMillis(), _radio->getEstAirtimeFor(packet->getRawLength())); }
  void logTxFail(mesh::Packet* packet, int) override { job.finished(packet, false, _ms->getMillis()); }
  float getAirtimeBudgetFactor() const override { return factor; }
  unsigned long getDutyCycleWindowMs() const override { return window; }
public:
  float factor = 1;
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
  void advance(uint32_t duration) {
    for (uint32_t i = 0; i < duration; ++i) {
      ++clock.now;
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

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
