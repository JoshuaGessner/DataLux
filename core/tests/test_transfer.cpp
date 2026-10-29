#include <fileflow/transfer.h>

#include <gtest/gtest.h>

#include <algorithm>

#include <vector>

using namespace fileflow;

namespace {
std::vector<std::uint8_t> Payload(std::size_t n, std::uint64_t seed = 99) {
    std::vector<std::uint8_t> v(n);
    SplitMix64 rng(seed);
    for (auto& b : v) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
    return v;
}

// The next frame that carries a fountain SYMBOL. Tests that tamper with a symbol must not land
// on a manifest frame instead: the receiver refuses those outright, so the tamper would pass for
// the wrong reason and the test would prove nothing.
FileTransmitter::FramePayload NextDataFrame(FileTransmitter& tx) {
    for (;;) {
        auto fp = tx.NextFrame();
        if (!fp.header.is_manifest()) return fp;
    }
}
}  // namespace

// ---------------------------------------------------------------- filename safety

TEST(SanitiseFileName, StripsPathTraversal) {
    // THREAT-MODEL T3. The received name is a DISPLAY HINT, never a path.
    EXPECT_EQ(SanitiseFileName("../../etc/passwd"), "passwd");
    EXPECT_EQ(SanitiseFileName("/etc/shadow"), "shadow");
    EXPECT_EQ(SanitiseFileName("..\\..\\windows\\system32\\cfg"), "cfg");
    EXPECT_EQ(SanitiseFileName("dir/sub/file.txt"), "file.txt");
}

TEST(SanitiseFileName, RejectsDotOnlyNames) {
    EXPECT_EQ(SanitiseFileName(".."), "received.bin");
    EXPECT_EQ(SanitiseFileName("."), "received.bin");
    EXPECT_EQ(SanitiseFileName("..."), "received.bin");
    EXPECT_EQ(SanitiseFileName(""), "received.bin");
}

TEST(SanitiseFileName, StripsControlCharactersAndNulls) {
    const std::string nasty = std::string("ev\x01il\x7f.txt\0hidden", 17);
    const std::string out = SanitiseFileName(nasty);
    EXPECT_EQ(out.find('\x01'), std::string::npos);
    EXPECT_EQ(out.find('\x7f'), std::string::npos);
    EXPECT_EQ(out.find('\0'), std::string::npos);
}

TEST(SanitiseFileName, BoundsLength) {
    const std::string huge(5000, 'a');
    EXPECT_LE(SanitiseFileName(huge).size(), FileManifest::kMaxNameLength);
}

// ---------------------------------------------------------------- manifest

TEST(FileManifest, RoundTrips) {
    FileManifest m;
    m.file_size = 100000;
    m.sha256 = Sha256::Of(Payload(100));
    m.file_name = "test.bin";
    m.block_count = 4;
    m.block_symbols = 64;
    m.symbol_size = 512;
    m.session_id = 0xABCD1234;
    ASSERT_TRUE(m.Validate().ok());

    auto d = FileManifest::Decode(m.Encode());
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().file_size, m.file_size);
    EXPECT_EQ(d.value().sha256, m.sha256);
    EXPECT_EQ(d.value().file_name, m.file_name);
    EXPECT_EQ(d.value().block_count, m.block_count);
    EXPECT_EQ(d.value().session_id, m.session_id);
    EXPECT_EQ(d.value().content.original_size, 0U) << "no hint was given";
}

TEST(FileManifest, CarriesTheContentHint) {
    FileManifest m;
    m.file_size = 40000;
    m.sha256 = Sha256::Of(Payload(100));
    m.file_name = "notes.txt";
    m.block_count = 1;
    m.block_symbols = 80;
    m.symbol_size = 512;
    m.session_id = 7;
    m.content = ContentHint{.original_size = 150000, .codec = 2, .compress_ms = 85};
    auto d = FileManifest::Decode(m.Encode());
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().content.original_size, 150000U);
    EXPECT_EQ(d.value().content.codec, 2U);
    EXPECT_EQ(d.value().content.compress_ms, 85U);
}

TEST(FileManifest, AnUnknownExtensionIsSkippedNotRejected) {
    // What a newer sender's manifest looks like to this receiver: an extension it has never heard
    // of, before the one it knows. It must read the rest, not refuse the transfer.
    FileManifest m;
    m.file_size = 1000;
    m.sha256 = Sha256::Of(Payload(10));
    m.file_name = "a";
    m.block_count = 1;
    m.block_symbols = 2;
    m.symbol_size = 512;
    m.session_id = 9;
    std::vector<std::uint8_t> bytes = m.Encode();
    // Replace the empty extension block (U16 0, then CRC) with: type 99 len 3 [1 2 3], then a hint.
    bytes.resize(bytes.size() - 4 - 2);
    const std::vector<std::uint8_t> ext = {0, static_cast<std::uint8_t>(5 + 2 + ContentHint::kEncodedSize),
                                           99, 3, 1, 2, 3,
                                           ContentHint::kExtensionType, ContentHint::kEncodedSize,
                                           0, 0, 0, 0, 0, 0, 0x10, 0x00,  // 4096
                                           2, 0, 0, 0, 5};
    bytes.insert(bytes.end(), ext.begin(), ext.end());
    const std::uint32_t crc = Crc32(bytes);
    for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
    auto d = FileManifest::Decode(bytes);
    ASSERT_TRUE(d.ok()) << ErrorName(d.error());
    EXPECT_EQ(d.value().content.original_size, 4096U);
    EXPECT_EQ(d.value().content.compress_ms, 5U);
}

TEST(FileManifest, BatchAdvisoryCanTravelWithoutEncryptedContentDetails) {
    FileManifest m;
    m.file_size = 1000;
    m.file_name = "Encrypted file";
    m.block_count = 1;
    m.block_symbols = 2;
    m.symbol_size = 512;
    m.content.batch = true;
    auto decoded = FileManifest::Decode(m.Encode());
    ASSERT_TRUE(decoded.ok());
    EXPECT_TRUE(decoded.value().content.batch);
    EXPECT_EQ(decoded.value().content.original_size, 0U);
    EXPECT_EQ(decoded.value().content.codec, 0U);

    // Treating the advisory as an unknown extension, as an older receiver does, still reads
    // the complete manifest. The hint never changes the payload or its hash.
    auto bytes = m.Encode();
    bytes[bytes.size() - 7] = 99;
    bytes.resize(bytes.size() - 4);
    const auto crc = Crc32(bytes);
    for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
    decoded = FileManifest::Decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_FALSE(decoded.value().content.batch);
    EXPECT_EQ(decoded.value().file_size, m.file_size);
    EXPECT_EQ(decoded.value().sha256, m.sha256);
}

TEST(FileManifest, BatchAndCompressionHintsDoNotOverwriteEachOther) {
    FileManifest m;
    m.file_size = 1000;
    m.file_name = "2 files";
    m.block_count = 1;
    m.block_symbols = 2;
    m.symbol_size = 512;
    m.content = {.original_size = 2000, .codec = 2, .compress_ms = 10, .batch = true};
    auto bytes = m.Encode();
    // Move the three-byte batch TLV ahead of the compression TLV to cover either ordering.
    const auto first = bytes.end() - 4 - 3 - 2 - ContentHint::kEncodedSize;
    std::rotate(first, bytes.end() - 4 - 3, bytes.end() - 4);
    bytes.resize(bytes.size() - 4);
    const auto crc = Crc32(bytes);
    for (int i = 3; i >= 0; --i) bytes.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
    auto decoded = FileManifest::Decode(bytes);
    ASSERT_TRUE(decoded.ok());
    EXPECT_TRUE(decoded.value().content.batch);
    EXPECT_EQ(decoded.value().content.original_size, 2000U);
    EXPECT_EQ(decoded.value().content.codec, 2U);
    EXPECT_EQ(decoded.value().content.compress_ms, 10U);
}

TEST(FileManifest, RejectsHostileValues) {
    FileManifest m;
    m.file_size = 1000;
    m.block_count = 1;
    m.block_symbols = 64;
    m.symbol_size = 512;

    // Oversized file
    auto a = m; a.file_size = FileManifest::kMaxFileSize + 1;
    EXPECT_FALSE(a.Validate().ok());

    // Zero everything
    auto b = m; b.block_count = 0;
    EXPECT_EQ(b.Validate().error(), Error::kValueOutOfRange);
    auto c = m; c.symbol_size = 0;
    EXPECT_EQ(c.Validate().error(), Error::kValueOutOfRange);

    // Capacity smaller than the declared file size -- would truncate silently.
    auto d = m; d.file_size = 10'000'000;
    EXPECT_EQ(d.Validate().error(), Error::kLengthMismatch);

    // Overflow in block_symbols * symbol_size
    auto e = m;
    e.block_symbols = FountainParams::kMaxSourceSymbols;
    e.symbol_size = FountainParams::kMaxSymbolSize;
    e.block_count = FileManifest::kMaxBlockCount;
    EXPECT_FALSE(e.Validate().ok());
}

TEST(FileManifest, TruncationAndCorruptionAreRejected) {
    FileManifest m;
    m.file_size = 100000;
    m.file_name = "x.bin";
    m.block_count = 4;
    m.block_symbols = 64;
    m.symbol_size = 512;
    const auto bytes = m.Encode();

    for (std::size_t n = 0; n < bytes.size(); ++n) {
        EXPECT_FALSE(FileManifest::Decode(std::span(bytes.data(), n)).ok()) << "len " << n;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        auto corrupt = bytes;
        corrupt[i] ^= 0x40;
        EXPECT_FALSE(FileManifest::Decode(corrupt).ok()) << "byte " << i;
    }
}

TEST(FileManifest, RejectsEntirelyPaddingBlocksBeforeAllocation) {
    FileManifest m;
    m.file_size = 1;
    m.block_count = 64;
    m.block_symbols = FountainParams::kMaxSourceSymbols;
    m.symbol_size = FountainParams::kMaxSymbolSize;
    // Previously accepted: a one-byte file requested 4 GiB of decoder data.
    EXPECT_EQ(m.Validate().error(), Error::kLengthMismatch);
    EXPECT_EQ(FileManifest::Decode(m.Encode()).error(), Error::kLengthMismatch);
    EXPECT_EQ(FileReceiver::Create(m).error(), Error::kLengthMismatch);

    m.block_symbols = 2;
    m.symbol_size = 512;
    m.block_count = 2;
    m.file_size = 1024;
    EXPECT_EQ(m.Validate().error(), Error::kLengthMismatch);
    m.file_size = 1025;
    EXPECT_TRUE(m.Validate().ok());
}

TEST(Transfer, ReceiverBoundsDecoderWorkingSetBeforeAllocating) {
    FileManifest m;
    m.block_symbols = FountainParams::kMaxSourceSymbols;
    m.symbol_size = FountainParams::kMaxSymbolSize;
    m.block_count = 7;
    m.file_size = std::uint64_t{m.block_count} * m.block_symbols * m.symbol_size;
    // Validate the acceptance side without actually allocating hundreds of MiB.
    EXPECT_TRUE(FileReceiver::ValidateWorkingSet(m).ok());
    ++m.block_count;
    m.file_size = std::uint64_t{m.block_count} * m.block_symbols * m.symbol_size;
    EXPECT_TRUE(m.Validate().ok());
    EXPECT_EQ(FileReceiver::ValidateWorkingSet(m).error(), Error::kPayloadTooLarge);
    EXPECT_EQ(FileReceiver::Create(m).error(), Error::kPayloadTooLarge);

    // A modest payload can require an enormous matrix when symbols are one byte.
    m.symbol_size = 1;
    m.block_count = 4096;
    m.file_size = std::uint64_t{m.block_count} * m.block_symbols;
    EXPECT_TRUE(m.Validate().ok());
    EXPECT_LT(m.file_size, FileReceiver::kMaxWorkingSetBytes);
    EXPECT_EQ(FileReceiver::Create(m).error(), Error::kPayloadTooLarge);
}

TEST(FileManifest, ArbitraryGarbageNeverCrashes) {
    SplitMix64 rng(1234);
    for (int trial = 0; trial < 20000; ++trial) {
        std::vector<std::uint8_t> buf(1 + (rng.Next() % 200));
        for (auto& b : buf) b = static_cast<std::uint8_t>(rng.Next() & 0xFF);
        auto d = FileManifest::Decode(buf);
        (void)d;
    }
}

// ---------------------------------------------------------------- transfer

TEST(Transfer, CleanChannelRoundTrip) {
    const auto payload = Payload(50000);
    auto tx = FileTransmitter::Create(payload, "data.bin", 0x1234, 256, 32);
    ASSERT_TRUE(tx.ok()) << ErrorName(tx.error());

    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    while (!rx.value().complete()) {
        auto fp = tx.value().NextFrame();
        rx.value().Ingest(fp.header, fp.data);
    }

    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload);
}

TEST(Transfer, SurvivesHeavyFrameLoss) {
    const auto payload = Payload(20000);
    auto tx = FileTransmitter::Create(payload, "lossy.bin", 0x99, 256, 32);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    // Drop 40% of frames. Fountain coding is supposed to make this a non-event.
    SplitMix64 rng(7);
    std::uint64_t guard = 0;
    while (!rx.value().complete() && guard++ < 500000) {
        auto fp = tx.value().NextFrame();
        if (rng.NextDouble() < 0.4) continue;
        rx.value().Ingest(fp.header, fp.data);
    }

    ASSERT_TRUE(rx.value().complete()) << "did not converge under 40% loss";
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload);
}

TEST(Transfer, JoinsMidTransmissionAndStillCompletes) {
    // The operator's scenario: the camera is not aimed until the transmission is already 54% of
    // the way through. Everything before that is gone, and it is gone CONTIGUOUSLY -- which is a
    // different pattern from the random loss above, and the one a person actually produces by
    // picking up a phone late.
    //
    // Fountain coding is what makes this a non-event: symbols are generated, not enumerated, so
    // there is no such thing as "the missing ones". Any sufficiently large set of DISTINCT symbols
    // reconstructs a block, and the transmitter cycles blocks so a late joiner makes progress on
    // every block rather than waiting for one to come round. Nothing is renegotiated and the
    // receiver never asks for a retransmission -- it could not, there is no back channel (NG7).
    const auto payload = Payload(20000);
    auto tx = FileTransmitter::Create(payload, "late.bin", 0x5A, 256, 32);
    ASSERT_TRUE(tx.ok());

    // Run the transmitter with NO receiver attached, discarding 54% of the frames a clean
    // transfer would have needed.
    auto probe = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(probe.ok());
    std::uint64_t clean_frames = 0;
    {
        auto t2 = FileTransmitter::Create(payload, "late.bin", 0x5A, 256, 32);
        ASSERT_TRUE(t2.ok());
        while (!probe.value().complete() && clean_frames < 500000) {
            auto fp = t2.value().NextFrame();
            probe.value().Ingest(fp.header, fp.data);
            ++clean_frames;
        }
    }
    ASSERT_TRUE(probe.value().complete());
    const std::uint64_t skipped = (clean_frames * 54) / 100;
    ASSERT_GT(skipped, 0u);
    for (std::uint64_t i = 0; i < skipped; ++i) (void)tx.value().NextFrame();

    // Only NOW does the receiver start looking.
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());
    EXPECT_DOUBLE_EQ(rx.value().progress(), 0.0) << "nothing seen yet, so nothing claimed";

    double last = -1.0;
    std::uint64_t guard = 0;
    while (!rx.value().complete() && guard++ < 500000) {
        auto fp = tx.value().NextFrame();
        rx.value().Ingest(fp.header, fp.data);
        const double p = rx.value().progress();
        ASSERT_GE(p, last) << "progress went backwards, which a user would read as a fault";
        last = p;
        // Never claim completion before the hash gate would agree.
        if (p < 1.0) {
            ASSERT_FALSE(rx.value().complete());
        }
    }

    ASSERT_TRUE(rx.value().complete()) << "joining late must not make a transfer unrecoverable";
    EXPECT_DOUBLE_EQ(rx.value().progress(), 1.0);
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload) << "late join delivered the wrong bytes";
}

TEST(Transfer, RejectsForeignSessionPackets) {
    // THREAT-MODEL T8: two transmitters in view must not corrupt each other's session.
    const auto payload = Payload(5000);
    auto tx = FileTransmitter::Create(payload, "a.bin", 0x1111, 256, 16);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    auto fp = NextDataFrame(tx.value());
    auto foreign = fp;
    foreign.header.session_id = 0x2222;
    for (auto& b : foreign.data) b ^= 0xFF;

    rx.value().Ingest(foreign.header, foreign.data);
    EXPECT_EQ(rx.value().symbols_ingested(), 0u) << "foreign session packet was accepted";
}

TEST(Transfer, RejectsOutOfRangeBlockId) {
    const auto payload = Payload(5000);
    auto tx = FileTransmitter::Create(payload, "a.bin", 0x1111, 256, 16);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    auto fp = NextDataFrame(tx.value());
    fp.header.block_id = 999999;
    rx.value().Ingest(fp.header, fp.data);
    EXPECT_EQ(rx.value().symbols_ingested(), 0u);
}

TEST(Transfer, AccidentallyCorruptedFrameIsErasedAndRecovered) {
    // A channel-corrupted symbol must never reach the fountain decoder. The per-frame
    // payload CRC turns it into a frame ERASURE, which the fountain layer absorbs -- so the
    // transfer still completes CORRECTLY rather than failing at the end.
    const auto payload = Payload(10000);
    auto tx = FileTransmitter::Create(payload, "c.bin", 0x55, 256, 32);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    int flipped = 0;
    std::uint64_t guard = 0;
    while (!rx.value().complete() && guard++ < 500000) {
        auto fp = tx.value().NextFrame();
        if (flipped < 20) {
            fp.data[0] ^= 0x01;  // channel damage: CRC in the header still covers the original
            ++flipped;
        }
        rx.value().Ingest(fp.header, fp.data);
    }

    ASSERT_TRUE(rx.value().complete());
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << "erasure was not absorbed: " << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload);
}

TEST(Transfer, HashGateCatchesCorruptionThatEvadesEveryChecksum) {
    // THE GUARANTEE (G3), against a MALICIOUS transmitter (THREAT-MODEL T1/T5) rather than
    // a noisy channel. An attacker controls every transmitted bit, including the payload
    // CRC, so they can make corrupted data pass every intermediate check. SHA-256 over the
    // reassembled file is the last line of defence, and it must hold.
    const auto payload = Payload(10000);
    auto tx = FileTransmitter::Create(payload, "evil.bin", 0x55, 256, 32);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    bool tampered = false;
    std::uint64_t guard = 0;
    while (!rx.value().complete() && guard++ < 500000) {
        auto fp = NextDataFrame(tx.value());
        if (!tampered && fp.header.esi < tx.value().manifest().block_symbols) {
            fp.data[0] ^= 0x01;
            fp.header.payload_crc = Crc32(fp.data);  // forge a matching CRC
            tampered = true;
        }
        rx.value().Ingest(fp.header, fp.data);
    }

    ASSERT_TRUE(tampered);
    auto out = rx.value().Finish();
    ASSERT_FALSE(out.ok()) << "tampered transfer was delivered as valid";
    EXPECT_EQ(out.error(), Error::kHashMismatch);
}

TEST(Transfer, IncompleteTransferDeliversNothing) {
    const auto payload = Payload(10000);
    auto tx = FileTransmitter::Create(payload, "p.bin", 0x66, 256, 32);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    for (int i = 0; i < 5; ++i) {
        auto fp = tx.value().NextFrame();
        rx.value().Ingest(fp.header, fp.data);
    }
    EXPECT_FALSE(rx.value().complete());
    EXPECT_EQ(rx.value().Finish().error(), Error::kFountainIncomplete);
}

TEST(Transfer, ProgressAdvancesMonotonically) {
    const auto payload = Payload(20000);
    auto tx = FileTransmitter::Create(payload, "p.bin", 0x77, 256, 32);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());

    double last = 0.0;
    while (!rx.value().complete()) {
        auto fp = tx.value().NextFrame();
        rx.value().Ingest(fp.header, fp.data);
        const double p = rx.value().progress();
        EXPECT_GE(p, last);
        last = p;
    }
    EXPECT_NEAR(rx.value().progress(), 1.0, 1e-9);
}

TEST(Transfer, EmptyPayloadIsRejected) {
    EXPECT_EQ(FileTransmitter::Create({}, "e.bin", 1).error(), Error::kValueOutOfRange);
}

TEST(Transfer, ContentHintMustFitWithoutChangingAnExistingManifestOnFailure) {
    const auto payload = Payload(1000);
    auto tx = FileTransmitter::Create(payload, "x", 1, 79);
    ASSERT_TRUE(tx.ok());
    const auto before = tx.value().manifest().Encode();
    const ContentHint hint{.original_size = 2000, .codec = 2, .compress_ms = 1};
    EXPECT_EQ(tx.value().SetContentHint(hint).error(), Error::kValueOutOfRange);
    EXPECT_EQ(tx.value().manifest().Encode(), before);
    const auto frame = tx.value().NextFrame();
    EXPECT_EQ(frame.data.size(), 79u);
    EXPECT_LE(frame.header.payload_bytes, frame.data.size());

    auto roomy = FileTransmitter::Create(payload, "x", 1, 128);
    ASSERT_TRUE(roomy.ok());
    ASSERT_TRUE(roomy.value().SetContentHint(hint).ok());
    EXPECT_EQ(roomy.value().manifest().content.original_size, hint.original_size);
}

// ---------------------------------------------------------------- protocol v2 (ADR-0015)

TEST(Transfer, ManifestFramesRecurAndDecodeToTheSessionManifest) {
    const auto payload = Payload(30000);
    auto tx = FileTransmitter::Create(payload, "m.bin", 0xAB, 512, 0, 10);
    ASSERT_TRUE(tx.ok());
    for (std::uint32_t i = 0; i < 35; ++i) {
        auto fp = tx.value().NextFrame();
        ASSERT_EQ(fp.header.sequence, i);
        ASSERT_EQ(fp.data.size(), 512u) << "every frame must fill the optical payload";
        const bool want_manifest = (i % 10) == 0;
        ASSERT_EQ(fp.header.is_manifest(), want_manifest) << "seq " << i;
        if (!want_manifest) continue;

        // The manifest is recoverable from the frame alone -- this is what a receiver with no
        // out-of-band knowledge has to work with.
        const std::span<const std::uint8_t> m(fp.data.data(), fp.header.payload_bytes);
        EXPECT_EQ(Crc32(m), fp.header.payload_crc);
        auto d = FileManifest::Decode(m);
        ASSERT_TRUE(d.ok()) << ErrorName(d.error());
        EXPECT_EQ(d.value().sha256, tx.value().manifest().sha256);
        EXPECT_EQ(d.value().file_size, payload.size());

        // And the filler is not a black frame (F15, F39).
        std::size_t zeros = 0;
        for (std::size_t b = fp.header.payload_bytes; b < fp.data.size(); ++b) zeros += fp.data[b] == 0;
        EXPECT_LT(zeros, (fp.data.size() - fp.header.payload_bytes) / 20);
    }
}

TEST(Transfer, ReceiverRefusesManifestFramesAsSymbols) {
    const auto payload = Payload(5000);
    auto tx = FileTransmitter::Create(payload, "a.bin", 0x1111, 256, 0, 4);
    ASSERT_TRUE(tx.ok());
    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());
    auto fp = tx.value().NextFrame();
    ASSERT_TRUE(fp.header.is_manifest());
    // Its CRC is valid and its block/ESI are in range: only the flag stops it poisoning block 0.
    rx.value().Ingest(fp.header, fp.data);
    EXPECT_EQ(rx.value().symbols_ingested(), 0u);
}

TEST(Transfer, PaddingIsNeverTransmittedAndNeverCounted) {
    // 10 symbols of data in a 16-symbol block: 6 symbols of pure padding that v1 sent every cycle.
    const auto payload = Payload(10 * 128 - 7);
    auto tx = FileTransmitter::Create(payload, "pad.bin", 0x31, 128, 16, 1000);
    ASSERT_TRUE(tx.ok());
    EXPECT_EQ(FirstPaddingSymbol(tx.value().manifest(), 0), 10u);

    auto rx = FileReceiver::Create(tx.value().manifest());
    ASSERT_TRUE(rx.ok());
    EXPECT_EQ(rx.value().symbols_needed(), 10u);
    EXPECT_DOUBLE_EQ(rx.value().progress(), 0.0) << "known padding is not channel progress";

    std::uint64_t data_frames = 0;
    while (!rx.value().complete() && data_frames < 1000) {
        auto fp = tx.value().NextFrame();
        if (fp.header.is_manifest()) continue;
        ++data_frames;
        EXPECT_FALSE(fp.header.esi >= 10 && fp.header.esi < 16) << "sent padding esi " << fp.header.esi;
        rx.value().Ingest(fp.header, fp.data);
    }
    // Clean channel: exactly the data symbols, no more.
    EXPECT_EQ(data_frames, 10u);
    EXPECT_DOUBLE_EQ(rx.value().overhead(), 0.0);
    auto out = rx.value().Finish();
    ASSERT_TRUE(out.ok()) << ErrorName(out.error());
    EXPECT_EQ(out.value(), payload);
}

TEST(Transfer, AutoBlocksAreFewBalancedAndBounded) {
    EXPECT_EQ(AutoBlockSymbols(1), 1u);
    EXPECT_EQ(AutoBlockSymbols(128), 128u) << "one block when it fits";
    EXPECT_EQ(AutoBlockSymbols(FountainParams::kMaxSourceSymbols), FountainParams::kMaxSourceSymbols);
    // Just over the bound: two balanced blocks, not one full block and a stub.
    EXPECT_EQ(AutoBlockSymbols(FountainParams::kMaxSourceSymbols + 2),
              FountainParams::kMaxSourceSymbols / 2 + 1);
    for (std::uint64_t t : {5ull, 1000ull, 1025ull, 5000ull, 123457ull}) {
        const std::uint32_t k = AutoBlockSymbols(t);
        ASSERT_LE(k, FountainParams::kMaxSourceSymbols);
        const std::uint64_t blocks = (t + k - 1) / k;
        EXPECT_LT(blocks * k - t, blocks) << "padding must be under one symbol per block, t=" << t;
    }
}
