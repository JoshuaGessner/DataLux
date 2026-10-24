// Session layer (C13a): the receiver learns everything but the grid from the frames themselves.
#include <fileflow/session.h>
#include <fileflow/sim/channel.h>

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using namespace fileflow;

namespace {

std::vector<std::uint8_t> Payload(std::size_t n, std::uint64_t seed = 31) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(seed);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

const FrameLayout& Layout() {
    static const FrameLayout layout =
        FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{}).value();
    return layout;
}

std::vector<double> Clean(const CellMatrix& m) {
    sim::ChannelConfig cfg;
    cfg.read_noise_sigma = 0.0;
    sim::Channel ch(cfg);
    return ch.Apply(m);
}

}  // namespace

TEST(Session, ReceiverNeedsOnlyTheGrid) {
    const auto payload = Payload(40000);
    TransmitConfig tc;
    tc.fec_nsym = 24;  // NOT the receiver's old hard-coded 32: it must read this from the header
    auto tx = TransmitSession::Create(Layout(), payload, "only-grid.bin", 0x600D, tc);
    ASSERT_TRUE(tx.ok()) << ErrorName(tx.error());
    auto rx = ReceiveSession::Create(Layout());
    ASSERT_TRUE(rx.ok());

    CellMatrix frame(120, 200);
    for (int i = 0; i < 10000 && !rx.value().complete(); ++i) {
        ASSERT_TRUE(tx.value().Next(&frame).ok());
        rx.value().Ingest(Clean(frame), i);
    }
    ASSERT_TRUE(rx.value().complete());
    ASSERT_NE(rx.value().manifest(), nullptr);
    EXPECT_EQ(rx.value().manifest()->file_name, "only-grid.bin");
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload);
    EXPECT_EQ(rx.value().stats().header_fail, 0u);
    EXPECT_EQ(rx.value().stats().fec_fail, 0u);
}

TEST(Session, SymbolsReadBeforeTheManifestAreNotWasted) {
    // The receiver joins AFTER the first manifest frame and before the second. Everything it
    // reads in between must count: on a clean channel the data is all in hand before the second
    // manifest, so the transfer must complete ON that manifest frame, with no symbol read twice.
    const auto payload = Payload(60000);
    TransmitConfig tc;
    tc.manifest_interval = 64;
    auto tx = TransmitSession::Create(Layout(), payload, "late.bin", 0x1A7E, tc);
    ASSERT_TRUE(tx.ok());
    auto rx = ReceiveSession::Create(Layout());
    ASSERT_TRUE(rx.ok());

    CellMatrix frame(120, 200);
    ASSERT_TRUE(tx.value().Next(&frame).ok());  // the manifest at sequence 0: missed

    std::uint64_t seen = 0;
    bool buffered_any = false;
    FrameOutcome last = FrameOutcome::kUnreadable;
    while (!rx.value().complete() && seen < 10000) {
        ASSERT_TRUE(tx.value().Next(&frame).ok());
        last = rx.value().Ingest(Clean(frame), static_cast<std::int64_t>(seen));
        buffered_any = buffered_any || last == FrameOutcome::kBuffered;
        ++seen;
    }
    ASSERT_TRUE(rx.value().complete());
    EXPECT_TRUE(buffered_any);
    EXPECT_EQ(last, FrameOutcome::kManifest);
    EXPECT_EQ(seen, 64u) << "sequences 1..64: complete on the second manifest, not later";
    EXPECT_EQ(rx.value().stats().buffer_dropped, 0u);
    // Nothing the channel delivered before the manifest was thrown away.
    EXPECT_DOUBLE_EQ(rx.value().receiver()->overhead(), 0.0);
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload);
}

TEST(Session, AForeignTransmitterInViewCannotCorruptTheTransfer) {
    // THREAT-MODEL T8 at the session layer: two transmitters' frames interleaved. Whichever
    // manifest arrives first owns the session and the other's frames are refused, including the
    // ones buffered before either manifest was read.
    const auto a = Payload(30000, 1);
    const auto b = Payload(30000, 2);
    auto tx_a = TransmitSession::Create(Layout(), a, "a.bin", 0xAAAA);
    auto tx_b = TransmitSession::Create(Layout(), b, "b.bin", 0xBBBB);
    ASSERT_TRUE(tx_a.ok() && tx_b.ok());
    auto rx = ReceiveSession::Create(Layout());
    ASSERT_TRUE(rx.ok());

    CellMatrix fa(120, 200), fb(120, 200);
    // B's first frame (its manifest) is skipped, so A's manifest is the first one read.
    ASSERT_TRUE(tx_b.value().Next(&fb).ok());
    for (int i = 0; i < 20000 && !rx.value().complete(); ++i) {
        ASSERT_TRUE(tx_a.value().Next(&fa).ok());
        ASSERT_TRUE(tx_b.value().Next(&fb).ok());
        rx.value().Ingest(Clean(fb), 2 * i);
        rx.value().Ingest(Clean(fa), 2 * i + 1);
    }
    ASSERT_TRUE(rx.value().complete());
    EXPECT_GT(rx.value().stats().foreign, 0u);
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), a);
}

TEST(Session, StateRatesAreSeparatedNotConflated) {
    // Camera at 2x the state rate, display dropping every 10th state: the three rates EXP-006
    // needs come out separately and correctly.
    const auto payload = Payload(200000);
    auto tx = TransmitSession::Create(Layout(), payload, "rate.bin", 0x7A7E);
    ASSERT_TRUE(tx.ok());
    auto rx = ReceiveSession::Create(Layout());
    ASSERT_TRUE(rx.ok());

    constexpr std::int64_t kStateNs = 1'000'000'000 / 30;  // transmitter submits at 30/s
    CellMatrix frame(120, 200);
    std::int64_t t = 0;
    for (int s = 0; s < 61; ++s) {
        ASSERT_TRUE(tx.value().Next(&frame).ok());
        if (s % 10 == 5) {  // never presented: the camera sees the previous state again
            t += kStateNs;
            continue;
        }
        const auto samples = Clean(frame);
        rx.value().Ingest(samples, t);
        rx.value().Ingest(samples, t + kStateNs / 2);  // second capture of the same state
        t += kStateNs;
    }
    const ReceiveStats& st = rx.value().stats();
    EXPECT_EQ(st.repeated_sequences, st.distinct_sequences);
    EXPECT_EQ(st.distinct_sequences, 55u);
    EXPECT_NEAR(st.submission_rate(), 30.0, 1.0);
    EXPECT_LT(st.distinct_rate(), st.submission_rate());
}

TEST(Session, HeaderErasuresRescueFramesABlindDecodeLoses) {
    // F50. Erase most of every copy of a band of header bytes -- the pattern a dead stripe across
    // the header band produces -- and the frame must still be read.
    const auto payload = Payload(20000);
    auto tx = TransmitSession::Create(Layout(), payload, "erase.bin", 0xE5);
    ASSERT_TRUE(tx.ok());
    auto rx = ReceiveSession::Create(Layout());
    ASSERT_TRUE(rx.ok());

    CellMatrix frame(120, 200);
    ASSERT_TRUE(tx.value().Next(&frame).ok());
    auto samples = Clean(frame);

    const auto& hdr_cells = Layout().header_cells();
    const std::size_t period = HeaderCodec{}.coded_size() * 8;
    // Every copy of 22 whole PARITY bytes unreadable: past the 16 blind errors nsym=32 can locate
    // and inside the 32 erasures it can fill. Parity rather than the plain header because a
    // first-frame header is mostly zero bytes, and an all-abstaining vote reads zero -- a blind
    // decode would then "succeed" by luck and the test would prove nothing.
    const std::size_t lo = FrameHeader::kPlainSize * 8;
    const std::size_t hi = lo + 22 * 8;
    for (std::size_t i = 0; i < hdr_cells.size(); ++i) {
        if (i % period >= lo && i % period < hi) samples[hdr_cells[i]] = std::nan("");
    }
    const FrameOutcome o = rx.value().Ingest(samples, 0);
    EXPECT_EQ(o, FrameOutcome::kManifest) << FrameOutcomeName(o);
    EXPECT_EQ(rx.value().stats().header_rescued_by_erasures, 1u);
}

TEST(Session, ImpossibleCodeRateIsRefusedNotTrusted) {
    TransmitConfig tc;
    tc.fec_nsym = 255;
    EXPECT_FALSE(TransmitSession::Create(Layout(), Payload(1000), "x", 1, tc).ok());
}
