#include <gtest/gtest.h>
#include "file_io.hpp"
#include "replay_io.hpp"
#include "config.hpp"
#include "grid.hpp"
#include "wincheck.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- helpers ----------------------------------------------------------------

static game::ReplayLog make_minimal_log() {
  game::ReplayLog log;
  log.abi_version = game::cfg::ABI_VERSION;
  log.seed        = 0xC0FFEE;
  log.map_w       = 3;
  log.map_h       = 3;
  for (int i = 0; i < 9; ++i)
    log.map_tiles.push_back(game::Tile{});
  log.faction_a = 0;
  log.faction_b = 1;
  log.name_a    = "p0";
  log.name_b    = "p1";
  log.tick_cap  = 100;
  log.expected_hash        = 0xDEADBEEFCAFEBABEull;
  log.outcome.winner.value = 0;
  log.outcome.reason       = game::WinReason::Draw;
  return log;
}

// ---- deduce_file_io_type ----------------------------------------------------

TEST(FileIo, DeduceUdIsRaw) {
  EXPECT_EQ(game::deduce_file_io_type("match.ud"), game::FileIoType::Raw);
}

TEST(FileIo, DeduceZstIsZstd) {
  EXPECT_EQ(game::deduce_file_io_type("match.zst"), game::FileIoType::Zstd);
}

TEST(FileIo, DeduceZstdIsZstd) {
  EXPECT_EQ(game::deduce_file_io_type("match.zstd"), game::FileIoType::Zstd);
}

TEST(FileIo, DeduceNoExtensionThrows) {
  EXPECT_THROW(game::deduce_file_io_type("matchfile"), std::runtime_error);
}

TEST(FileIo, DeduceUnknownExtensionThrows) {
  EXPECT_THROW(game::deduce_file_io_type("match.mp4"), std::runtime_error);
}

// ---- Raw file round-trip ----------------------------------------------------

TEST(FileIo, RawRoundTrip) {
  const std::string path = "/tmp/ud_test_raw.ud";
  std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 100, 200, 255};

  {
    auto w = game::make_file_writer(path, game::FileIoType::Raw);
    w->write(payload.data(), payload.size());
    w->close();
  }

  std::vector<uint8_t> got(payload.size());
  {
    auto r = game::make_file_reader(path, game::FileIoType::Raw);
    EXPECT_TRUE(r->read(got.data(), got.size()));
    EXPECT_FALSE(r->eof());
    // consume to end
    uint8_t dummy;
    r->read(&dummy, 1);
    EXPECT_TRUE(r->eof());
    r->close();
  }

  EXPECT_EQ(got, payload);
  std::remove(path.c_str());
}

TEST(FileIo, RawWriteFailureThrows) {
  EXPECT_THROW(game::make_file_writer("/nonexistent_dir/a.ud", game::FileIoType::Raw),
               std::runtime_error);
}

TEST(FileIo, RawReadFailureThrows) {
  EXPECT_THROW(game::make_file_reader("/nonexistent_dir/a.ud", game::FileIoType::Raw),
               std::runtime_error);
}

// ---- Zstd file round-trip ---------------------------------------------------

TEST(FileIo, ZstdRoundTripSmall) {
  const std::string path = "/tmp/ud_test_small.zst";
  std::vector<uint8_t> payload = {7, 8, 9, 42, 77};

  {
    auto w = game::make_file_writer(path, game::FileIoType::Zstd);
    w->write(payload.data(), payload.size());
    w->close();
  }

  std::vector<uint8_t> got(payload.size());
  {
    auto r = game::make_file_reader(path, game::FileIoType::Zstd);
    EXPECT_TRUE(r->read(got.data(), got.size()));
    r->close();
  }

  EXPECT_EQ(got, payload);
  std::remove(path.c_str());
}

TEST(FileIo, ZstdRoundTripLarge) {
  // >128 KB of incompressible data to exercise the streaming decompressor path
  // (refill input buffer loop and drain decoded-output-buffer loop).
  const std::string path = "/tmp/ud_test_large.zst";
  constexpr size_t N = 300 * 1024;
  std::vector<uint8_t> payload(N);
  for (size_t i = 0; i < N; ++i)
    payload[i] = static_cast<uint8_t>((i * 6364136223846793005ull) >> 56);

  {
    auto w = game::make_file_writer(path, game::FileIoType::Zstd);
    w->write(payload.data(), payload.size());
    w->close();
  }

  std::vector<uint8_t> got(N);
  {
    auto r = game::make_file_reader(path, game::FileIoType::Zstd);
    EXPECT_TRUE(r->read(got.data(), got.size()));
    EXPECT_FALSE(r->eof());
    r->close();
  }

  EXPECT_EQ(got, payload);
  std::remove(path.c_str());
}

TEST(FileIo, ZstdWriteFailureThrows) {
  EXPECT_THROW(game::make_file_writer("/nonexistent_dir/a.zst", game::FileIoType::Zstd),
               std::runtime_error);
}

TEST(FileIo, ZstdReadFailureThrows) {
  EXPECT_THROW(game::make_file_reader("/nonexistent_dir/a.zst", game::FileIoType::Zstd),
               std::runtime_error);
}

TEST(FileIo, ZstdEofAfterPayload) {
  const std::string path = "/tmp/ud_test_eof.zst";
  std::vector<uint8_t> payload = {1, 2, 3};

  {
    auto w = game::make_file_writer(path, game::FileIoType::Zstd);
    w->write(payload.data(), payload.size());
    w->close();
  }

  {
    auto r = game::make_file_reader(path, game::FileIoType::Zstd);
    std::vector<uint8_t> buf(payload.size());
    EXPECT_TRUE(r->read(buf.data(), buf.size()));
    // Reading past end should report eof
    uint8_t dummy;
    EXPECT_FALSE(r->read(&dummy, 1));
    EXPECT_TRUE(r->eof());
    r->close();
  }

  std::remove(path.c_str());
}

TEST(FileIo, ZstdSkippableFrameBoundaryDoesNotTriggerPrematureEof) {
  // A skippable frame matching ZSTD_DStreamInSize() (128 KiB) causes the first
  // decompress call to consume in_buf_ with 0 output bytes; read() must refill
  // instead of reporting premature EOF.
  const std::string payload_path = "/tmp/ud_test_skip_payload.zst";
  const std::string full_path    = "/tmp/ud_test_skip_full.zst";
  const std::vector<uint8_t> payload = {11, 22, 33, 44, 55};

  {
    auto w = game::make_file_writer(payload_path, game::FileIoType::Zstd);
    w->write(payload.data(), payload.size());
    w->close();
  }

  // ZSTD skippable frame: magic 0x184D2A50 (LE) + 4-byte LE size + N zero bytes.
  {
    auto r_in = game::make_file_reader(payload_path, game::FileIoType::Raw);
    std::vector<uint8_t> compressed;
    uint8_t b;
    while (r_in->read(&b, 1))
      compressed.push_back(b);
    r_in->close();

    constexpr uint32_t kSkipPayload = 128 * 1024 - 8;
    std::vector<uint8_t> skip_frame(8 + kSkipPayload, 0);
    const uint32_t magic = 0x184D2A50u;
    const uint32_t sz    = kSkipPayload;
    std::memcpy(skip_frame.data(), &magic, 4);
    std::memcpy(skip_frame.data() + 4, &sz, 4);

    auto w = game::make_file_writer(full_path, game::FileIoType::Raw);
    w->write(skip_frame.data(), skip_frame.size());
    w->write(compressed.data(), compressed.size());
    w->close();
  }

  std::vector<uint8_t> got(payload.size());
  {
    auto r = game::make_file_reader(full_path, game::FileIoType::Zstd);
    EXPECT_TRUE(r->read(got.data(), got.size()));
    EXPECT_FALSE(r->eof());
    r->close();
  }
  EXPECT_EQ(got, payload);

  std::remove(payload_path.c_str());
  std::remove(full_path.c_str());
}

// ---- Replay log file round-trip (Raw + Zstd) --------------------------------

TEST(FileIo, ReplayRoundTripRaw) {
  const std::string path = "/tmp/ud_test_replay.ud";
  auto log = make_minimal_log();

  game::write_replay_file(log, path);
  auto log2 = game::read_replay_file(path);
  std::remove(path.c_str());

  EXPECT_EQ(log2.abi_version, log.abi_version);
  EXPECT_EQ(log2.seed, log.seed);
  EXPECT_EQ(log2.name_a, log.name_a);
  EXPECT_EQ(log2.name_b, log.name_b);
  EXPECT_EQ(log2.expected_hash, log.expected_hash);
  EXPECT_EQ(log2.outcome.reason, log.outcome.reason);
}

TEST(FileIo, ReplayRoundTripZstd) {
  const std::string path = "/tmp/ud_test_replay.zst";
  auto log = make_minimal_log();

  game::write_replay_file(log, path);
  auto log2 = game::read_replay_file(path);
  std::remove(path.c_str());

  EXPECT_EQ(log2.abi_version, log.abi_version);
  EXPECT_EQ(log2.seed, log.seed);
  EXPECT_EQ(log2.expected_hash, log.expected_hash);
  EXPECT_EQ(log2.outcome.reason, log.outcome.reason);
}

// ---- MemoryReader / MemoryWriter edge cases ---------------------------------

TEST(FileIo, MemoryWriterRoundTrip) {
  game::MemoryWriter mw;
  mw.write_val<uint32_t>(0xDEAD);
  mw.write_val<uint32_t>(0xBEEF);
  mw.close();

  game::MemoryReader mr(mw.data);
  EXPECT_EQ(mr.read_val<uint32_t>(), 0xDEADu);
  EXPECT_EQ(mr.read_val<uint32_t>(), 0xBEEFu);
  EXPECT_FALSE(mr.eof());
  mr.close();
}

TEST(FileIo, MemoryReaderTruncationReturnsFalse) {
  std::vector<uint8_t> small = {1, 2};
  game::MemoryReader mr(small);
  uint8_t buf[10];
  EXPECT_FALSE(mr.read(buf, 10)); // asks for more than available
  EXPECT_TRUE(mr.eof());
}

TEST(FileIo, MemoryReaderReadValThrows) {
  std::vector<uint8_t> empty;
  game::MemoryReader mr(empty);
  EXPECT_THROW(mr.read_val<uint32_t>(), std::runtime_error);
}
