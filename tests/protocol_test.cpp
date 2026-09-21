#include <algorithm>
#include <vector>

#include "blackhole/blackhole.hpp"

#include "support/fixtures.hpp"
#include "support/testkit.hpp"

using namespace bhg;
using namespace bhg::test;

namespace {

std::vector<std::byte> frame_bytes(const Frame& f) {
  Writer w(1u << 20);
  encode_frame(w, f);
  return w.take();
}

void decode_expect_ok(FrameDecoder& d, Frame& out) {
  const Outcome o = d.next(out);
  BHG_CHECK(is_affirmative(o));
}

}  // namespace

BHG_TEST(protocol, frame_roundtrip) {
  Frame f;
  f.type = MessageType::EvaluateRequest;
  f.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  const std::vector<std::byte> bytes = frame_bytes(f);
  BHG_CHECK_EQ(bytes.size(), kFrameHeaderSize + 3u);

  FrameDecoder d;
  BHG_REQUIRE(is_affirmative(d.feed(std::span<const std::byte>(bytes.data(), bytes.size()))));
  Frame out;
  decode_expect_ok(d, out);
  BHG_CHECK(out.type == f.type);
  BHG_CHECK(out.flags == 0u);
  BHG_CHECK(out.payload == f.payload);
  BHG_CHECK(is_affirmative(d.finish()));
}

BHG_TEST(protocol, frame_decoder_accepts_every_split_of_the_stream) {
  Frame a;
  a.type = MessageType::HelloRequest;
  a.payload = {std::byte{7}, std::byte{8}, std::byte{9}, std::byte{10}};
  Frame b;
  b.type = MessageType::StatsRequest;
  b.payload = {std::byte{1}};
  std::vector<std::byte> stream = frame_bytes(a);
  const std::vector<std::byte> second = frame_bytes(b);
  stream.insert(stream.end(), second.begin(), second.end());

  for (std::size_t split = 0; split <= stream.size(); ++split) {
    FrameDecoder d;
    BHG_REQUIRE(is_affirmative(
        d.feed(std::span<const std::byte>(stream.data(), split))));
    Frame out;
    std::vector<Frame> frames;
    while (is_affirmative(d.next(out))) frames.push_back(out);
    BHG_REQUIRE(is_affirmative(
        d.feed(std::span<const std::byte>(stream.data() + split, stream.size() - split))));
    while (is_affirmative(d.next(out))) frames.push_back(out);
    BHG_REQUIRE(frames.size() == 2u);
    BHG_CHECK(frames[0].payload == a.payload);
    BHG_CHECK(frames[1].payload == b.payload);
    BHG_CHECK(is_affirmative(d.finish()));
  }
}

BHG_TEST(protocol, every_truncated_prefix_is_incomplete_not_accepted) {
  Frame f;
  f.type = MessageType::RestoreRequest;
  f.payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
  const std::vector<std::byte> bytes = frame_bytes(f);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    FrameDecoder d;
    BHG_REQUIRE(is_affirmative(d.feed(std::span<const std::byte>(bytes.data(), cut))));
    Frame out;
    BHG_CHECK(d.next(out) == Outcome::NotFound);
    BHG_CHECK(!d.failed());
    // An empty feed has nothing buffered; any non-empty prefix is an incomplete frame.
    BHG_CHECK(d.finish() == (cut == 0 ? Outcome::Ok : Outcome::Malformed));
    // Completing the frame must still succeed: a partial frame is not an error.
    FrameDecoder d2;
    BHG_REQUIRE(is_affirmative(d2.feed(std::span<const std::byte>(bytes.data(), cut))));
    BHG_REQUIRE(is_affirmative(d2.feed(
        std::span<const std::byte>(bytes.data() + cut, bytes.size() - cut))));
    Frame complete;
    BHG_CHECK(is_affirmative(d2.next(complete)));
    BHG_CHECK(complete.payload == f.payload);
  }
}

BHG_TEST(protocol, corrupt_frames_fail_stickily) {
  Frame f;
  f.type = MessageType::EvaluateRequest;
  f.payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  const std::vector<std::byte> good = frame_bytes(f);

  const auto expect_failure = [&](const std::vector<std::byte>& bytes, Outcome expected,
                                  const char* what) {
    FrameDecoder d;
    const Outcome fo = d.feed(std::span<const std::byte>(bytes.data(), bytes.size()));
    if (!is_affirmative(fo)) {
      BHG_CHECK_EQ(static_cast<int>(fo), static_cast<int>(expected));
      BHG_CHECK(d.failed());
      // Sticky: the failure repeats and no frame is ever produced.
      Frame out;
      BHG_CHECK(d.next(out) == expected);
      BHG_CHECK(d.finish() == expected);
      (void)what;
      return;
    }
    Frame out;
    const Outcome no = d.next(out);
    BHG_CHECK_EQ(static_cast<int>(no), static_cast<int>(expected));
    if (!is_affirmative(no)) {
      BHG_CHECK(d.failed());
      BHG_CHECK(d.next(out) == expected);
    }
    (void)what;
  };

  std::vector<std::byte> bad = good;
  bad[0] ^= std::byte{0xFF};
  expect_failure(bad, Outcome::Corrupt, "magic");

  bad = good;
  bad[4] = std::byte{99};
  expect_failure(bad, Outcome::Unsupported, "version");

  bad = good;
  bad[6] = std::byte{0xEE};
  bad[7] = std::byte{0xEE};
  expect_failure(bad, Outcome::Invalid, "type");

  bad = good;
  bad[8] = std::byte{0x01};
  expect_failure(bad, Outcome::Invalid, "flags");

  bad = good;
  bad[16] ^= std::byte{0x01};
  expect_failure(bad, Outcome::Corrupt, "integrity");

  bad = good;
  bad[20] ^= std::byte{0x40};
  expect_failure(bad, Outcome::Corrupt, "payload integrity");

  // Oversized declared length is refused before any allocation.
  bad = good;
  bad.assign(kFrameHeaderSize, std::byte{0});
  Writer w(64);
  w.u32(0x31474842u);
  w.u16(1);
  w.u16(1);
  w.u32(0);
  w.u32(0xFFFFFFFFu);
  w.u32(0);
  bad = w.take();
  expect_failure(bad, Outcome::Oversized, "declared length");
}

BHG_TEST(protocol, decoder_refuses_to_buffer_beyond_the_frame_limit) {
  FrameDecoder d(64);
  const std::vector<std::byte> flood(4096, std::byte{0xAA});
  const Outcome o = d.feed(std::span<const std::byte>(flood.data(), flood.size()));
  BHG_CHECK(o == Outcome::Oversized);
  BHG_CHECK(d.failed());
  BHG_CHECK_EQ(d.failure(), Outcome::Oversized);
}

BHG_TEST(protocol, maximum_size_payload_is_accepted_and_refused_at_the_boundary) {
  Frame f;
  f.type = MessageType::LocalizeRequest;
  f.payload.assign(1024, std::byte{0x5A});
  const std::vector<std::byte> bytes = frame_bytes(f);
  FrameDecoder ok(1024);
  BHG_REQUIRE(is_affirmative(ok.feed(std::span<const std::byte>(bytes.data(), bytes.size()))));
  Frame out;
  BHG_CHECK(is_affirmative(ok.next(out)));
  BHG_CHECK_EQ(out.payload.size(), 1024u);

  // A decoder whose limit is one byte below the payload refuses the frame; the
  // refusal may happen while feeding (bounded buffering) or while decoding, but it
  // must happen and it must be explicit.
  FrameDecoder tight(1023);
  const Outcome fo = tight.feed(std::span<const std::byte>(bytes.data(), bytes.size()));
  if (is_affirmative(fo)) {
    BHG_CHECK(tight.next(out) == Outcome::Oversized);
  } else {
    BHG_CHECK_EQ(static_cast<int>(fo), static_cast<int>(Outcome::Oversized));
  }
  BHG_CHECK(tight.failed());
  BHG_CHECK_EQ(tight.buffered() <= static_cast<std::size_t>(1023) + kFrameHeaderSize, true);
}

BHG_TEST(protocol, hello_and_response_roundtrip) {
  HelloRequest req;
  req.token.session = SessionId{};
  req.token.request = RequestId{4};
  req.token.seq = SessionSequence{0};
  req.token.client_boot = BootId{11};
  req.token.client_incarnation = IncarnationId{12};
  req.client_name = "observer-a";
  req.label = Provenance::Synthetic;

  Writer w(512);
  encode(w, req);
  BHG_REQUIRE(w.ok());
  Reader r(w.span());
  HelloRequest out;
  BHG_REQUIRE(is_affirmative(decode(r, out)));
  BHG_REQUIRE(is_affirmative(r.finish()));
  BHG_CHECK(out.client_name == req.client_name);
  BHG_CHECK(out.token.client_boot == req.token.client_boot);

  // Long names are truncated by the encoder and refused by the decoder if too long.
  Writer w2(512);
  HelloRequest long_req = req;
  long_req.client_name.assign(500, 'x');
  encode(w2, long_req);
  BHG_CHECK(!w2.ok());
}

BHG_TEST(protocol, message_decoders_are_total) {
  // A LocalizeRequest with a declared probe count larger than the payload.
  Writer w(64);
  encode(w, SessionToken{});
  encode(w, path_scope(PathId{1}));
  encode(w, make_gens(1, 1, 1, 1));
  w.u32(4);
  w.u32(kMaxProbesPerRequest);
  const std::vector<std::byte> bytes = w.take();
  Reader r(std::span<const std::byte>(bytes.data(), bytes.size()));
  LocalizeRequest req;
  BHG_CHECK(decode(r, req) == Outcome::Malformed);

  // An invalid enum in an EvaluateResponse.
  Writer w2(64);
  w2.u8(static_cast<std::uint8_t>(Outcome::Ok));
  encode(w2, Decision{});
  Reader r2(w2.span());
  EvaluateResponse resp;
  BHG_CHECK(!is_affirmative(decode(r2, resp)));

  // Trailing bytes after a valid StatsRequest.
  Writer w3(64);
  encode(w3, StatsRequest{});
  w3.u8(0);
  Reader r3(w3.span());
  StatsRequest sr;
  BHG_REQUIRE(is_affirmative(decode(r3, sr)));
  BHG_CHECK(r3.finish() == Outcome::Malformed);

  // Oversized lineage page.
  Writer w4(64);
  encode(w4, SessionToken{});
  w4.u32(kMaxLineagePage + 1u);
  Reader r4(w4.span());
  LineageRequest lr;
  BHG_CHECK(decode(r4, lr) == Outcome::Oversized);

  // Corrupt a StatisticsResponse and confirm the enum/domain checks hold.
  StatsResponse good;
  good.outcome = Outcome::Ok;
  Writer w5(1024);
  encode(w5, good);
  const std::vector<std::byte> stats_bytes = w5.take();
  for (std::size_t cut = 0; cut < stats_bytes.size(); cut += 3) {
    Reader rc(std::span<const std::byte>(stats_bytes.data(), cut));
    StatsResponse partial;
    const Outcome o = decode(rc, partial);
    if (is_affirmative(o)) {
      BHG_CHECK(is_affirmative(rc.finish()) == false);
    }
  }
}

BHG_TEST(protocol, session_table_authority_rules) {
  FencePolicy policy = default_policy();
  SessionTable table(policy);
  SessionId id{};
  BHG_REQUIRE(is_affirmative(
      table.establish(BootId{1}, IncarnationId{2}, Provenance::Synthetic, WallNs{100}, id)));
  BHG_CHECK(!id.is_nil());
  BHG_CHECK_EQ(table.size(), 1u);

  ReasonCode reason = ReasonCode::None;
  BHG_CHECK(is_affirmative(table.validate(id, BootId{1}, IncarnationId{2}, SessionSequence{1}, reason)));
  // Replaying the same sequence is refused.
  BHG_CHECK(table.validate(id, BootId{1}, IncarnationId{2}, SessionSequence{1}, reason) ==
            Outcome::Rejected);
  BHG_CHECK(reason == ReasonCode::ReplayedSequence);
  // Skipping ahead is refused.
  BHG_CHECK(table.validate(id, BootId{1}, IncarnationId{2}, SessionSequence{9}, reason) ==
            Outcome::Rejected);
  BHG_CHECK(reason == ReasonCode::WindowViolation);
  // A different boot/incarnation may not act under this session.
  BHG_CHECK(table.validate(id, BootId{7}, IncarnationId{2}, SessionSequence{2}, reason) ==
            Outcome::Rejected);
  BHG_CHECK(reason == ReasonCode::SourceIncarnationChanged);
  // The refused attempts must not have advanced the sequence.
  BHG_CHECK(is_affirmative(table.validate(id, BootId{1}, IncarnationId{2}, SessionSequence{2}, reason)));

  // An unknown session id is refused.
  BHG_CHECK(table.validate(SessionId{12345}, BootId{1}, IncarnationId{2}, SessionSequence{3},
                           reason) == Outcome::Rejected);

  // Capacity is bounded.
  FencePolicy small = default_policy();
  small.max_sessions = 2;
  SessionTable tiny(small);
  SessionId a{};
  SessionId b{};
  SessionId c{};
  BHG_CHECK(is_affirmative(tiny.establish(BootId{1}, IncarnationId{1}, Provenance::Synthetic,
                                          WallNs{1}, a)));
  BHG_CHECK(is_affirmative(tiny.establish(BootId{2}, IncarnationId{2}, Provenance::Synthetic,
                                          WallNs{1}, b)));
  BHG_CHECK(tiny.establish(BootId{3}, IncarnationId{3}, Provenance::Synthetic, WallNs{1}, c) ==
            Outcome::Exhausted);
  // Releasing frees a slot.
  tiny.release(a);
  BHG_CHECK(is_affirmative(tiny.establish(BootId{3}, IncarnationId{3}, Provenance::Synthetic,
                                          WallNs{1}, c)));
  BHG_CHECK(tiny.validate(a, BootId{1}, IncarnationId{1}, SessionSequence{1}, reason) ==
            Outcome::Rejected);
}

BHG_TEST(protocol, session_identities_are_distinct_across_boots) {
  SessionTable table(default_policy());
  SessionId a{};
  SessionId b{};
  BHG_REQUIRE(is_affirmative(
      table.establish(BootId{1}, IncarnationId{1}, Provenance::Synthetic, WallNs{1}, a)));
  BHG_REQUIRE(is_affirmative(
      table.establish(BootId{1}, IncarnationId{2}, Provenance::Synthetic, WallNs{1}, b)));
  BHG_CHECK(a != b);
  SessionId nil{};
  BHG_CHECK(table.establish(BootId{}, IncarnationId{1}, Provenance::Synthetic, WallNs{1}, nil) ==
            Outcome::Invalid);
}
