// End-to-end: file -> frames -> optical channel -> decode chain -> verified file.
//
// The identity-channel case is the simulator's primary self-validation (ADR-0010): with no
// impairments, decoding MUST be perfect. A failure here is a decoder bug, never a channel
// effect -- which is exactly why this test exists before any impairment is tuned.
//
// Driven through the session layer (C13a), the same code ffsim, ffreplay and the phone run, and
// the receiver is given NOTHING but the grid: the code rate comes from each header and the
// manifest from the frames (ADR-0015). This file used to hand the receiver the transmitter's
// manifest object, which is the one thing a real receiver can never have.
#include <fileflow/session.h>
#include <fileflow/sim/channel.h>

#include <gtest/gtest.h>

#include <vector>

using namespace fileflow;

namespace {

std::vector<std::uint8_t> Payload(std::size_t n, std::uint64_t seed = 2026) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(seed);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

struct RunStats {
    bool verified = false;
    Error failure = Error::kNone;
    std::uint64_t presented = 0;
    std::uint64_t header_ok = 0;
    std::uint64_t header_fail = 0;
    std::uint64_t erasures = 0;
    double fountain_overhead = 0.0;
};

// One complete transfer through the real chain under a configured channel.
RunStats RunTransfer(const std::vector<std::uint8_t>& payload, sim::ChannelConfig cfg,
                     std::size_t fec_nsym = 32, std::uint64_t max_frames = 400000) {
    const GridGeometry geom{120, 200};
    const FrameLayout layout = FrameLayout::Create(geom, LayoutConfig{}).value();
    RunStats st;

    TransmitConfig tc;
    tc.fec_nsym = fec_nsym;
    auto tx_r = TransmitSession::Create(layout, payload, "e2e.bin", 0xE2EE2E, tc);
    EXPECT_TRUE(tx_r.ok()) << ErrorName(tx_r.error());
    if (!tx_r.ok()) return st;
    TransmitSession tx = std::move(tx_r).value();

    auto rx_r = ReceiveSession::Create(layout);
    EXPECT_TRUE(rx_r.ok());
    ReceiveSession rx = std::move(rx_r).value();

    sim::Channel channel(cfg);
    CellMatrix frame(geom.cols, geom.rows);
    while (!rx.complete() && st.presented < max_frames) {
        EXPECT_TRUE(tx.Next(&frame).ok());
        ++st.presented;
        if (cfg.frame_drop_rate > 0.0 && channel.rng().NextDouble() < cfg.frame_drop_rate) {
            continue;  // presented but never captured -- a Pc term
        }
        const std::vector<double> samples = channel.Apply(frame);
        rx.Ingest(samples, static_cast<std::int64_t>(st.presented) * 16'666'667);
    }

    st.header_ok = rx.stats().header_ok;
    st.header_fail = rx.stats().header_fail;
    st.erasures = rx.stats().erased_cells;
    if (rx.receiver() != nullptr) st.fountain_overhead = rx.receiver()->overhead();
    auto out = rx.Finish();
    st.verified = out.ok();
    if (!out.ok()) {
        st.failure = out.error();
    } else {
        EXPECT_EQ(out.value(), payload);
    }
    return st;
}

struct Rig {
    GridGeometry geom{120, 200};
    FrameLayout layout;
    M0Modulator mod;
    HeaderCodec codec;

    Rig()
        : layout(FrameLayout::Create(GridGeometry{120, 200}, LayoutConfig{}).value()),
          mod(layout) {}
};

}  // namespace


TEST(EndToEnd, IdentityChannelDecodesPerfectly) {
    sim::ChannelConfig cfg;
    cfg.read_noise_sigma = 0.0;  // no impairment whatsoever

    const auto payload = Payload(16000);
    const RunStats st = RunTransfer(payload, cfg);

    ASSERT_TRUE(st.verified) << "identity channel failed: " << ErrorName(st.failure);
    EXPECT_EQ(st.header_fail, 0u);
    EXPECT_EQ(st.erasures, 0u);
    // Systematic prefix means a clean channel needs essentially no repair symbols.
    EXPECT_LT(st.fountain_overhead, 0.05);
}

TEST(EndToEnd, ModerateNoiseStillVerifies) {
    sim::ChannelConfig cfg;
    cfg.read_noise_sigma = 20.0;
    cfg.shot_noise_scale = 0.5;

    const auto payload = Payload(16000);
    const RunStats st = RunTransfer(payload, cfg);
    EXPECT_TRUE(st.verified) << "failed under moderate noise: " << ErrorName(st.failure);
}

TEST(EndToEnd, PhotometricDistortionIsAbsorbedByPilots) {
    // Gamma, vignetting and exposure offset are exactly what the distributed pilot lattice
    // (layout Candidate B) is supposed to normalise away.
    sim::ChannelConfig cfg;
    cfg.gamma = 2.2;
    cfg.vignetting = 0.4;
    cfg.exposure_gain = 0.7;
    cfg.black_level = 12.0;
    cfg.read_noise_sigma = 4.0;

    const auto payload = Payload(16000);
    const RunStats st = RunTransfer(payload, cfg);
    EXPECT_TRUE(st.verified) << "photometric distortion defeated the pilots: "
                             << ErrorName(st.failure);
}

TEST(EndToEnd, FrameLossIsAbsorbedByTheFountainLayer) {
    // The whole point of ADR-0009: whole-frame loss should be a non-event.
    sim::ChannelConfig cfg;
    cfg.read_noise_sigma = 4.0;
    cfg.frame_drop_rate = 0.35;

    const auto payload = Payload(16000);
    const RunStats st = RunTransfer(payload, cfg);
    EXPECT_TRUE(st.verified) << "frame loss was not absorbed: " << ErrorName(st.failure);
}

TEST(EndToEnd, OcclusionProducesErasuresAndStillVerifies) {
    sim::ChannelConfig cfg;
    cfg.read_noise_sigma = 4.0;
    cfg.occlusion_fraction = 0.05;

    const auto payload = Payload(8000);
    const RunStats st = RunTransfer(payload, cfg);
    EXPECT_GT(st.erasures, 0u) << "occlusion should have produced erasures";
    // Note: without a payload FEC layer (EXP-011 selects it), an occluded cell corrupts its
    // symbol outright. The fountain layer still recovers, at a cost in overhead.
    EXPECT_TRUE(st.verified) << "occlusion defeated the transfer: " << ErrorName(st.failure);
}

TEST(EndToEnd, SimulatedSourceProvidesGroundTruth) {
    // Ground truth is the thing a real camera can never give us. It is what lets us measure
    // COMPONENT accuracy rather than only end-to-end success.
    Rig rig;
    std::vector<CellMatrix> frames;
    for (int i = 0; i < 3; ++i) {
        CellMatrix m(rig.geom.cols, rig.geom.rows);
        const std::vector<std::uint8_t> hdr(rig.codec.coded_size(), static_cast<std::uint8_t>(i));
        ASSERT_TRUE(rig.mod.Render(hdr, std::vector<std::uint8_t>(64, 0x11), &m).ok());
        frames.push_back(std::move(m));
    }

    sim::ChannelConfig cfg;
    cfg.read_noise_sigma = 1.0;
    sim::SimulatedSource src(rig.geom, frames, cfg);

    auto f = src.Next();
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(f->ground_truth.has_value());
    EXPECT_EQ(f->ground_truth->cols(), rig.geom.cols);
    EXPECT_EQ(f->cell_samples.size(), rig.geom.cells());
    EXPECT_EQ(f->phase, FramePhase::kClean);
}

TEST(EndToEnd, SimulatedSourceExhausts) {
    Rig rig;
    std::vector<CellMatrix> frames;
    CellMatrix m(rig.geom.cols, rig.geom.rows);
    const std::vector<std::uint8_t> hdr(rig.codec.coded_size(), 0);
    ASSERT_TRUE(rig.mod.Render(hdr, {}, &m).ok());
    frames.push_back(std::move(m));

    sim::ChannelConfig cfg;
    sim::SimulatedSource src(rig.geom, frames, cfg);
    EXPECT_TRUE(src.Next().has_value());
    EXPECT_FALSE(src.Next().has_value());
    EXPECT_EQ(src.frames_emitted(), 1u);
}
