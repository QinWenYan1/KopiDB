#include "lsm/engine.h"
#include "lsm/level_iterator.h"
#include "config/config.h"
#include "sst/sst.h"
#include "sst/sst_iterator.h"
#include "sst/concact_iterator.h"
#include "vlog/vlog.h"
#include "block/block_cache.h"
#include "block/block_iterator.h"
#include "utils/files.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace tiny_lsm;

// ---- helpers -------------------------------------------------------

static std::string make_value(size_t sz, char fill = 'x') {
    return std::string(sz, fill);
}

static std::shared_ptr<BlockCache> make_cache() {
    return std::make_shared<BlockCache>(256, 2);
}

// ---- VLog unit tests -----------------------------------------------

class VLogOpenTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 每项测试原子创建独占目录，避免触碰其他进程正在使用的数据。
    const auto root = std::filesystem::temp_directory_path();
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto candidate =
          root / ("kopidb-vlog-open-" + std::to_string(stamp) + "-" +
                  std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        dir_ = candidate;
        return;
      }
      ASSERT_TRUE(!error || error == std::errc::file_exists) << error.message();
    }
    FAIL() << "无法创建独占的 VLog 测试目录";
  }

  ~VLogOpenTest() override {
    // 即使断言提前结束，夹具析构也会清理本次测试自己的目录。
    if (!dir_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(dir_, error);
    }
  }

  std::filesystem::path dir_;
};

TEST_F(VLogOpenTest, CreatesEmptyFile) {
  // 父目录已由调用方准备；首次打开应创建长度为零的文件。
  const auto path = dir_ / "vlog.data";
  ASSERT_FALSE(std::filesystem::exists(path));

  auto vlog = VLog::open(path.string());

  ASSERT_NE(vlog, nullptr);
  ASSERT_TRUE(std::filesystem::is_regular_file(path));
  EXPECT_EQ(std::filesystem::file_size(path), 0u);
  EXPECT_EQ(vlog->tail_offset(), 0u);
}

TEST_F(VLogOpenTest, PreservesBinaryContentsAcrossReopen) {
  // 直接预置含空字节和高位字节的内容，不依赖尚未实现的追加、读取接口。
  const auto path = dir_ / "vlog.data";
  const std::string expected{'k', '\0', 'v', static_cast<char>(0xff), '\0'};
  {
    std::ofstream seed(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(seed.is_open());
    seed.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    seed.close();
    ASSERT_TRUE(seed.good());
  }

  // 首次打开和关闭后的再次打开，都必须保留长度与逐字节内容。
  for (int attempt = 0; attempt < 2; ++attempt) {
    SCOPED_TRACE(attempt);
    {
      auto vlog = VLog::open(path.string());
      ASSERT_NE(vlog, nullptr);
      EXPECT_EQ(vlog->tail_offset(), expected.size());
    }

    EXPECT_EQ(std::filesystem::file_size(path), expected.size());
    std::ifstream input(path, std::ios::binary);
    ASSERT_TRUE(input.is_open());
    const std::string actual{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    EXPECT_FALSE(input.bad());
    EXPECT_EQ(actual, expected);
  }
}

TEST_F(VLogOpenTest, ThrowsWhenParentDirectoryDoesNotExist) {
  // open 不负责创建父目录；文件层打开失败应向调用方传播异常。
  const auto parent = dir_ / "missing_parent";
  const auto path = parent / "vlog.data";
  ASSERT_FALSE(std::filesystem::exists(parent));

  EXPECT_THROW(VLog::open(path.string()), std::runtime_error);

  EXPECT_FALSE(std::filesystem::exists(parent));
  EXPECT_FALSE(std::filesystem::exists(path));
}

// 复用独占临时目录；这些测试直接检查文件，不依赖 read_value 的实现。
class VLogAppendTest : public VLogOpenTest {};

TEST_F(VLogAppendTest, WritesRecordLayoutAndChecksum) {
  // 当前小端平台：k/abc 应占 2+1+4+3+4=14 字节。
  // 固定样本的 CRC32 为 0x7a443de8（用独立的 zlib CRC32 核对），
  // 逐字节验证长度字段宽度、字段位置和校验值，避免写读两端同错仍通过。
  const auto path = dir_ / "vlog.data";
  {
    auto vlog = VLog::open(path.string());
    EXPECT_EQ(vlog->append("k", "abc"), 0u);
    EXPECT_EQ(vlog->tail_offset(), 14u);
  }

  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open());
  const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  ASSERT_FALSE(input.bad());
  const std::vector<uint8_t> expected{
      0x01, 0x00, 0x6b, 0x03, 0x00, 0x00, 0x00,
      0x61, 0x62, 0x63, 0xe8, 0x3d, 0x44, 0x7a};
  EXPECT_EQ(actual, expected);
}

TEST_F(VLogAppendTest, PreservesValueBeyondUint16Length) {
  // 65536 字节刚好超过 uint16_t 上限，验证 value_len 使用完整的 4 字节。
  // 负载包含空字节和高位字节；独立读文件检查全部内容，避免依赖 read_value。
  const auto path = dir_ / "vlog.data";
  const std::string key = "k";
  std::string value(65536, '\0');
  for (size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<char>(i % 256);
  }
  const size_t value_len_offset = 2 + key.size();
  const size_t payload_offset = value_len_offset + 4;
  const size_t record_size = payload_offset + value.size() + 4;
  {
    auto vlog = VLog::open(path.string());
    EXPECT_EQ(vlog->append(key, value), 0u);
    EXPECT_EQ(vlog->tail_offset(), record_size);
  }
  EXPECT_EQ(std::filesystem::file_size(path), record_size);

  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open());
  const std::string actual{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
  ASSERT_FALSE(input.bad());
  ASSERT_EQ(actual.size(), record_size);

  // 当前小端平台：按文件字节解析长度，不能复用 VLog 的编码或读取逻辑。
  uint32_t stored_value_len = 0;
  for (size_t i = 0; i < 4; ++i) {
    stored_value_len |=
        static_cast<uint32_t>(static_cast<uint8_t>(actual[value_len_offset + i]))
        << (8 * i);
  }
  EXPECT_EQ(stored_value_len, 65536u);
  EXPECT_EQ(actual.substr(payload_offset, value.size()), value);
}

TEST_F(VLogAppendTest, ContinuesAtPreviousRecordEndAfterReopen) {
  // 第一条记录占 19 字节；关闭重开后，第二条必须紧接其后。
  // 第二条含空字节和高位字节，长度为 13，不能按 C 字符串截断。
  const auto path = dir_ / "vlog.data";
  {
    auto vlog = VLog::open(path.string());
    EXPECT_EQ(vlog->append("a", "12345678"), 0u);
    EXPECT_EQ(vlog->tail_offset(), 19u);
  }
  {
    auto vlog = VLog::open(path.string());
    EXPECT_EQ(vlog->tail_offset(), 19u);
    EXPECT_EQ(vlog->append("b", std::string("\0\xff", 2)), 19u);
    EXPECT_EQ(vlog->tail_offset(), 32u);
  }
  EXPECT_EQ(std::filesystem::file_size(path), 32u);
}

TEST_F(VLogAppendTest, RejectsOversizedKeyWithoutChangingFile) {
  // key 长度字段只能保存 uint16_t；拒绝后文件应保持空，
  // 不能把无法表示的记录部分写入或悄悄截短 key。
  auto vlog = VLog::open((dir_ / "vlog.data").string());
  const std::string oversized_key(
      static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1, 'k');

  EXPECT_THROW(vlog->append(oversized_key, "v"), std::length_error);
  EXPECT_EQ(vlog->tail_offset(), 0u);
}

class VLogSyncTest : public VLogOpenTest {};

TEST_F(VLogSyncTest, FlushesRecordBeforeVLogIsClosed) {
  const auto path = dir_ / "vlog.data";
  auto vlog = VLog::open(path.string());
  ASSERT_EQ(vlog->append("k", "abc"), 0u);

  // 直接刷新，不先查询尾位置或读取 VLog，避免其他操作隐式刷新缓冲。
  ASSERT_NO_THROW(vlog->sync());

  // VLog 仍然打开；独立文件流必须能看到整条记录及其 CRC。
  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open());
  const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
  ASSERT_FALSE(input.bad());
  const std::vector<uint8_t> expected{
      0x01, 0x00, 0x6b, 0x03, 0x00, 0x00, 0x00,
      0x61, 0x62, 0x63, 0xe8, 0x3d, 0x44, 0x7a};
  EXPECT_EQ(actual, expected);
}

#ifndef _WIN32
class VLogSyncDeathTest : public VLogOpenTest {};

TEST_F(VLogSyncDeathTest, ReportsFlushFailure) {
  // 重新执行子进程，避免在已有线程的进程中直接 fork 后访问文件流。
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        // threadsafe 会重新运行夹具；_exit 不析构，需清理子进程自己的目录。
        const auto exit_child = [&](int code) {
          std::error_code error;
          std::filesystem::remove_all(dir_, error);
          _exit(error ? 99 : code);
        };

        struct rlimit limits;
        if (getrlimit(RLIMIT_FSIZE, &limits) != 0) exit_child(90);
        limits.rlim_cur = 0;
        if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) exit_child(91);
        if (setrlimit(RLIMIT_FSIZE, &limits) != 0) exit_child(92);

        try {
          auto vlog = VLog::open((dir_ / "vlog.data").string());
          try {
            // 总长 32 字节，预期先进入流缓冲；append 提前失败必须单独报错。
            if (vlog->append("k", std::string(21, 'v')) != 0u) exit_child(94);
          } catch (...) {
            exit_child(94);
          }

          try {
            vlog->sync();
          } catch (const std::runtime_error &error) {
            // 只接受 sync 自己向上传递的刷新异常，不能以其他异常冒充成功。
            exit_child(std::string(error.what()).find("VLog::sync:") == 0
                           ? 0
                           : 95);
          } catch (...) {
            exit_child(96);
          }
          exit_child(2);
        } catch (...) {
          exit_child(93);
        }
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

// 复用独占目录；损坏样本直接写入文件，不依赖 append 产生相同错误。
class VLogReadTest : public VLogOpenTest {
protected:
  static std::vector<uint8_t> golden_record() {
    // 当前小端布局的 k/abc，CRC32 独立核对为 0x7a443de8。
    return {0x01, 0x00, 0x6b, 0x03, 0x00, 0x00, 0x00,
            0x61, 0x62, 0x63, 0xe8, 0x3d, 0x44, 0x7a};
  }

  void write_bytes(const std::filesystem::path &path,
                   const std::vector<uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    if (!bytes.empty()) {
      output.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    ASSERT_TRUE(output.good());
  }
};

TEST_F(VLogReadTest, ReadsGoldenRecord) {
  // 固定样本独立验证读取布局与 CRC，避免写读两端同错仍然通过。
  const auto path = dir_ / "golden.data";
  ASSERT_NO_FATAL_FAILURE(write_bytes(path, golden_record()));

  auto vlog = VLog::open(path.string());
  EXPECT_EQ(vlog->read_value(0, 3), "abc");
}

TEST_F(VLogReadTest, ReadsBinaryLargeAndEmptyValuesAcrossReopen) {
  // 65536 字节验证 value_len 不会截成 uint16_t；内容遍历所有字节值。
  // 空 value 也有完整记录及 CRC，不能因为长度为零就跳过读取校验。
  const auto path = dir_ / "values.data";
  std::string large_value(65536, '\0');
  for (size_t i = 0; i < large_value.size(); ++i) {
    large_value[i] = static_cast<char>(i % 256);
  }
  const std::vector<std::string> keys{
      "large", "empty", std::string("k\0\xff", 3)};
  const std::vector<std::string> values{
      large_value, "", std::string("\0\xffv\0", 4)};
  std::vector<uint64_t> offsets;
  {
    auto vlog = VLog::open(path.string());
    for (size_t i = 0; i < keys.size(); ++i) {
      offsets.push_back(vlog->append(keys[i], values[i]));
    }
    ASSERT_EQ(offsets.front(), 0u);
    ASSERT_GT(offsets[1], offsets[0]);
    ASSERT_GT(offsets[2], offsets[1]);

    // 倒序读取不同 offset，检查每次读取都重新定位，不受前一次游标影响。
    for (size_t i = values.size(); i-- > 0;) {
      SCOPED_TRACE(i);
      EXPECT_EQ(vlog->read_value(offsets[i],
                                static_cast<uint32_t>(values[i].size())),
                values[i]);
    }
  }
  auto reopened = VLog::open(path.string());
  for (size_t i = 0; i < values.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(reopened->read_value(offsets[i],
                                  static_cast<uint32_t>(values[i].size())),
              values[i]);
  }
}

TEST_F(VLogReadTest, ConcurrentAppendsAndReadsPreserveRecordsAndOffsets) {
  // 两个写线程追加不同记录，两个读线程反复读取不同位置的种子记录，
  // 验证同一 VLog 的读写定位互不干扰，且追加返回的记录范围没有重叠。
  auto vlog = VLog::open((dir_ / "concurrent.data").string());
  struct Record {
    std::string key;
    std::string value;
    uint64_t offset = 0;
  };
  std::vector<Record> seeds{
      {"seed-a", "first-value"},
      {"seed-binary", std::string("\0\xffseed", 6)},
      {"seed-empty", ""}};
  for (auto &record : seeds) {
    record.offset = vlog->append(record.key, record.value);
  }

  constexpr size_t writer_count = 2;
  constexpr size_t reader_count = 2;
  constexpr size_t records_per_writer = 48;
  constexpr size_t reads_per_reader = 128;
  std::array<std::vector<Record>, writer_count> written;
  for (size_t writer = 0; writer < writer_count; ++writer) {
    for (size_t i = 0; i < records_per_writer; ++i) {
      const auto id = std::to_string(writer) + "/" + std::to_string(i);
      written[writer].push_back(
          {"key/" + id, "value/" + id + std::string(i % 17 + 1, '\0')});
    }
  }

  // 每个线程只写自己的结果槽；主线程在 join 后统一检查，避免断言中止线程。
  std::array<std::exception_ptr, writer_count + reader_count> errors{};
  std::array<size_t, reader_count> read_counts{};
  std::atomic<bool> start{false};
  std::vector<std::thread> workers;
  workers.reserve(writer_count + reader_count);
  try {
    for (size_t worker = 0; worker < writer_count + reader_count; ++worker) {
      workers.emplace_back([&, worker] {
        try {
          while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          if (worker < writer_count) {
            for (auto &record : written[worker]) {
              record.offset = vlog->append(record.key, record.value);
              std::this_thread::yield();
            }
          } else {
            const size_t reader = worker - writer_count;
            for (size_t i = 0; i < reads_per_reader; ++i) {
              const auto &record = seeds[(i + reader) % seeds.size()];
              if (vlog->read_value(record.offset,
                                   static_cast<uint32_t>(record.value.size())) !=
                  record.value) {
                throw std::runtime_error("并发读取种子记录内容不一致");
              }
              ++read_counts[reader];
              std::this_thread::yield();
            }
          }
        } catch (...) {
          errors[worker] = std::current_exception();
        }
      });
    }
  } catch (...) {
    // 创建后续线程失败时，先放行并回收已经启动的线程。
    // 避免析构仍可 join 的 std::thread 导致 terminate。
    start.store(true, std::memory_order_release);
    for (auto &worker : workers) {
      worker.join();
    }
    throw;
  }
  start.store(true, std::memory_order_release);
  for (auto &worker : workers) {
    worker.join();
  }

  for (size_t worker = 0; worker < errors.size(); ++worker) {
    if (errors[worker]) {
      EXPECT_NO_THROW(std::rethrow_exception(errors[worker])) << worker;
    }
  }
  for (const auto count : read_counts) {
    EXPECT_EQ(count, reads_per_reader);
  }

  std::vector<Record> all_records = seeds;
  for (const auto &records : written) {
    all_records.insert(all_records.end(), records.begin(), records.end());
  }
  std::sort(all_records.begin(), all_records.end(),
            [](const Record &left, const Record &right) {
              return left.offset < right.offset;
            });
  uint64_t expected_offset = 0;
  for (const auto &record : all_records) {
    SCOPED_TRACE(record.key);
    EXPECT_EQ(record.offset, expected_offset);
    EXPECT_EQ(vlog->read_value(record.offset,
                              static_cast<uint32_t>(record.value.size())),
              record.value);
    // 文件格式：2 字节 key_len、key、4 字节 value_len、value、4 字节 CRC。
    expected_offset += 2 + record.key.size() + 4 + record.value.size() + 4;
  }
  // tail_offset 仅在线程全部结束后读取，不扩展其并发使用约定。
  EXPECT_EQ(vlog->tail_offset(), expected_offset);
}

TEST_F(VLogReadTest, RejectsCorruptedKeyValueAndChecksum) {
  // 分别破坏 key、value 首尾及 CRC 首尾；长度不变，必须靠 CRC 发现损坏。
  for (const size_t byte_offset : {2u, 7u, 9u, 10u, 13u}) {
    SCOPED_TRACE(byte_offset);
    auto bytes = golden_record();
    bytes[byte_offset] ^= 0x01;
    const auto path = dir_ / ("corrupt-" + std::to_string(byte_offset) + ".data");
    ASSERT_NO_FATAL_FAILURE(write_bytes(path, bytes));

    // 每个错误场景使用新对象，避免失败后的 fstream 状态影响下一项检查。
    auto vlog = VLog::open(path.string());
    EXPECT_THROW(vlog->read_value(0, 3), std::runtime_error);
  }
}

TEST_F(VLogReadTest, RejectsCorruptedLengthFields) {
  // key_len 改为 0 会错位解析；value_len 的四个字节都必须参与长度核对。
  // 这些样本按调用方长度计算仍在文件内，应报告内容不一致。
  for (const size_t byte_offset : {0u, 3u, 4u, 5u, 6u}) {
    SCOPED_TRACE(byte_offset);
    auto bytes = golden_record();
    bytes[byte_offset] ^= 0x01;
    const auto path = dir_ / ("length-" + std::to_string(byte_offset) + ".data");
    ASSERT_NO_FATAL_FAILURE(write_bytes(path, bytes));

    auto vlog = VLog::open(path.string());
    EXPECT_THROW(vlog->read_value(0, 3), std::runtime_error);
  }

  // key_len 高位损坏使记录范围超出文件，应在分配、读取整条记录前拒绝。
  auto bytes = golden_record();
  bytes[1] = 0xff;
  const auto path = dir_ / "oversized-key-length.data";
  ASSERT_NO_FATAL_FAILURE(write_bytes(path, bytes));
  auto vlog = VLog::open(path.string());
  EXPECT_THROW(vlog->read_value(0, 3), std::out_of_range);
}

TEST_F(VLogReadTest, RejectsMismatchedReferenceValueLength) {
  // 后面放另一条完整记录，让偏大的引用长度仍落在文件内；
  // 长度不符必须明确拒绝，不能返回前缀，也不能跨进 CRC 或下一条记录。
  auto bytes = golden_record();
  const auto second_record = golden_record();
  bytes.insert(bytes.end(), second_record.begin(), second_record.end());
  const auto path = dir_ / "reference-length.data";
  ASSERT_NO_FATAL_FAILURE(write_bytes(path, bytes));

  for (const uint32_t value_size : {0u, 2u, 4u}) {
    SCOPED_TRACE(value_size);
    auto vlog = VLog::open(path.string());
    EXPECT_THROW(vlog->read_value(0, value_size), std::runtime_error);
  }
}

TEST_F(VLogReadTest, RejectsTruncatedRecord) {
  // 覆盖 key_len/头部/payload 截断，以及 payload 完整但 CRC 缺失或不足 4 字节。
  for (const size_t file_size : {0u, 1u, 2u, 6u, 9u, 10u, 11u, 12u, 13u}) {
    SCOPED_TRACE(file_size);
    auto bytes = golden_record();
    bytes.resize(file_size);
    const auto path = dir_ / ("truncated-" + std::to_string(file_size) + ".data");
    ASSERT_NO_FATAL_FAILURE(write_bytes(path, bytes));

    auto vlog = VLog::open(path.string());
    EXPECT_THROW(vlog->read_value(0, 3), std::out_of_range);
  }
}

TEST_F(VLogReadTest, RejectsOutOfRangeOffsets) {
  const auto path = dir_ / "offsets.data";
  const auto bytes = golden_record();
  ASSERT_NO_FATAL_FAILURE(write_bytes(path, bytes));
  const uint64_t file_size = bytes.size();

  // 剩 1 字节、恰好 EOF、超过 EOF 以及 UINT64_MAX 都不能读取 key_len。
  // 最大偏移还验证边界检查不会因 offset + 长度溢出而误放行。
  for (const uint64_t offset :
       {file_size - 1, file_size, file_size + 1,
        std::numeric_limits<uint64_t>::max()}) {
    SCOPED_TRACE(offset);
    auto vlog = VLog::open(path.string());
    EXPECT_THROW(vlog->read_value(offset, 3), std::out_of_range);
  }
}

// 原有三个 VLog 用例改用独占目录，保留测试名称和原有断言。
class VLogAppendRead : public VLogOpenTest {};
class VLogMultiAppend : public VLogOpenTest {};
class VLogReopen : public VLogOpenTest {};

TEST_F(VLogAppendRead, Single) {
    const std::string path = (dir_ / "single.data").string();

    auto vlog = VLog::open(path);
    uint64_t off = vlog->append("hello", "world");
    EXPECT_EQ(off, 0u);
    EXPECT_EQ(vlog->read_value(off, 5), "world");

}

TEST_F(VLogMultiAppend, HundredRecords) {
    const std::string path = (dir_ / "multi.data").string();

    auto vlog = VLog::open(path);
    std::vector<uint64_t> offsets;
    std::vector<std::string> values;

    for (int i = 0; i < 100; ++i) {
        std::string key = "key" + std::to_string(i);
        std::string val = make_value(i + 1, 'a' + (i % 26));
        offsets.push_back(vlog->append(key, val));
        values.push_back(val);
    }

    for (int i = 0; i < 100; ++i) {
        auto got = vlog->read_value(offsets[i], static_cast<uint32_t>(values[i].size()));
        EXPECT_EQ(got, values[i]) << "mismatch at i=" << i;
    }

}

TEST_F(VLogReopen, PersistAndContinue) {
    const std::string path = (dir_ / "reopen.data").string();

    uint64_t off0, off1;
    {
        auto vlog = VLog::open(path);
        off0 = vlog->append("k0", "value0");
        vlog->sync();
    }
    {
        auto vlog = VLog::open(path);
        EXPECT_EQ(vlog->read_value(off0, 6), "value0");
        off1 = vlog->append("k1", "value1");
        EXPECT_GT(off1, off0);
        EXPECT_EQ(vlog->read_value(off1, 6), "value1");
    }

}

// ---- SST WiscKey tests -----------------------------------------------

class SSTBuilderWiscKeyTest : public VLogOpenTest {};

TEST_F(SSTBuilderWiscKeyTest, ZeroThresholdKeepsOrdinaryModeWithAnyVLog) {
  const std::string value = "123456789012";
  for (bool has_vlog : {false, true}) {
    SCOPED_TRACE(has_vlog);
    const std::string suffix = has_vlog ? "with-vlog" : "without-vlog";
    const auto vlog_path = dir_ / (suffix + ".data");
    const auto sst_path = dir_ / (suffix + ".sst");
    auto vlog = has_vlog ? VLog::open(vlog_path.string()) : nullptr;
    auto cache = make_cache();

    SSTBuilder builder(4096, true, vlog, 0);
    builder.add("key", value, 1);
    auto sst = builder.build(0, sst_path.string(), cache);
    ASSERT_FALSE(sst->is_wisckey());
    auto it = sst->get("key", 1);
    ASSERT_FALSE(it.is_end());
    EXPECT_EQ(it.value(), value);

    // 阈值为零时保存普通格式，12 字节原文在无 VLog 重开后仍须原样返回。
    auto reopened = SST::open(
        0, FileObj::open(sst_path.string(), false), make_cache());
    ASSERT_FALSE(reopened->is_wisckey());
    auto reopened_it = reopened->get("key", 1);
    ASSERT_FALSE(reopened_it.is_end());
    EXPECT_EQ(reopened_it.value(), value);
    if (has_vlog) {
      EXPECT_EQ(vlog->tail_offset(), 0u);
      EXPECT_EQ(std::filesystem::file_size(vlog_path), 0u);
    } else {
      EXPECT_FALSE(std::filesystem::exists(vlog_path));
    }
  }
}

TEST_F(SSTBuilderWiscKeyTest, RejectsNonzeroThresholdWithoutVLog) {
  for (size_t threshold : std::array<size_t, 4>{1, 12, 13, 64}) {
    SCOPED_TRACE(threshold);
    for (bool has_bloom : {false, true}) {
      SCOPED_TRACE(has_bloom);
      EXPECT_THROW(SSTBuilder(4096, has_bloom, nullptr, threshold),
                   std::invalid_argument);
    }
  }
}

TEST_F(SSTBuilderWiscKeyTest, PositiveThresholdPreservesModeAndDependencies) {
  for (size_t threshold : std::array<size_t, 4>{1, 12, 13, 64}) {
    SCOPED_TRACE(threshold);
    for (bool has_bloom : {false, true}) {
      SCOPED_TRACE(has_bloom);
      const std::string suffix =
          std::to_string(threshold) + (has_bloom ? "-bloom" : "-plain");
      auto vlog = VLog::open((dir_ / (suffix + ".data")).string());
      SSTBuilder builder(4096, has_bloom, vlog, threshold);
      EXPECT_EQ(vlog->tail_offset(), 0u);

      // 短值均小于 12 字节；分离值严格超过传入阈值，单独检查构造参数传递。
      const std::string inline_value(std::min(threshold, size_t{11}), 'i');
      const std::string separated_value(threshold + 1, 'v');
      builder.add("aaa", inline_value, 1);
      EXPECT_EQ(vlog->tail_offset(), 0u);
      builder.add("bbb", separated_value, 1);
      EXPECT_EQ(vlog->tail_offset(), 2u + 3u + 4u + separated_value.size() + 4u);

      auto sst = builder.build(0, (dir_ / (suffix + ".sst")).string(),
                               make_cache());
      ASSERT_TRUE(sst->is_wisckey());
      // 调用方释放自己的指针后，构建所得 SST 仍持有读回分离值所需的 VLog。
      vlog.reset();
      auto inline_it = sst->get("aaa", 1);
      ASSERT_FALSE(inline_it.is_end());
      EXPECT_EQ(inline_it.value(), inline_value);
      auto separated_it = sst->get("bbb", 1);
      ASSERT_FALSE(separated_it.is_end());
      EXPECT_EQ(separated_it.value(), separated_value);
    }
  }
}

TEST_F(SSTBuilderWiscKeyTest, OrdinaryConstructorPreservesBloomOptions) {
  const std::string value = "123456789012";
  for (bool has_bloom : {false, true}) {
    SCOPED_TRACE(has_bloom);
    const auto path = dir_ / (has_bloom ? "bloom.sst" : "plain.sst");
    SSTBuilder builder(4096, has_bloom);
    builder.add("aaa", value, 1);
    builder.add("zzz", "last", 1);
    auto sst = builder.build(0, path.string(), make_cache());

    // 普通格式 footer 为 24 字节；Bloom 段为空或存在应与构造选项一致。
    auto file = FileObj::open(path.string(), false);
    const auto footer_base = file.size() - 24;
    const auto bloom_offset = file.read_uint32(file.size() - 20);
    if (has_bloom) {
      EXPECT_LT(bloom_offset, footer_base);
    } else {
      EXPECT_EQ(bloom_offset, footer_base);
    }

    auto reopened = SST::open(
        0, FileObj::open(path.string(), false), make_cache());
    for (const auto &table : {sst, reopened}) {
      EXPECT_FALSE(table->is_wisckey());
      auto first = table->get("aaa", 1);
      ASSERT_FALSE(first.is_end());
      EXPECT_EQ(first.value(), value);
      auto last = table->get("zzz", 1);
      ASSERT_FALSE(last.is_end());
      EXPECT_EQ(last.value(), "last");
      EXPECT_TRUE(table->get("mmm", 1).is_end());
    }
  }
}

class SSTBuilderAddTest : public VLogOpenTest {
protected:
  static std::string binary_value(size_t length) {
    std::string value(length, '\0');
    for (size_t i = 0; i < length; ++i) {
      value[i] = static_cast<char>((i * 37) % 256);
    }
    return value;
  }

  static void check_raw_value(const std::string &raw,
                              const std::string &expected,
                              const std::shared_ptr<VLog> &vlog,
                              bool separated, uint64_t expected_offset) {
    if (!separated) {
      EXPECT_EQ(raw, expected);
      return;
    }

    // 直接核对 Block 中的 12 字节引用，不经过尚待配套的 resolve_value。
    ASSERT_EQ(raw.size(), 12u);
    uint64_t offset = 0;
    uint32_t value_size = 0;
    std::memcpy(&offset, raw.data(), sizeof(offset));
    std::memcpy(&value_size, raw.data() + sizeof(offset), sizeof(value_size));
    ASSERT_EQ(offset, expected_offset);
    ASSERT_EQ(value_size, expected.size());
    ASSERT_NE(vlog, nullptr);
    EXPECT_EQ(vlog->read_value(offset, value_size), expected);
  }

  void check_separation_cases(
      size_t threshold, const std::vector<std::pair<size_t, bool>> &cases) {
    SCOPED_TRACE(threshold);
    const auto suffix = std::to_string(threshold);
    auto vlog = VLog::open((dir_ / (suffix + ".data")).string());
    SSTBuilder builder(4096, true, vlog, threshold);
    std::vector<std::string> values;
    std::vector<uint64_t> offsets;
    uint64_t expected_tail = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
      SCOPED_TRACE(cases[i].first);
      const auto key = "key" + std::to_string(i);
      values.push_back(binary_value(cases[i].first));
      offsets.push_back(expected_tail);
      builder.add(key, values.back(), i + 1);
      if (cases[i].second) {
        expected_tail += 2 + key.size() + 4 + values.back().size() + 4;
      }
      EXPECT_EQ(vlog->tail_offset(), expected_tail);
    }

    // 本轮只验收 add；由测试显式刷新，不要求 build 已接入 VLog::sync。
    vlog->sync();
    auto sst = builder.build(0, (dir_ / (suffix + ".sst")).string(),
                             make_cache());
    ASSERT_TRUE(sst->is_wisckey());
    ASSERT_EQ(sst->num_blocks(), 1u);
    auto block = sst->read_block(0);
    ASSERT_EQ(block->size(), cases.size());
    for (size_t i = 0; i < cases.size(); ++i) {
      SCOPED_TRACE(cases[i].first);
      const auto raw = block->get_value_binary("key" + std::to_string(i), 0);
      ASSERT_TRUE(raw.has_value());
      check_raw_value(*raw, values[i], vlog, cases[i].second, offsets[i]);
    }
    EXPECT_EQ(vlog->tail_offset(), expected_tail);
  }
};

TEST_F(SSTBuilderAddTest, ReservesReferenceLengthAtThreshold64) {
  // 12 字节必须分离；13、63 和恰好等于阈值的 64 字节仍保存完整原文。
  check_separation_cases(64, {{0, false}, {11, false}, {12, true},
                              {13, false}, {63, false}, {64, false},
                              {65, true}});
}

TEST_F(SSTBuilderAddTest, HandlesSmallThresholdBoundaries) {
  // 预期逐项列出，独立覆盖严格大于阈值与保留长度相交的边界。
  check_separation_cases(1, {{0, false}, {1, false}, {2, true},
                             {11, true}, {12, true}, {13, true}});
  check_separation_cases(12, {{0, false}, {11, false}, {12, true}, {13, true}});
  check_separation_cases(13, {{0, false}, {11, false}, {12, true},
                              {13, false}, {14, true}});
}

TEST_F(SSTBuilderAddTest, OrdinaryAndZeroThresholdKeepOriginalBytes) {
  auto vlog = VLog::open((dir_ / "unused.data").string());
  SSTBuilder ordinary(4096, true);
  SSTBuilder zero_with_vlog(4096, true, vlog, 0);
  SSTBuilder zero_without_vlog(4096, false, nullptr, 0);
  const std::array<SSTBuilder *, 3> builders{
      &ordinary, &zero_with_vlog, &zero_without_vlog};
  const std::array<std::string, 3> values{
      binary_value(12), binary_value(65), std::string{}};

  for (size_t i = 0; i < builders.size(); ++i) {
    SCOPED_TRACE(i);
    for (size_t j = 0; j < values.size(); ++j) {
      builders[i]->add("key" + std::to_string(j), values[j], j + 1);
      EXPECT_EQ(vlog->tail_offset(), 0u);
    }
    auto sst = builders[i]->build(
        i, (dir_ / (std::to_string(i) + ".sst")).string(), make_cache());
    ASSERT_FALSE(sst->is_wisckey());
    ASSERT_EQ(sst->num_blocks(), 1u);
    auto block = sst->read_block(0);
    ASSERT_EQ(block->size(), values.size());
    for (size_t j = 0; j < values.size(); ++j) {
      const auto raw = block->get_value_binary("key" + std::to_string(j), 0);
      ASSERT_TRUE(raw.has_value());
      EXPECT_EQ(*raw, values[j]);
    }
    EXPECT_EQ(vlog->tail_offset(), 0u);
    EXPECT_EQ(std::filesystem::file_size(dir_ / "unused.data"), 0u);
  }
}

TEST_F(SSTBuilderAddTest, KeepsVersionsAndTombstoneTogetherAcrossBlocks) {
  struct Record {
    std::string key;
    std::string value;
    uint64_t tranc_id;
    bool separated;
    uint64_t offset;
  };
  // 使用超过 32 位的真实版本号，同时检查墓碑、内联值和引用的编码。
  const uint64_t base = uint64_t{1} << 40;
  const std::vector<Record> records{
      {"aaa", binary_value(11), base + 6, false, 0},
      {"mmm", "", base + 9, false, 0},
      {"mmm", binary_value(12), base + 7, true, 0},
      {"mmm", binary_value(13), base + 5, false, 0},
      {"mmm", binary_value(65), base + 3, true, 25},
      {"mmm", binary_value(11), base + 1, false, 0},
      {"zzz", binary_value(64), base + 2, false, 0}};
  auto vlog = VLog::open((dir_ / "versions.data").string());
  // 同 key 的版本组实际超过 64 字节，必须强制留在同一块；下个 key 换块。
  SSTBuilder builder(64, true, vlog, 64);
  uint64_t expected_tail = 0;
  for (const auto &record : records) {
    builder.add(record.key, record.value, record.tranc_id);
    if (record.separated) {
      expected_tail += 2 + record.key.size() + 4 + record.value.size() + 4;
    }
    EXPECT_EQ(vlog->tail_offset(), expected_tail);
  }
  vlog->sync();
  auto sst = builder.build(0, (dir_ / "versions.sst").string(), make_cache());
  ASSERT_GE(sst->num_blocks(), 2u);
  const auto range = sst->get_tranc_id_range();
  EXPECT_EQ(range.first, base + 1);
  EXPECT_EQ(range.second, base + 9);

  size_t record_index = 0;
  size_t group_block = sst->num_blocks();
  size_t group_versions = 0;
  for (size_t block_index = 0; block_index < sst->num_blocks(); ++block_index) {
    SCOPED_TRACE(block_index);
    auto block = sst->read_block(block_index);
    for (BlockIterator it(block, size_t{0}, 0, true); !it.is_end(); ++it) {
      ASSERT_LT(record_index, records.size());
      const auto &expected = records[record_index++];
      SCOPED_TRACE(expected.tranc_id);
      EXPECT_EQ(it->first, expected.key);
      EXPECT_EQ(it.get_cur_tranc_id(), expected.tranc_id);
      check_raw_value(it->second, expected.value, vlog, expected.separated,
                      expected.offset);
      if (it->first == "mmm") {
        if (group_versions == 0) {
          group_block = block_index;
        }
        EXPECT_EQ(block_index, group_block);
        ++group_versions;
      }
    }
  }
  EXPECT_EQ(record_index, records.size());
  ASSERT_EQ(group_versions, 5u);
  auto block = sst->read_block(group_block);
  auto tombstone = block->get_value_binary("mmm", base + 9);
  ASSERT_TRUE(tombstone.has_value());
  EXPECT_TRUE(tombstone->empty());
  auto inline_value = block->get_value_binary("mmm", base + 6);
  ASSERT_TRUE(inline_value.has_value());
  EXPECT_EQ(*inline_value, binary_value(13));
  auto reference = block->get_value_binary("mmm", base + 4);
  ASSERT_TRUE(reference.has_value());
  check_raw_value(*reference, binary_value(65), vlog, true, 25);
  EXPECT_FALSE(block->get_value_binary("mmm", base).has_value());
  EXPECT_EQ(vlog->tail_offset(), expected_tail);
}

class SSTBuilderBuildTest : public VLogOpenTest {};

TEST_F(SSTBuilderBuildTest, PreservesFooterAndDependenciesAcrossBuildAndReopen) {
  const uint64_t base = uint64_t{1} << 40;
  const std::string value(21, 'v');
  for (bool wisckey : {false, true}) {
    SCOPED_TRACE(wisckey);
    for (bool has_bloom : {false, true}) {
      SCOPED_TRACE(has_bloom);
      const auto suffix = std::string(wisckey ? "wisckey" : "ordinary") +
                          (has_bloom ? "-bloom" : "-plain");
      const auto path = dir_ / (suffix + ".sst");
      const auto vlog_path = dir_ / (suffix + ".data");
      std::weak_ptr<VLog> retained_vlog;
      std::shared_ptr<SST> sst;
      {
        auto vlog = wisckey ? VLog::open(vlog_path.string()) : nullptr;
        retained_vlog = vlog;
        auto builder = wisckey ? SSTBuilder(4096, has_bloom, vlog, 1)
                               : SSTBuilder(4096, has_bloom);
        builder.add("aaa", value, base + 9);
        builder.add("bbb", "i", base + 5);
        builder.add("ccc", "", base + 7);
        builder.add("zzz", value, base + 3);
        // build 前不读 VLog 或查询尾位置，刷新必须由 build 完成。
        sst = builder.build(37, path.string(), make_cache());
        if (wisckey) {
          // 两条分离记录各占 2+3+4+21+4=34 字节，独立检查磁盘长度。
          EXPECT_EQ(std::filesystem::file_size(vlog_path), 68u);
        }
      }

      ASSERT_NE(sst, nullptr);
      auto file = FileObj::open(path.string(), false);
      const size_t footer_size = wisckey ? 26 : 24;
      ASSERT_GE(file.size(), footer_size);
      const size_t footer_base = file.size() - footer_size;
      const auto meta_offset = file.read_uint32(footer_base);
      const auto bloom_offset = file.read_uint32(footer_base + 4);
      ASSERT_LT(meta_offset, bloom_offset);
      if (has_bloom) {
        ASSERT_LT(bloom_offset, footer_base);
      } else {
        // 关闭 Bloom 时，bloom_offset 恰好位于 24B/26B footer 起点。
        ASSERT_EQ(bloom_offset, footer_base);
      }
      EXPECT_EQ(file.read_uint64(footer_base + 8), base + 3);
      EXPECT_EQ(file.read_uint64(footer_base + 16), base + 9);
      const auto metadata = BlockMeta::decode_meta_from_slice(
          file.read_to_slice(meta_offset, bloom_offset - meta_offset));
      ASSERT_EQ(metadata.size(), 1u);
      EXPECT_EQ(metadata.front().offset, 0u);
      EXPECT_EQ(metadata.front().first_key, "aaa");
      EXPECT_EQ(metadata.front().last_key, "zzz");
      if (wisckey) {
        EXPECT_EQ(file.read_uint8(footer_base + 24), 1u);
        EXPECT_EQ(file.read_uint8(footer_base + 25), 0x4bu);
        // builder 与调用方指针都已销毁，返回的 SST 仍须持有 VLog。
        EXPECT_FALSE(retained_vlog.expired());
      }

      // open 独立重开 VLog 与 SST，不借用 build 保留的 VLog 对象或块缓存。
      auto reopened_vlog = wisckey ? VLog::open(vlog_path.string()) : nullptr;
      const std::weak_ptr<VLog> reopened_dependency = reopened_vlog;
      auto reopened = SST::open(37, FileObj::open(path.string(), false),
                                make_cache(), reopened_vlog);
      reopened_vlog.reset();
      if (wisckey) EXPECT_FALSE(reopened_dependency.expired());

      for (const auto &table : {sst, reopened}) {
        ASSERT_NE(table, nullptr);
        EXPECT_EQ(table->is_wisckey(), wisckey);
        EXPECT_EQ(table->get_sst_id(), 37u);
        EXPECT_EQ(table->get_first_key(), "aaa");
        EXPECT_EQ(table->get_last_key(), "zzz");
        EXPECT_EQ(table->sst_size(), file.size());
        ASSERT_EQ(table->num_blocks(), 1u);
        EXPECT_EQ(table->get_tranc_id_range(),
                  std::make_pair(base + 3, base + 9));
        auto first = table->get("aaa", base + 9);
        ASSERT_FALSE(first.is_end());
        EXPECT_EQ(first.value(), value);
        EXPECT_EQ(first.get_cur_tranc_id(), base + 9);
        auto short_value = table->get("bbb", base + 9);
        ASSERT_FALSE(short_value.is_end());
        EXPECT_EQ(short_value.value(), "i");
        auto tombstone = table->get("ccc", base + 9);
        ASSERT_FALSE(tombstone.is_end());
        EXPECT_TRUE(tombstone.value().empty());
        auto last = table->get("zzz", base + 9);
        ASSERT_FALSE(last.is_end());
        EXPECT_EQ(last.value(), value);
        EXPECT_EQ(last.get_cur_tranc_id(), base + 3);
        EXPECT_TRUE(table->get("aaa", base + 8).is_end());
      }
    }
  }
}

TEST_F(SSTBuilderBuildTest, RejectsEmptyBuilderWithoutCreatingSST) {
  auto vlog = VLog::open((dir_ / "empty.data").string());
  for (bool wisckey : {false, true}) {
    SCOPED_TRACE(wisckey);
    const auto path = dir_ / (wisckey ? "wisckey.sst" : "ordinary.sst");
    auto builder = wisckey ? SSTBuilder(4096, false, vlog, 1)
                           : SSTBuilder(4096, false);
    EXPECT_THROW(builder.build(0, path.string(), make_cache()),
                 std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(path));
  }
}

#ifndef _WIN32
class SSTBuilderBuildDeathTest : public VLogOpenTest {};

TEST_F(SSTBuilderBuildDeathTest, StopsBeforeCreatingSSTWhenVLogSyncFails) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        // threadsafe 子进程会重建夹具；_exit 前清理它自己的独占目录。
        const auto exit_child = [&](int code) {
          std::error_code error;
          std::filesystem::remove_all(dir_, error);
          _exit(error ? 99 : code);
        };
        try {
          const auto vlog_path = dir_ / "values.data";
          const auto sst_path = dir_ / "result.sst";
          auto vlog = VLog::open(vlog_path.string());
          SSTBuilder builder(4096, false, vlog, 1);
          try {
            // 32 字节记录先成功进入缓冲，不能把 add 失败当作 build 失败。
            builder.add("k", std::string(21, 'v'), 1);
          } catch (...) {
            exit_child(94);
          }
          if (std::filesystem::file_size(vlog_path) != 0u) exit_child(94);

          // 限制只作用于子进程，且在 add 成功后才启用。
          struct rlimit limits;
          if (getrlimit(RLIMIT_FSIZE, &limits) != 0) exit_child(90);
          limits.rlim_cur = 0;
          if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR) exit_child(91);
          if (setrlimit(RLIMIT_FSIZE, &limits) != 0) exit_child(92);

          try {
            builder.build(0, sst_path.string(), make_cache());
          } catch (const std::runtime_error &error) {
            if (std::string(error.what()).find("VLog::sync:") != 0)
              exit_child(95);
            exit_child(std::filesystem::exists(sst_path) ? 97 : 0);
          } catch (...) {
            exit_child(96);
          }
          exit_child(2);
        } catch (...) {
          exit_child(93);
        }
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

// 复用独占目录和二进制样本；读取断言只经过 SST 的公开读取入口。
class SSTResolveValueTest : public SSTBuilderAddTest {
protected:
  struct Record {
    std::string key;
    std::string value;
    uint64_t tranc_id;
  };

  static void check_iterator(SstIterator &it, const Record &expected) {
    ASSERT_FALSE(it.is_end());
    ASSERT_TRUE(it.is_valid());
    EXPECT_EQ(it.get_cur_tranc_id(), expected.tranc_id);
    // 先复制迭代器，再分别验证各读取入口，避免先前解引用的缓存影响断言。
    auto key_it = it;
    auto arrow_it = it;
    auto value_it = it;
    EXPECT_EQ(key_it.key(), expected.key);
    EXPECT_EQ(arrow_it->first, expected.key);
    EXPECT_EQ(arrow_it->second, expected.value);
    EXPECT_EQ(*it, std::make_pair(expected.key, expected.value));
    EXPECT_EQ(value_it.value(), expected.value);
  }

  static void check_view(
      const std::shared_ptr<SST> &sst, uint64_t snapshot,
      const std::vector<Record> &expected,
      const std::function<bool(uint64_t)> &is_aborted = {}) {
    auto it = sst->begin(snapshot, false, is_aborted);
    for (const auto &record : expected) {
      SCOPED_TRACE(record.key);
      auto found = sst->get(record.key, snapshot, is_aborted);
      check_iterator(found, record);
      check_iterator(it, record);
      ++it;
    }
    EXPECT_TRUE(it.is_end());
    EXPECT_EQ(it, sst->end());
  }
};

TEST_F(SSTResolveValueTest, MixedLengthsReadThroughEveryInterfaceAfterReopen) {
  const uint64_t base = uint64_t{1} << 40;
  const std::array<size_t, 7> lengths{0, 11, 12, 13, 63, 64, 65};
  const auto path = dir_ / "mixed.sst";
  const auto vlog_path = dir_ / "mixed.data";
  auto vlog = VLog::open(vlog_path.string());
  SSTBuilder builder(128, true, vlog, 64);
  std::vector<Record> records;
  for (size_t i = 0; i < lengths.size(); ++i) {
    records.push_back({"key" + std::to_string(i), binary_value(lengths[i]),
                       base + i + 1});
    const auto &record = records.back();
    builder.add(record.key, record.value, record.tranc_id);
  }
  auto sst = builder.build(71, path.string(), make_cache());
  // 独立重开文件、VLog 和缓存，不能借用 build 留下的读取状态。
  auto reopened = SST::open(71, FileObj::open(path.string(), false), make_cache(),
                            VLog::open(vlog_path.string()));
  for (const auto &table : {sst, reopened}) {
    ASSERT_TRUE(table->is_wisckey());
    ASSERT_GE(table->num_blocks(), 2u);
    check_view(table, base + 100, records);
    EXPECT_TRUE(table->get("missing", base + 100).is_end());
    auto range = sst_iters_monotony_predicate(
        table, base + 100, [](const std::string &key) {
          return key < "key1" ? 1 : (key > "key5" ? -1 : 0);
        });
    ASSERT_TRUE(range.has_value());
    auto it = range->first;
    for (size_t i = 1; i <= 5; ++i) {
      ASSERT_NE(it, range->second);
      check_iterator(it, records[i]);
      ++it;
    }
    EXPECT_EQ(it, range->second);
  }
}

TEST_F(SSTResolveValueTest, SmallAndDisabledThresholdsPreserveOriginalBytes) {
  const std::array<size_t, 7> lengths{0, 1, 2, 11, 12, 13, 14};
  const std::array<size_t, 5> thresholds{1, 12, 13, 0, 0};
  // 最后一轮使用普通二参数构造；阈值 0 则验证四参数关闭路径。
  for (size_t variant = 0; variant < thresholds.size(); ++variant) {
    const auto threshold = thresholds[variant];
    SCOPED_TRACE(variant);
    const auto suffix = std::to_string(variant);
    const auto path = dir_ / (suffix + ".sst");
    const auto vlog_path = dir_ / (suffix + ".data");
    auto vlog = VLog::open(vlog_path.string());
    auto builder = variant == 4 ? SSTBuilder(4096, false)
                                : SSTBuilder(4096, true, vlog, threshold);
    std::vector<Record> records;
    for (size_t i = 0; i < lengths.size(); ++i) {
      records.push_back({"key" + std::to_string(i), binary_value(lengths[i]),
                         i + 1});
      const auto &record = records.back();
      builder.add(record.key, record.value, record.tranc_id);
    }
    auto sst = builder.build(variant, path.string(), make_cache());
    auto reopened = SST::open(
        variant, FileObj::open(path.string(), false), make_cache(),
        threshold == 0 ? nullptr : VLog::open(vlog_path.string()));
    for (const auto &table : {sst, reopened}) {
      EXPECT_EQ(table->is_wisckey(), threshold != 0);
      check_view(table, 100, records);
    }
    if (threshold == 0) {
      EXPECT_EQ(vlog->tail_offset(), 0u);
      EXPECT_EQ(std::filesystem::file_size(vlog_path), 0u);
    }
  }
}

TEST_F(SSTResolveValueTest, KeepsSnapshotsAndAbortedVersionsAcrossBlocks) {
  const uint64_t base = uint64_t{1} << 40;
  // 与 MemTable 刷盘一致：key 升序，同 key 的真实版本降序。
  const std::vector<Record> records{
      {"aaa", binary_value(12), base + 12},
      {"bbb", binary_value(65), base + 11},
      {"bbb", binary_value(13), base + 7},
      {"bbb", binary_value(12), base + 3},
      {"ccc", "", base + 10},
      {"ccc", binary_value(65), base + 4},
      {"ddd", binary_value(64), base + 9},
      {"ddd", binary_value(12), base + 5},
      {"eee", binary_value(11), base + 2},
      {"zzz", binary_value(65), base + 1}};
  const auto is_aborted = [base](uint64_t version) {
    return version == base + 12 || version == base + 11 || version == base + 9;
  };
  const auto path = dir_ / "versions.sst";
  const auto vlog_path = dir_ / "versions.data";
  auto vlog = VLog::open(vlog_path.string());
  SSTBuilder builder(64, true, vlog, 64);
  for (const auto &record : records) {
    builder.add(record.key, record.value, record.tranc_id);
  }
  auto sst = builder.build(73, path.string(), make_cache());
  auto reopened = SST::open(73, FileObj::open(path.string(), false), make_cache(),
                            VLog::open(vlog_path.string()));
  for (const auto &table : {sst, reopened}) {
    ASSERT_GE(table->num_blocks(), 4u);
    EXPECT_EQ(table->get_tranc_id_range(), std::make_pair(base + 1, base + 12));
    check_view(table, 0, {records[0], records[1], records[4], records[6],
                          records[8], records[9]});
    check_view(table, 0,
               {records[2], records[4], records[7], records[8], records[9]},
               is_aborted);
    check_view(table, base + 6,
               {records[3], records[5], records[7], records[8], records[9]},
               is_aborted);
    check_view(table, base, {}, is_aborted);
    EXPECT_TRUE(table->get("aaa", 0, is_aborted).is_end());
    EXPECT_TRUE(table->get("bbb", base + 2, is_aborted).is_end());

    auto range = sst_iters_monotony_predicate(
        table, 0,
        [](const std::string &key) {
          return key < "bbb" ? 1 : (key > "ddd" ? -1 : 0);
        },
        is_aborted);
    ASSERT_TRUE(range.has_value());
    auto range_it = range->first;
    for (const auto index : {2u, 4u, 7u}) {
      ASSERT_NE(range_it, range->second);
      check_iterator(range_it, records[index]);
      ++range_it;
    }
    EXPECT_EQ(range_it, range->second);

    // 保留全部版本时，解引用仍返回原文，版本号仍是记录自身的 64 位值。
    auto all = table->begin(0, true);
    for (const auto &record : records) {
      check_iterator(all, record);
      EXPECT_EQ(all.get_tranc_id(), record.tranc_id);
      ++all;
    }
    EXPECT_TRUE(all.is_end());
    auto filtered = table->begin(base + 10, true, is_aborted);
    for (const auto index : {2u, 3u, 4u, 5u, 7u, 8u, 9u}) {
      check_iterator(filtered, records[index]);
      EXPECT_EQ(filtered.get_tranc_id(), records[index].tranc_id);
      ++filtered;
    }
    EXPECT_TRUE(filtered.is_end());
  }
}

TEST_F(SSTResolveValueTest, MissingVLogThrowsOnlyWhenReferenceIsDereferenced) {
  const auto path = dir_ / "missing-vlog.sst";
  auto vlog = VLog::open((dir_ / "values.data").string());
  SSTBuilder builder(4096, true, vlog, 64);
  builder.add("aaa", "", 4);
  builder.add("bbb", binary_value(13), 3);
  builder.add("ccc", binary_value(12), 2);
  builder.add("ddd", binary_value(65), 1);
  builder.build(74, path.string(), make_cache());

  // open 和定位只读取 SST；现有约定在首次解析引用时报告依赖缺失。
  std::shared_ptr<SST> sst;
  ASSERT_NO_THROW(sst = SST::open(74, FileObj::open(path.string(), false),
                                 make_cache()));
  ASSERT_TRUE(sst->is_wisckey());
  auto tombstone = sst->get("aaa", 4);
  check_iterator(tombstone, {"aaa", "", 4});
  auto inline_value = sst->get("bbb", 4);
  check_iterator(inline_value, {"bbb", binary_value(13), 3});
  for (const auto &key : {"ccc", "ddd"}) {
    SCOPED_TRACE(key);
    auto it = sst->get(key, 4);
    ASSERT_FALSE(it.is_end());
    EXPECT_THROW(it.value(), std::runtime_error);
    EXPECT_THROW((void)*it, std::runtime_error);
    EXPECT_THROW((void)it.operator->(), std::runtime_error);
  }
}

class SSTIteratorReadModeTest : public SSTResolveValueTest {
protected:
  static std::vector<Record> collect_stored_records(
      const std::shared_ptr<SST> &sst) {
    // 直接读取 Block 中的字节，作为存储值的独立对照；不经过 SST 解引用。
    std::vector<Record> records;
    for (size_t block_index = 0; block_index < sst->num_blocks(); ++block_index) {
      BlockIterator it(sst->read_block(block_index), 0, 0, true);
      for (; !it.is_end(); ++it) {
        const auto entry = *it;
        records.push_back({entry.first, entry.second, it.get_cur_tranc_id()});
      }
    }
    return records;
  }

  static void check_cached_record(SstIterator &it, const Record &record,
                                  bool expected_reference = false) {
    ASSERT_TRUE(it.is_valid());
    // 从基类接口先查类型，尚未读取 value；合并模式也不能因此访问 VLog。
    BaseIterator &base = it;
    EXPECT_EQ(base.is_vlog_ref(), expected_reference);
    EXPECT_EQ(it.key(), record.key);
    EXPECT_EQ(it.get_cur_tranc_id(), record.tranc_id);
    // 先调用 value() 填充缓存，再检查 * 和 ->，确保三个入口使用同一模式。
    EXPECT_EQ(it.value(), record.value);
    EXPECT_EQ(it->first, record.key);
    EXPECT_EQ(it->second, record.value);
    EXPECT_EQ(*it, std::make_pair(record.key, record.value));
    // 同一位置反复读取，缓存不能因为移动临时字符串而丢失内容。
    EXPECT_EQ(*it, std::make_pair(record.key, record.value));
    EXPECT_EQ(it->second, record.value);
    EXPECT_EQ(it.value(), record.value);
    EXPECT_EQ(base.is_vlog_ref(), expected_reference);
  }
};

// 目的：Query 返回原值，Compaction 返回真实存储字节；两者保留同一记录版本。
// 普通 SST 的12B原文、WiscKey的12B引用、内联值和墓碑都不能混淆。
TEST_F(SSTIteratorReadModeTest, QueryAndCompactionReturnTheirOwnRepresentation) {
  const uint64_t base = uint64_t{1} << 40;
  const std::vector<Record> records{
      {"key0", "", base + 5}, {"key1", binary_value(11), base + 4},
      {"key2", binary_value(12), base + 3},
      {"key3", binary_value(65), base + 2},
      {"key4", binary_value(13), base + 1}};
  for (bool wisckey : {false, true}) {
    SCOPED_TRACE(wisckey);
    const auto prefix = wisckey ? "wisckey" : "ordinary";
    auto vlog = VLog::open((dir_ / (std::string(prefix) + ".data")).string());
    auto builder = wisckey ? SSTBuilder(64, true, vlog, 64)
                           : SSTBuilder(64, true);
    for (const auto &record : records) {
      builder.add(record.key, record.value, record.tranc_id);
    }
    auto sst = builder.build(80, (dir_ / (std::string(prefix) + ".sst")).string(),
                              make_cache());
    const auto stored = collect_stored_records(sst);
    ASSERT_EQ(stored.size(), records.size());
    // 从 SST 创建遍历：旧调用默认 Query，合并调用显式选择 Compaction。
    auto query = sst->begin(0, true);
    auto compact = sst->begin(0, true, {}, ValueReadMode::Compaction);
    for (size_t i = 0; i < records.size(); ++i) {
      SCOPED_TRACE(records[i].key);
      ASSERT_EQ(stored[i].key, records[i].key);
      ASSERT_EQ(stored[i].tranc_id, records[i].tranc_id);
      if (wisckey && (i == 2 || i == 3)) {
        ASSERT_EQ(stored[i].value.size(), 12u);
        EXPECT_NE(stored[i].value, records[i].value);
      } else {
        EXPECT_EQ(stored[i].value, records[i].value);
      }
      ASSERT_NO_FATAL_FAILURE(check_cached_record(query, records[i]));
      ASSERT_NO_FATAL_FAILURE(check_cached_record(
          compact, stored[i], wisckey && (i == 2 || i == 3)));
      ++query;
      ++compact;
    }
    EXPECT_TRUE(query.is_end());
    EXPECT_TRUE(compact.is_end());
    EXPECT_EQ(query, sst->end());
    EXPECT_EQ(compact, sst->end());
    EXPECT_FALSE(query.is_vlog_ref());
    EXPECT_FALSE(compact.is_vlog_ref());
    EXPECT_THROW(query.value(), std::runtime_error);
    EXPECT_THROW(compact.value(), std::runtime_error);
  }
}

// 目的：合并读取不依赖 VLog，且跨块／重新定位后不会沿用上一条记录的缓存。
// 源 SST 没有 VLog 指针：若 Compaction 意外调用 resolve_value，测试立即报错。
TEST_F(SSTIteratorReadModeTest, CompactionCacheSurvivesMovementWithoutVLog) {
  const auto path = dir_ / "no-vlog-handle.sst";
  auto vlog = VLog::open((dir_ / "values.data").string());
  const std::vector<Record> records{
      {"key0", binary_value(12), 10}, {"key1", binary_value(65), 9},
      {"key2", binary_value(13), 8}, {"key3", "", 7},
      {"key4", binary_value(128), 6}, {"key5", binary_value(63), 5}};
  SSTBuilder builder(48, true, vlog, 64);
  for (const auto &record : records) {
    builder.add(record.key, record.value, record.tranc_id);
  }
  builder.build(81, path.string(), make_cache());
  auto sst = SST::open(81, FileObj::open(path.string(), false), make_cache());
  ASSERT_TRUE(sst->is_wisckey());
  ASSERT_GE(sst->num_blocks(), 2u);
  const auto stored = collect_stored_records(sst);
  ASSERT_EQ(stored.size(), records.size());

  const std::array<bool, 6> references{true, true, false, false, true, false};
  auto compact = sst->begin(0, true, {}, ValueReadMode::Compaction);
  for (size_t i = 0; i < stored.size(); ++i) {
    ASSERT_NO_FATAL_FAILURE(check_cached_record(compact, stored[i], references[i]));
    ++compact;
  }
  EXPECT_TRUE(compact.is_end());
  EXPECT_FALSE(compact.is_vlog_ref());
  compact.seek_first();
  ASSERT_NO_FATAL_FAILURE(check_cached_record(compact, stored.front(), true));
  for (size_t index : {5u, 2u, 1u, 3u, 4u}) {
    compact.seek(stored[index].key);
    ASSERT_NO_FATAL_FAILURE(check_cached_record(
        compact, stored[index], references[index]));
  }
  compact.seek("missing");
  EXPECT_TRUE(compact.is_end());
  EXPECT_FALSE(compact.is_vlog_ref());
  EXPECT_THROW(compact.value(), std::runtime_error);
  EXPECT_THROW((void)*compact, std::runtime_error);
  compact.seek_first();
  ASSERT_NO_FATAL_FAILURE(check_cached_record(compact, stored.front(), true));

  // 负对照：同一 SST 的 Query 读取引用仍必须依赖 VLog。
  auto query = sst->begin(0, true);
  EXPECT_FALSE(query.is_vlog_ref());
  EXPECT_THROW(query.value(), std::runtime_error);
  EXPECT_THROW((void)*query, std::runtime_error);
  EXPECT_THROW((void)query.operator->(), std::runtime_error);
  query.seek("key2");
  ASSERT_NO_FATAL_FAILURE(check_cached_record(query, records[2]));

  // 两种模式的空迭代器都没有当前引用，不应访问空 SST。
  for (auto mode : {ValueReadMode::Query, ValueReadMode::Compaction}) {
    SstIterator empty(nullptr, 0, true, {}, mode);
    EXPECT_FALSE(empty.is_vlog_ref());
  }
}

// 目的：begin 同时传递读取模式、快照上限、回滚判断和版本保留选项。
// a@8被取消，c@10超出读上限；普通遍历选a@7，完整遍历还保留a@5。
// b@7墓碑仍交给上层处理，不能因增加读取模式就提前丢弃。
TEST_F(SSTIteratorReadModeTest, BeginPreservesModeAndMvccConditions) {
  auto vlog = VLog::open((dir_ / "begin.data").string());
  const std::vector<Record> records{
      {"a", binary_value(65), 8}, {"a", binary_value(13), 7},
      {"a", binary_value(12), 5}, {"b", "", 7},
      {"b", binary_value(65), 4}, {"c", binary_value(128), 10},
      {"c", binary_value(11), 6}};
  SSTBuilder builder(64, true, vlog, 64);
  for (const auto &record : records) {
    builder.add(record.key, record.value, record.tranc_id);
  }
  auto sst = builder.build(82, (dir_ / "begin.sst").string(), make_cache());
  ASSERT_GT(sst->num_blocks(), 1u);
  const auto stored = collect_stored_records(sst);
  ASSERT_EQ(stored.size(), records.size());
  const auto is_aborted = [](uint64_t id) { return id == 8; };

  for (bool keep_all : {false, true}) {
    const std::vector<size_t> indices = keep_all
        ? std::vector<size_t>{1, 2, 3, 4, 6}
        : std::vector<size_t>{1, 3, 6};
    for (auto mode : {ValueReadMode::Query, ValueReadMode::Compaction}) {
      SCOPED_TRACE(keep_all);
      SCOPED_TRACE(static_cast<int>(mode));
      auto it = sst->begin(8, keep_all, is_aborted, mode);
      for (const auto index : indices) {
        const bool compact = mode == ValueReadMode::Compaction;
        const auto &record = compact ? stored[index] : records[index];
        const bool is_reference = compact && (index == 2 || index == 4);
        ASSERT_NO_FATAL_FAILURE(check_cached_record(it, record, is_reference));
        ++it;
      }
      EXPECT_TRUE(it.is_end());
      EXPECT_EQ(it, sst->end());
      EXPECT_FALSE(it.is_vlog_ref());
    }
  }

  // 所有版本都超过上限时，两个读取模式都返回正常的结束位置。
  for (auto mode : {ValueReadMode::Query, ValueReadMode::Compaction}) {
    auto empty = sst->begin(1, false, is_aborted, mode);
    EXPECT_EQ(empty, sst->end());
    EXPECT_FALSE(empty.is_vlog_ref());
  }
}

class SSTMergeReadModeTest : public SSTIteratorReadModeTest {
protected:
  struct Sources {
    std::shared_ptr<SST> ordinary;
    std::shared_ptr<SST> wisckey;
    std::vector<Record> ordinary_records;
    std::vector<Record> wisckey_records;
  };

  void prepare_sources(Sources &sources) {
    sources.ordinary_records = {
        {"a", "plain-seven", 7}, {"a", "hello world!", 3},
        {"b", "old-b", 5}, {"d", "ordinary-six", 6}, {"f", "", 2}};
    sources.wisckey_records = {
        {"a", binary_value(65), 7}, {"a", binary_value(12), 5},
        {"b", "", 8}, {"c", binary_value(13), 4},
        {"d", binary_value(12), 6}, {"e", binary_value(65), 9}};
    SSTBuilder ordinary_builder(64, true);
    for (const auto &record : sources.ordinary_records) {
      ordinary_builder.add(record.key, record.value, record.tranc_id);
    }
    sources.ordinary = ordinary_builder.build(
        10, (dir_ / "ordinary.sst").string(), make_cache());

    auto vlog = VLog::open((dir_ / "merge.data").string());
    SSTBuilder wisckey_builder(64, true, vlog, 64);
    for (const auto &record : sources.wisckey_records) {
      wisckey_builder.add(record.key, record.value, record.tranc_id);
    }
    sources.wisckey = wisckey_builder.build(
        20, (dir_ / "wisckey.sst").string(), make_cache());
    ASSERT_FALSE(sources.ordinary->is_wisckey());
    ASSERT_TRUE(sources.wisckey->is_wisckey());
  }

  static void check_merged(HeapIterator &heap, const std::vector<Record> &expected,
                           const std::vector<bool> &references) {
    ASSERT_EQ(expected.size(), references.size());
    BaseIterator &it = heap;
    for (size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(i);
      ASSERT_TRUE(it.is_valid());
      EXPECT_EQ(it.is_vlog_ref(), references[i]);
      EXPECT_EQ(*it, std::make_pair(expected[i].key, expected[i].value));
      EXPECT_EQ(*heap.operator->(),
                std::make_pair(expected[i].key, expected[i].value));
      EXPECT_EQ(it.get_cur_tranc_id(), expected[i].tranc_id);
      ++it;
    }
    EXPECT_TRUE(it.is_end());
    EXPECT_FALSE(it.is_vlog_ref());
  }
};

// 目的：Query 输入合并后仍输出原值，且版本、来源优先级和墓碑语义不变。
// a@7与d@6各有两个来源，id=20应排在id=10之前；e@9由输入快照8排除。
TEST_F(SSTMergeReadModeTest, QueryKeepsOriginalValuesAndMergeOrder) {
  Sources sources;
  ASSERT_NO_FATAL_FAILURE(prepare_sources(sources));
  const auto &p = sources.ordinary_records;
  const auto &w = sources.wisckey_records;
  for (bool keep_all : {true, false}) {
    SCOPED_TRACE(keep_all);
    // 每次重新创建输入，不依赖迭代器拷贝后的独立推进行为。
    std::vector<SstIterator> input{
        sources.ordinary->begin(8, true), sources.wisckey->begin(8, true)};
    auto merged = SstIterator::merge_sst_iterator(std::move(input), 8, keep_all);
    // 手工指定期望顺序，不使用被测的比较器重新计算期望。
    const std::vector<Record> expected = keep_all
        ? std::vector<Record>{w[0], p[0], w[1], p[1], w[2], p[2],
                              w[3], w[4], p[3], p[4]}
        : std::vector<Record>{w[0], w[2], w[3], w[4], p[4]};
    ASSERT_NO_FATAL_FAILURE(check_merged(
        merged.first, expected, std::vector<bool>(expected.size(), false)));
    EXPECT_EQ(merged.first, merged.second);
  }
}

// 目的：Compaction 将原始字节和类型一起放入堆，不再次访问 VLog。
// 普通SST的12B原文仍是原值；WiscKey中的引用必须连同true标志一起保留。
TEST_F(SSTMergeReadModeTest, CompactionKeepsReferencesWithoutReadingVLog) {
  Sources sources;
  ASSERT_NO_FATAL_FAILURE(prepare_sources(sources));
  // 用没有VLog依赖的新对象替换原SST；若merge内部再次resolve，测试会抛错。
  sources.wisckey = SST::open(
      20, FileObj::open((dir_ / "wisckey.sst").string(), false), make_cache());
  const auto p = collect_stored_records(sources.ordinary);
  const auto w = collect_stored_records(sources.wisckey);
  ASSERT_EQ(p.size(), 5u);
  ASSERT_EQ(w.size(), 6u);
  ASSERT_EQ(p[1].value, "hello world!");
  ASSERT_EQ(p[1].value.size(), 12u);
  ASSERT_EQ(w[0].value.size(), 12u);
  for (bool keep_all : {true, false}) {
    SCOPED_TRACE(keep_all);
    std::vector<SstIterator> input{
        sources.ordinary->begin(8, true, {}, ValueReadMode::Compaction),
        sources.wisckey->begin(8, true, {}, ValueReadMode::Compaction)};
    auto merged = SstIterator::merge_sst_iterator(std::move(input), 8, keep_all);
    const std::vector<Record> expected = keep_all
        ? std::vector<Record>{w[0], p[0], w[1], p[1], w[2], p[2],
                              w[3], w[4], p[3], p[4]}
        : std::vector<Record>{w[0], w[2], w[3], w[4], p[4]};
    const std::vector<bool> references = keep_all
        ? std::vector<bool>{true, false, true, false, false, false,
                            false, true, false, false}
        : std::vector<bool>{true, false, false, true, false};
    ASSERT_NO_FATAL_FAILURE(check_merged(merged.first, expected, references));
    EXPECT_EQ(merged.first, merged.second);
  }

  // 相同源的Query访问仍会报缺少VLog，证明上面的合并没有偷偷还原引用。
  auto query = sources.wisckey->begin(8, true);
  EXPECT_THROW((void)*query, std::runtime_error);
  auto empty = SstIterator::merge_sst_iterator({}, 8, true);
  EXPECT_EQ(empty.first, empty.second);
  EXPECT_FALSE(empty.first.is_vlog_ref());
  std::vector<SstIterator> ends{sources.ordinary->end(), sources.wisckey->end()};
  auto exhausted = SstIterator::merge_sst_iterator(std::move(ends), 8, true);
  EXPECT_EQ(exhausted.first, exhausted.second);
  EXPECT_FALSE(exhausted.first.is_vlog_ref());
}

class ConcactReadModeTest : public SSTIteratorReadModeTest {
protected:
  std::shared_ptr<SST> build_source(size_t id, const std::vector<Record> &records,
                                    const std::shared_ptr<VLog> &vlog) {
    SSTBuilder builder(64, true, vlog, 64);
    for (const auto &record : records) {
      builder.add(record.key, record.value, record.tranc_id);
    }
    return builder.build(id, (dir_ / (std::to_string(id) + ".sst")).string(),
                         make_cache());
  }

  static void check_start(ConcactIterator &it, const Record &expected,
                          bool expected_reference = false) {
    ASSERT_TRUE(it.is_valid());
    EXPECT_FALSE(it.is_end());
    BaseIterator &base = it;
    // 在读取 value 前后都检查类型，确认转发的是当前 SST 的实际输出类型。
    EXPECT_EQ(base.is_vlog_ref(), expected_reference);
    EXPECT_EQ(it.key(), expected.key);
    EXPECT_EQ(it.get_cur_tranc_id(), expected.tranc_id);
    EXPECT_EQ(it.value(), expected.value);
    EXPECT_EQ(*it, std::make_pair(expected.key, expected.value));
    EXPECT_EQ(it->first, expected.key);
    EXPECT_EQ(it->second, expected.value);
    EXPECT_EQ(it.value(), expected.value);
    EXPECT_EQ(base.is_vlog_ref(), expected_reference);
  }
};

// 目的：构造时连续跳过不可见的前两张 SST，并把读取模式传给首张可见表。
// a@20 超过快照8，b@8已取消，第三张中的c@9也不可见，最终定位c@7。
TEST_F(ConcactReadModeTest, ConstructorKeepsModeAtFirstVisibleSst) {
  auto vlog = VLog::open((dir_ / "concat.data").string());
  std::vector<std::shared_ptr<SST>> sources{
      build_source(91, {{"a", binary_value(65), 20}}, vlog),
      build_source(92, {{"b", binary_value(65), 8}}, vlog),
      build_source(93, {{"c", binary_value(65), 9},
                        {"c", binary_value(12), 7},
                        {"c", binary_value(13), 5}}, vlog)};
  const auto is_aborted = [](uint64_t id) { return id == 8; };
  const auto stored = collect_stored_records(sources[2]);
  ASSERT_EQ(stored.size(), 3u);
  ASSERT_EQ(stored[1].value.size(), 12u);
  EXPECT_NE(stored[1].value, binary_value(12));

  auto raw_sources = sources;
  raw_sources[2] = SST::open(
      93, FileObj::open((dir_ / "93.sst").string(), false), make_cache());
  for (bool keep_all : {false, true}) {
    SCOPED_TRACE(keep_all);
    // 不传模式的旧调用仍返回原值。
    ConcactIterator query(sources, 8, keep_all, is_aborted);
    ASSERT_NO_FATAL_FAILURE(check_start(query, {"c", binary_value(12), 7}));

    // 第三张表没有VLog依赖；若构造漏传Compaction，首次取值就会报错。
    ConcactIterator compact(raw_sources, 8, keep_all, is_aborted,
                            ValueReadMode::Compaction);
    ASSERT_NO_FATAL_FAILURE(check_start(compact, stored[1], true));
  }

  // 同一无VLog源在Query模式下应报错，作为合并模式不解引用的负对照。
  ConcactIterator missing_vlog(raw_sources, 8, false, is_aborted);
  ASSERT_TRUE(missing_vlog.is_valid());
  EXPECT_FALSE(missing_vlog.is_vlog_ref());
  EXPECT_THROW(missing_vlog.value(), std::runtime_error);
}

// 目的：两种模式都能处理空输入、全部不可见和首条有效记录为墓碑。
// SST范围依次为a/b/c，没有重叠；检查只停留在构造后的当前位置。
TEST_F(ConcactReadModeTest, ConstructorHandlesEmptyInvisibleAndTombstone) {
  auto vlog = VLog::open((dir_ / "empty-or-tombstone.data").string());
  std::vector<std::shared_ptr<SST>> sources{
      build_source(94, {{"a", binary_value(65), 20}}, vlog),
      build_source(95, {{"b", "", 8}}, vlog),
      build_source(96, {{"c", "", 7}, {"c", binary_value(65), 5}}, vlog)};
  const auto skip_b = [](uint64_t id) { return id == 8; };
  const auto skip_visible = [](uint64_t id) {
    return id == 8 || id == 7 || id == 5;
  };
  for (auto mode : {ValueReadMode::Query, ValueReadMode::Compaction}) {
    SCOPED_TRACE(static_cast<int>(mode));
    ConcactIterator empty({}, 8, false, {}, mode);
    EXPECT_TRUE(empty.is_end());
    EXPECT_FALSE(empty.is_valid());
    EXPECT_FALSE(empty.is_vlog_ref());
    EXPECT_THROW(empty.value(), std::runtime_error);

    ConcactIterator invisible(sources, 8, false, skip_visible, mode);
    EXPECT_TRUE(invisible.is_end());
    EXPECT_FALSE(invisible.is_valid());
    EXPECT_FALSE(invisible.is_vlog_ref());
    EXPECT_THROW(invisible.value(), std::runtime_error);

    for (bool keep_all : {false, true}) {
      ConcactIterator tombstone(sources, 8, keep_all, skip_b, mode);
      ASSERT_NO_FATAL_FAILURE(check_start(tombstone, {"c", "", 7}));
    }
  }
}

// 目的：跨 Block、跨 SST 和连续跳过不可见表时，读取模式与 MVCC 条件都不丢失。
// 七张 SST 的 key 范围互不重叠；中间夹一张普通 SST，最后还有一张不可见表。
// Compaction 使用未绑定 VLog 的源表，遗漏模式转发就会在读取引用时报错。
TEST_F(ConcactReadModeTest, IncrementKeepsModeAcrossMixedAndInvisibleSsts) {
  auto vlog = VLog::open((dir_ / "increment.data").string());
  const std::vector<std::vector<Record>> groups{
      {{"a", binary_value(65), 7}, {"a", binary_value(13), 5},
       {"aa", binary_value(11), 6}},
      {{"b", binary_value(65), 20}},
      {{"c", binary_value(65), 8}},
      {{"d", binary_value(65), 8}, {"d", "", 7},
       {"d", binary_value(65), 4}, {"dd", binary_value(12), 6}},
      {{"e", "hello world!", 6}, {"ee", binary_value(13), 5}},
      {{"f", binary_value(65), 7}, {"ff", binary_value(11), 6}},
      {{"g", binary_value(65), 20}}};
  std::vector<std::shared_ptr<SST>> sources;
  std::vector<std::shared_ptr<SST>> raw_sources;
  std::vector<std::vector<Record>> stored;
  for (size_t i = 0; i < groups.size(); ++i) {
    const size_t id = 100 + i;
    const auto path = dir_ / (std::to_string(id) + ".sst");
    if (i == 4) {
      SSTBuilder ordinary(64, true);
      for (const auto &record : groups[i]) {
        ordinary.add(record.key, record.value, record.tranc_id);
      }
      sources.push_back(ordinary.build(id, path.string(), make_cache()));
    } else {
      sources.push_back(build_source(id, groups[i], vlog));
    }
    stored.push_back(collect_stored_records(sources.back()));
    ASSERT_EQ(stored.back().size(), groups[i].size());
    for (size_t j = 0; j < groups[i].size(); ++j) {
      ASSERT_EQ(stored.back()[j].key, groups[i][j].key);
      ASSERT_EQ(stored.back()[j].tranc_id, groups[i][j].tranc_id);
    }
    raw_sources.push_back(
        SST::open(id, FileObj::open(path.string(), false), make_cache()));
  }
  ASSERT_GT(sources.front()->num_blocks(), 1u);
  ASSERT_FALSE(raw_sources[4]->is_wisckey());
  const auto is_aborted = [](uint64_t id) { return id == 8; };

  for (auto mode : {ValueReadMode::Query, ValueReadMode::Compaction}) {
    const bool compact = mode == ValueReadMode::Compaction;
    const auto &values = compact ? stored : groups;
    for (bool keep_all : {false, true}) {
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(keep_all);
      // 手工列出过滤后的顺序；a@5与d@4仅在保留全版本时出现。
      const std::vector<Record> expected = keep_all
          ? std::vector<Record>{values[0][0], values[0][1], values[0][2],
                                values[3][1], values[3][2], values[3][3],
                                values[4][0], values[4][1],
                                values[5][0], values[5][1]}
          : std::vector<Record>{values[0][0], values[0][2], values[3][1],
                                values[3][3], values[4][0], values[4][1],
                                values[5][0], values[5][1]};
      ConcactIterator it(compact ? raw_sources : sources, 8, keep_all,
                        is_aborted, mode);
      // a@7、d@4、dd@6、f@7来自WiscKey引用；普通SST的e@6仍是12B原文。
      const std::vector<bool> references = keep_all
          ? std::vector<bool>{true, false, false, false, true, true,
                              false, false, true, false}
          : std::vector<bool>{true, false, false, true, false, false, true, false};
      ASSERT_EQ(expected.size(), references.size());
      for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_NO_FATAL_FAILURE(check_start(
            it, expected[i], compact && references[i]));
        ++it;
      }
      EXPECT_TRUE(it.is_end());
      EXPECT_FALSE(it.is_valid());
      EXPECT_FALSE(it.is_vlog_ref());
      EXPECT_THROW(it.value(), std::runtime_error);
      ++it;
      EXPECT_TRUE(it.is_end());
      EXPECT_FALSE(it.is_vlog_ref());
    }
    ConcactIterator empty({}, 8, false, {}, mode);
    ++empty;
    EXPECT_TRUE(empty.is_end());
    EXPECT_FALSE(empty.is_vlog_ref());
  }
}

class SSTWiscKeyBuildRead : public VLogOpenTest {};
class SSTBackwardCompat : public VLogOpenTest {};
class SSTTombstoneInline : public VLogOpenTest {};
class WiscKeyPutGet : public VLogOpenTest {};
class WiscKeyPutRemove : public VLogOpenTest {};

TEST_F(SSTWiscKeyBuildRead, ThresholdSeparation) {
    std::string dir = dir_.string();
    std::string vlog_path = dir + "/vlog.data";
    std::string sst_path = dir + "/sst_00000.0";

    auto vlog = VLog::open(vlog_path);
    auto cache = make_cache();

    // threshold = 64 bytes: short values inline, long values -> vlog
    SSTBuilder builder(4096, true, vlog, 64);
    builder.add("aaa", "short",                 1); // inline
    builder.add("bbb", make_value(128, 'B'),    1); // vlog
    builder.add("ccc", make_value(200, 'C'),    1); // vlog
    builder.add("ddd", "also-short",            1); // inline

    auto sst = builder.build(0, sst_path, cache);
    EXPECT_TRUE(sst->is_wisckey());

    // Verify footer magic: last byte of file must be 0x4B
    FileObj f = FileObj::open(sst_path, false);
    EXPECT_EQ(f.read_uint8(f.size() - 1), 0x4Bu);
    EXPECT_EQ(f.read_uint8(f.size() - 2), 1u); // storage_mode == 1

    // Read back through iterator
    auto it = sst->begin(1);
    EXPECT_FALSE(it.is_end());
    EXPECT_EQ((*it).second, "short");
    ++it;
    EXPECT_EQ((*it).second, make_value(128, 'B'));
    ++it;
    EXPECT_EQ((*it).second, make_value(200, 'C'));
    ++it;
    EXPECT_EQ((*it).second, "also-short");
    ++it;
    EXPECT_TRUE(it.is_end());
}

TEST_F(SSTBackwardCompat, OldFormat) {
    std::string dir = dir_.string();
    std::string sst_path = dir + "/sst_00001.0";
    auto cache = make_cache();

    // Old 2-argument builder (inline only)
    SSTBuilder builder(4096, true);
    builder.add("foo", "bar", 1);
    builder.add("zoo", "baz", 1);
    auto sst = builder.build(1, sst_path, cache);

    EXPECT_FALSE(sst->is_wisckey());

    // Reopen without vlog
    auto sst2 = SST::open(1, FileObj::open(sst_path, false), cache);
    EXPECT_FALSE(sst2->is_wisckey());

    auto it = sst2->begin(1);
    EXPECT_EQ((*it).second, "bar");
}

TEST_F(SSTTombstoneInline, EmptyValueAlwaysInline) {
    std::string dir = dir_.string();
    std::string vlog_path = dir + "/vlog.data";
    std::string sst_path = dir + "/sst_00002.0";
    auto vlog = VLog::open(vlog_path);
    auto cache = make_cache();

    // threshold = 1: even 1-byte values would go to vlog, but empty stays inline
    SSTBuilder builder(4096, true, vlog, 1);
    builder.add("tomb", "", 1); // tombstone — empty value must stay inline

    auto sst = builder.build(2, sst_path, cache);
    auto it = sst->begin(1);
    EXPECT_FALSE(it.is_end());
    EXPECT_EQ((*it).second, ""); // tombstone survives as empty
}

// ---- LSM integration tests ------------------------------------------

class EngineInitializationTest : public VLogOpenTest {};

TEST_F(EngineInitializationTest, CreatesDirectoryAndEmptyVLog) {
  // 夹具只提供父目录，数据库目录由 Engine 构造函数创建。
  const auto data_path = dir_ / "new_database";
  const auto vlog_path = data_path / "vlog.data";
  ASSERT_FALSE(std::filesystem::exists(data_path));

  LSMEngine engine(data_path.string());

  EXPECT_EQ(engine.data_dir, data_path.string());
  ASSERT_TRUE(std::filesystem::is_directory(data_path));
  ASSERT_TRUE(std::filesystem::is_regular_file(vlog_path));
  ASSERT_NE(engine.vlog_, nullptr);
  EXPECT_EQ(engine.vlog_->tail_offset(), 0u);
  EXPECT_EQ(std::filesystem::file_size(vlog_path), 0u);
  EXPECT_NE(engine.block_cache, nullptr);
  EXPECT_TRUE(engine.ssts.empty());
  EXPECT_TRUE(engine.level_sst_ids.empty());
  EXPECT_EQ(engine.next_sst_id, 1u);
  EXPECT_EQ(engine.cur_max_level, 0u);
}

TEST_F(EngineInitializationTest, LoadsMixedSSTModesWithWiscKeyDisabled) {
  // 当前阈值只控制后续写入，不能使已有 WiscKey SST 失去 VLog 依赖。
  ASSERT_EQ(TomlConfig::getInstance().getWisckeyValueThreshold(), 0u);
  const auto vlog_path = dir_ / "vlog.data";
  const uint64_t base = uint64_t{1} << 40;
  struct Record {
    std::string key;
    std::string value;
    uint64_t tranc_id;
  };
  const std::array<Record, 3> wisckey_records{{
      {"aaa", std::string("12-byte\0raw!", 12), base + 1},
      {"aab", make_value(65, 'L'), base + 2},
      {"aac", "short", base + 3},
  }};
  const Record ordinary_record{
      "zzz", std::string("plain\0value!", 12), base + 4};

  {
    auto vlog = VLog::open(vlog_path.string());
    auto cache = make_cache();
    SSTBuilder wisckey_builder(4096, true, vlog, 64);
    for (const auto &record : wisckey_records) {
      wisckey_builder.add(record.key, record.value, record.tranc_id);
    }
    auto wisckey_sst =
        wisckey_builder.build(7, (dir_ / "sst_7.0").string(), cache);
    ASSERT_TRUE(wisckey_sst->is_wisckey());

    SSTBuilder ordinary_builder(4096, true);
    ordinary_builder.add(ordinary_record.key, ordinary_record.value,
                         ordinary_record.tranc_id);
    auto ordinary_sst =
        ordinary_builder.build(11, (dir_ / "sst_11.2").string(), cache);
    ASSERT_FALSE(ordinary_sst->is_wisckey());
  }
  // 原 builder、SST、缓存和 VLog 均已销毁，读取只能依靠 Engine 新建的依赖。
  const auto original_vlog_size = std::filesystem::file_size(vlog_path);
  ASSERT_GT(original_vlog_size, 0u);

  LSMEngine engine(dir_.string());

  ASSERT_EQ(TomlConfig::getInstance().getWisckeyValueThreshold(), 0u);
  ASSERT_NE(engine.vlog_, nullptr);
  EXPECT_EQ(engine.vlog_->tail_offset(), original_vlog_size);
  EXPECT_EQ(std::filesystem::file_size(vlog_path), original_vlog_size);
  ASSERT_EQ(engine.ssts.size(), 2u);
  ASSERT_EQ(engine.level_sst_ids.size(), 2u);
  ASSERT_EQ(engine.level_sst_ids.count(0), 1u);
  ASSERT_EQ(engine.level_sst_ids.count(2), 1u);
  EXPECT_EQ(engine.level_sst_ids.at(0), (std::deque<size_t>{7}));
  EXPECT_EQ(engine.level_sst_ids.at(2), (std::deque<size_t>{11}));
  EXPECT_EQ(engine.next_sst_id, 12u);
  EXPECT_EQ(engine.cur_max_level, 2u);

  ASSERT_EQ(engine.ssts.count(7), 1u);
  const auto &wisckey_sst = engine.ssts.at(7);
  ASSERT_TRUE(wisckey_sst->is_wisckey());
  EXPECT_EQ(wisckey_sst->get_sst_id(), 7u);
  EXPECT_EQ(wisckey_sst->get_first_key(), "aaa");
  EXPECT_EQ(wisckey_sst->get_last_key(), "aac");
  EXPECT_EQ(wisckey_sst->get_tranc_id_range(),
            std::make_pair(base + 1, base + 3));
  for (const auto &record : wisckey_records) {
    SCOPED_TRACE(record.key);
    auto it = wisckey_sst->get(record.key, 0);
    ASSERT_FALSE(it.is_end());
    EXPECT_EQ(it.key(), record.key);
    EXPECT_EQ(it.value(), record.value);
    EXPECT_EQ(it.get_cur_tranc_id(), record.tranc_id);
  }

  ASSERT_EQ(engine.ssts.count(11), 1u);
  const auto &ordinary_sst = engine.ssts.at(11);
  ASSERT_FALSE(ordinary_sst->is_wisckey());
  EXPECT_EQ(ordinary_sst->get_sst_id(), 11u);
  auto ordinary_it = ordinary_sst->get(ordinary_record.key, 0);
  ASSERT_FALSE(ordinary_it.is_end());
  EXPECT_EQ(ordinary_it.key(), ordinary_record.key);
  EXPECT_EQ(ordinary_it.value(), ordinary_record.value);
  EXPECT_EQ(ordinary_it.get_cur_tranc_id(), ordinary_record.tranc_id);
  EXPECT_EQ(std::filesystem::file_size(vlog_path), original_vlog_size);
}

#ifndef _WIN32
class WiscKeyFlushDeathTest : public VLogOpenTest {
protected:
  static void check_public_reads(
      LSM &db,
      const std::vector<std::pair<std::string, std::string>> &records) {
    std::vector<std::string> keys;
    for (const auto &[key, value] : records) {
      SCOPED_TRACE(key);
      const auto actual = db.get(key);
      ASSERT_TRUE(actual.has_value());
      EXPECT_EQ(*actual, value);
      keys.push_back(key);
    }
    EXPECT_FALSE(db.get("deleted").has_value());
    EXPECT_FALSE(db.get("missing").has_value());
    keys.push_back("deleted");
    keys.push_back("missing");

    const auto batch = db.get_batch(keys);
    ASSERT_EQ(batch.size(), keys.size());
    for (size_t i = 0; i < records.size(); ++i) {
      EXPECT_EQ(batch[i].first, records[i].first);
      ASSERT_TRUE(batch[i].second.has_value());
      EXPECT_EQ(*batch[i].second, records[i].second);
    }
    EXPECT_EQ(batch[records.size()].first, "deleted");
    EXPECT_FALSE(batch[records.size()].second.has_value());
    EXPECT_EQ(batch.back().first, "missing");
    EXPECT_FALSE(batch.back().second.has_value());

    auto it = db.begin(0);
    for (size_t i = 0; i < records.size(); ++i) {
      ASSERT_FALSE(it.is_end());
      EXPECT_EQ(*it, records[i]);
      EXPECT_EQ(it.get_cur_tranc_id(), i + 1);
      ++it;
    }
    EXPECT_TRUE(it.is_end());
    EXPECT_EQ(it, db.end());
  }

  static void run_flush_and_reopen(const std::filesystem::path &dir) {
    // 子进程首次访问配置前，只创建文件；避免构造任何存储组件抢先初始化单例。
    const auto config_path = dir / "wisckey.toml";
    std::ofstream config_file(config_path);
    ASSERT_TRUE(config_file.is_open());
    config_file << R"toml([lsm.core]
LSM_TOL_MEM_SIZE_LIMIT = 67108864
LSM_PER_MEM_SIZE_LIMIT = 4194304
LSM_BLOCK_SIZE = 4096
LSM_SST_LEVEL_RATIO = 4
[lsm.cache]
LSM_BLOCK_CACHE_CAPACITY = 256
LSM_BLOCK_CACHE_K = 2
[bloom_filter]
BLOOM_FILTER_EXPECTED_SIZE = 65536
BLOOM_FILTER_EXPECTED_ERROR_RATE = 0.1
[lsm.wisckey]
WISCKEY_VALUE_THRESHOLD = 64
)toml";
    config_file.close();
    ASSERT_TRUE(config_file.good());
    ASSERT_EQ(TomlConfig::getInstance(config_path.string())
                  .getWisckeyValueThreshold(), 64u);

    std::vector<std::pair<std::string, std::string>> records;
    for (const size_t length : {11u, 12u, 13u, 64u, 65u}) {
      std::string value(length, '\0');
      for (size_t i = 0; i < length; ++i) {
        value[i] = static_cast<char>((i * 37) % 256);
      }
      records.emplace_back("key_" + std::to_string(length), std::move(value));
    }

    const auto data_path = dir / "database";
    const auto vlog_path = data_path / "vlog.data";
    {
      LSM db(data_path.string());
      for (const auto &[key, value] : records) {
        db.put(key, value);
      }
      db.put("deleted", records.back().second);
      db.remove("deleted");
      // 大内存上限保证本例由显式 flush_all 触发值分离。
      EXPECT_EQ(std::filesystem::file_size(vlog_path), 0u);
      db.flush_all();

      ASSERT_GT(std::filesystem::file_size(vlog_path), 0u);
      size_t sst_count = 0;
      for (const auto &entry : std::filesystem::directory_iterator(data_path)) {
        if (!entry.is_regular_file() ||
            !entry.path().filename().string().starts_with("sst_")) {
          continue;
        }
        ++sst_count;
        auto file = FileObj::open(entry.path().string(), false);
        ASSERT_GE(file.size(), 26u);
        EXPECT_EQ(file.read_uint8(file.size() - 1), 0x4bu);
        EXPECT_EQ(file.read_uint8(file.size() - 2), 1u);
      }
      ASSERT_GT(sst_count, 0u);
      ASSERT_NO_FATAL_FAILURE(check_public_reads(db, records));
    }

    // 前一个 LSM 已正常关闭；新实例从相同目录加载后仍返回完整原值和墓碑语义。
    {
      LSM reopened(data_path.string());
      ASSERT_NO_FATAL_FAILURE(check_public_reads(reopened, records));
    }
  }
};

TEST_F(WiscKeyFlushDeathTest, FlushesSeparatedValuesAndReopensThroughPublicAPI) {
  // 重执行隔离 TomlConfig 单例，使本例确实使用非零阈值且不影响其他测试。
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        int exit_code = 0;
        try {
          run_flush_and_reopen(dir_);
          exit_code = ::testing::Test::HasFailure() ? 1 : 0;
        } catch (const std::exception &error) {
          ADD_FAILURE() << error.what();
          exit_code = 90;
        } catch (...) {
          exit_code = 91;
        }
        // helper 返回或抛异常均已析构数据库；随后清理子进程自己的独占目录。
        std::error_code error;
        std::filesystem::remove_all(dir_, error);
        std::_Exit(error ? 99 : exit_code);
      },
      ::testing::ExitedWithCode(0), "");
}

class WiscKeyCompactionDeathTest : public VLogOpenTest {
protected:
  using Expected =
      std::map<std::string, std::pair<std::string, uint64_t>>;

  static std::string binary_value(size_t length, unsigned seed) {
    std::string value(length, '\0');
    for (size_t i = 0; i < length; ++i) {
      value[i] = static_cast<char>((i * 37 + seed) % 256);
    }
    return value;
  }

  static void configure(const std::filesystem::path &dir) {
    // 先用较大的单表上限控制源 SST 数量；合并前再缩小目标 SST 大小。
    // threadsafe 子进程首次访问配置，保证真实启用值分离且不污染其他测试。
    const auto path = dir / "wisckey.toml";
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << R"toml([lsm.core]
LSM_TOL_MEM_SIZE_LIMIT = 67108864
LSM_PER_MEM_SIZE_LIMIT = 1048576
LSM_BLOCK_SIZE = 128
LSM_SST_LEVEL_RATIO = 2
[lsm.cache]
LSM_BLOCK_CACHE_CAPACITY = 64
LSM_BLOCK_CACHE_K = 2
[bloom_filter]
BLOOM_FILTER_EXPECTED_SIZE = 4096
BLOOM_FILTER_EXPECTED_ERROR_RATE = 0.1
[lsm.wisckey]
WISCKEY_VALUE_THRESHOLD = 64
)toml";
    output.close();
    ASSERT_TRUE(output.good());
    const auto &config = TomlConfig::getInstance(path.string());
    ASSERT_EQ(config.getWisckeyValueThreshold(), 64u);
    ASSERT_EQ(config.getLsmSstLevelRatio(), 2);
  }

  static size_t count_level(const std::filesystem::path &dir, size_t level) {
    size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      const auto name = entry.path().filename().string();
      if (entry.is_regular_file() && name.starts_with("sst_") &&
          name.ends_with("." + std::to_string(level))) {
        ++count;
      }
    }
    return count;
  }

  static void check_sst_modes(const std::filesystem::path &dir) {
    // 每张新输出表都必须沿用 WiscKey 模式，尤其覆盖 gen_sst_from_iter
    // 达到目标大小后重建 builder 的分支，不能只有第一张表正确。
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      if (!entry.is_regular_file() ||
          !entry.path().filename().string().starts_with("sst_")) {
        continue;
      }
      SCOPED_TRACE(entry.path().string());
      auto file = FileObj::open(entry.path().string(), false);
      ASSERT_GE(file.size(), 26u);
      EXPECT_EQ(file.read_uint8(file.size() - 1), 0x4bu);
      EXPECT_EQ(file.read_uint8(file.size() - 2), 1u);
    }
  }

  static void check_scan(LSM &db, uint64_t snapshot,
                         const Expected &expected) {
    // 使用公开的带版本遍历，核对历史值、真实版本和墓碑处理；
    // helper 结束时销毁迭代器，避免持有 SST 读锁后又触发刷盘。
    auto it = db.begin(snapshot);
    for (const auto &[key, record] : expected) {
      SCOPED_TRACE(key);
      ASSERT_FALSE(it.is_end());
      EXPECT_EQ(it->first, key);
      EXPECT_EQ(it->second, record.first);
      EXPECT_EQ(it.get_cur_tranc_id(), record.second);
      ++it;
    }
    EXPECT_TRUE(it.is_end());
    EXPECT_EQ(it, db.end());
  }

  static void check_latest(LSM &db, const Expected &expected) {
    std::vector<std::string> keys;
    for (const auto &[key, record] : expected) {
      SCOPED_TRACE(key);
      const auto value = db.get(key);
      ASSERT_TRUE(value.has_value());
      EXPECT_EQ(*value, record.first);
      keys.push_back(key);
    }
    keys.push_back("b_deleted");
    keys.push_back("missing");
    EXPECT_FALSE(db.get("b_deleted").has_value());
    EXPECT_FALSE(db.get("missing").has_value());
    const auto batch = db.get_batch(keys);
    ASSERT_EQ(batch.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
      SCOPED_TRACE(keys[i]);
      EXPECT_EQ(batch[i].first, keys[i]);
      const auto found = expected.find(keys[i]);
      if (found == expected.end()) {
        EXPECT_FALSE(batch[i].second.has_value());
      } else {
        ASSERT_TRUE(batch[i].second.has_value());
        EXPECT_EQ(*batch[i].second, found->second.first);
      }
    }
    ASSERT_NO_FATAL_FAILURE(check_scan(db, 0, expected));
  }

  static void run_compaction(const std::filesystem::path &dir,
                              bool include_deeper_level) {
    ASSERT_NO_FATAL_FAILURE(configure(dir));
    const auto data_dir = dir / "database";
    const auto vlog_path = data_dir / "vlog.data";
    Expected first_snapshot;
    Expected second_snapshot;
    Expected latest;
    uint64_t first_id = 0;
    uint64_t second_id = 0;

    {
      LSM db(data_dir.string());
      auto first = db.begin_tran(IsolationLevel::READ_OP_COMMITTED);
      first_id = first->tranc_id_;
      const std::vector<std::pair<std::string, size_t>> records{
          {"a_versioned", 65}, {"b_deleted", 128}, {"c_twelve", 12},
          {"d_inline11", 11}, {"e_inline13", 13}, {"f_boundary", 64},
          {"g_large", 200}};
      for (const auto &[key, length] : records) {
        auto value = binary_value(length, 1);
        first->put(key, value);
        latest[key] = {std::move(value), first_id};
      }
      // 这些内联小值让合并结果足够大，确保目标层实际输出多张 SST；
      // 测试规模固定且很小，不依赖写入上万条数据才能触发合并。
      for (int i = 0; i < 32; ++i) {
        const auto key = "x_filler_" + std::to_string(100 + i);
        auto value = binary_value(63, static_cast<unsigned>(i));
        first->put(key, value);
        latest[key] = {std::move(value), first_id};
      }
      ASSERT_TRUE(first->commit());
      first.reset();
      first_snapshot = latest;
      db.flush_all();
      ASSERT_EQ(count_level(data_dir, 0), 1u);

      auto second = db.begin_tran(IsolationLevel::READ_OP_COMMITTED);
      second_id = second->tranc_id_;
      latest["a_versioned"] = {binary_value(128, 2), second_id};
      latest["c_twelve"] = {binary_value(12, 2), second_id};
      second->put("a_versioned", latest.at("a_versioned").first);
      second->put("c_twelve", latest.at("c_twelve").first);
      second->remove("b_deleted");
      latest.erase("b_deleted");
      ASSERT_TRUE(second->commit());
      second.reset();
      second_snapshot = latest;

      // RU 已写入存储的取消版本也参与后续合并，但查询必须继续跳过它。
      auto cancelled = db.begin_tran(IsolationLevel::READ_UNOP_COMMITTED);
      const uint64_t cancelled_id = cancelled->tranc_id_;
      cancelled->put("a_versioned", binary_value(200, 3));
      ASSERT_TRUE(cancelled->abort());
      cancelled.reset();
      db.flush_all();
      ASSERT_EQ(count_level(data_dir, 0), 2u);
      ASSERT_EQ(count_level(data_dir, 1), 0u);

      auto &config = const_cast<TomlConfig &>(TomlConfig::getInstance());
      config.modify_lsm_per_mem_size_limit(128);
      const int trigger_count = include_deeper_level ? 3 : 1;
      for (int i = 0; i < trigger_count; ++i) {
        const auto key = "zz_trigger_" + std::to_string(i);
        const auto value = binary_value(11, static_cast<unsigned>(i));
        db.put(key, value);
        latest[key] = {value, cancelled_id + 1 + static_cast<uint64_t>(i)};
        const auto before = std::filesystem::file_size(vlog_path);
        db.flush_all();
        const auto after = std::filesystem::file_size(vlog_path);
        // 这里只记录性能基线，不要求 VLog 必须增长：之后引用复用优化
        // 应仍能通过本测试。触发记录为 11B 内联值，本次刷盘无新分离值。
        std::cout << "WiscKey compaction flush " << i
                  << ": VLog before=" << before << ", after=" << after
                  << "; trigger value is inline (11B)" << std::endl;
        if (i == 0) {
          ASSERT_GT(count_level(data_dir, 1), 1u);
          ASSERT_EQ(count_level(data_dir, 2), 0u);
        }
      }

      if (include_deeper_level) {
        // L2 不为空只能来自 full_common_compact；多张输出同时覆盖
        // 更深层 gen_sst_from_iter 的 builder 重建与收尾。
        ASSERT_GT(count_level(data_dir, 2), 1u);
      }
      ASSERT_NO_FATAL_FAILURE(check_sst_modes(data_dir));
      ASSERT_NO_FATAL_FAILURE(check_latest(db, latest));
      ASSERT_NO_FATAL_FAILURE(check_scan(db, first_id, first_snapshot));
      ASSERT_NO_FATAL_FAILURE(check_scan(db, second_id, second_snapshot));
    }

    // 正常关闭后重新打开，重新加载 VLog 和 SST，不能借助旧内存表或缓存。
    {
      LSM reopened(data_dir.string());
      ASSERT_NO_FATAL_FAILURE(check_latest(reopened, latest));
      ASSERT_NO_FATAL_FAILURE(check_scan(reopened, first_id, first_snapshot));
      ASSERT_NO_FATAL_FAILURE(check_scan(reopened, second_id, second_snapshot));
      ASSERT_NO_FATAL_FAILURE(check_sst_modes(data_dir));
      ASSERT_GT(count_level(data_dir, include_deeper_level ? 2 : 1), 1u);
    }
  }

  static void run_child(const std::filesystem::path &dir, bool deeper) {
    int exit_code = 0;
    try {
      run_compaction(dir, deeper);
      exit_code = ::testing::Test::HasFailure() ? 1 : 0;
    } catch (const std::exception &error) {
      ADD_FAILURE() << error.what();
      exit_code = 90;
    } catch (...) {
      exit_code = 91;
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::_Exit(error ? 99 : exit_code);
  }
};

TEST_F(WiscKeyCompactionDeathTest, L0MergePreservesValuesVersionsAndReopens) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(run_child(dir_, false), ::testing::ExitedWithCode(0), "");
}

TEST_F(WiscKeyCompactionDeathTest, DeeperMergePreservesValuesVersionsAndReopens) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(run_child(dir_, true), ::testing::ExitedWithCode(0), "");
}

class WiscKeyClearDeathTest : public VLogOpenTest {
protected:
  static void check_records(
      LSM &db, const std::map<std::string, std::string> &expected,
      uint64_t old_version) {
    std::vector<std::string> keys;
    for (const auto &[key, value] : expected) {
      EXPECT_EQ(db.get(key), std::optional<std::string>(value));
      keys.push_back(key);
    }
    for (const auto &key : {"old_only", "old_memory", "deleted"}) {
      EXPECT_FALSE(db.get(key).has_value());
      keys.push_back(key);
    }
    const auto batch = db.get_batch(keys);
    ASSERT_EQ(batch.size(), keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
      EXPECT_EQ(batch[i].first, keys[i]);
      const auto found = expected.find(keys[i]);
      if (found == expected.end()) {
        EXPECT_FALSE(batch[i].second.has_value());
      } else {
        EXPECT_EQ(batch[i].second, std::optional<std::string>(found->second));
      }
    }
    // helper 返回时销毁迭代器，随后才允许 clear 或 flush 获取写锁。
    auto it = db.begin(0);
    for (const auto &[key, value] : expected) {
      ASSERT_FALSE(it.is_end());
      EXPECT_EQ(it->first, key);
      EXPECT_EQ(it->second, value);
      EXPECT_GT(it.get_cur_tranc_id(), old_version);
      ++it;
    }
    EXPECT_TRUE(it.is_end());
  }

  static void run_clear(const std::filesystem::path &dir, size_t threshold) {
    // 两种配置分别在新进程中初始化单例；清库约定为排他操作，
    // 测试不会让事务或迭代器存活到 clear 调用期间。
    const auto config_path = dir / "wisckey.toml";
    std::ofstream config(config_path);
    ASSERT_TRUE(config.is_open());
    config << R"toml([lsm.core]
LSM_TOL_MEM_SIZE_LIMIT = 67108864
LSM_PER_MEM_SIZE_LIMIT = 1048576
LSM_BLOCK_SIZE = 4096
LSM_SST_LEVEL_RATIO = 4
[lsm.cache]
LSM_BLOCK_CACHE_CAPACITY = 64
LSM_BLOCK_CACHE_K = 2
[bloom_filter]
BLOOM_FILTER_EXPECTED_SIZE = 4096
BLOOM_FILTER_EXPECTED_ERROR_RATE = 0.1
[lsm.wisckey]
WISCKEY_VALUE_THRESHOLD = )toml" << threshold << '\n';
    config.close();
    ASSERT_TRUE(config.good());
    ASSERT_EQ(TomlConfig::getInstance(config_path.string())
                  .getWisckeyValueThreshold(), threshold);

    const auto data_dir = dir / "database";
    const auto vlog_path = data_dir / "vlog.data";
    const std::map<std::string, std::string> replacements{
        {"inline", std::string("new\0inline", 10)},
        {"reused", std::string(200, 'N')},
        {"twelve", std::string("new\0twelve!!", 12)}};
    uint64_t old_version = 0;
    {
      LSM db(data_dir.string());
      db.put("reused", std::string(200, 'O'));
      db.put("twelve", std::string(12, 'O'));
      db.put("old_only", "persisted");
      db.flush_all();
      // 先实际查询，令旧块进入缓存；清库后复用 key 不能返回旧缓存值。
      EXPECT_EQ(db.get("reused"), std::optional<std::string>(std::string(200, 'O')));
      EXPECT_EQ(db.get("twelve"), std::optional<std::string>(std::string(12, 'O')));
      EXPECT_EQ(std::filesystem::file_size(vlog_path) > 0, threshold != 0);
      db.put("old_memory", "not flushed");
      {
        auto it = db.begin(0);
        for (; !it.is_end(); ++it) {
          old_version = std::max(old_version, it.get_cur_tranc_id());
        }
      }

      db.clear();
      ASSERT_TRUE(std::filesystem::is_regular_file(vlog_path));
      EXPECT_EQ(std::filesystem::file_size(vlog_path), 0u);
      for (const auto &entry : std::filesystem::directory_iterator(data_dir)) {
        EXPECT_FALSE(entry.path().filename().string().starts_with("sst_"));
      }
      EXPECT_FALSE(db.get("reused").has_value());
      EXPECT_FALSE(db.get("twelve").has_value());
      ASSERT_NO_FATAL_FAILURE(check_records(db, {}, old_version));

      // 空库再 clear 一次仍可正常使用，不依赖旧 VLog/SST 句柄。
      db.clear();
      EXPECT_EQ(std::filesystem::file_size(vlog_path), 0u);
      for (const auto &[key, value] : replacements) {
        db.put(key, value);
      }
      db.put("deleted", std::string(200, 'D'));
      db.remove("deleted");
      db.flush_all();
      EXPECT_EQ(std::filesystem::file_size(vlog_path) > 0, threshold != 0);

      size_t sst_count = 0;
      for (const auto &entry : std::filesystem::directory_iterator(data_dir)) {
        if (!entry.is_regular_file() ||
            !entry.path().filename().string().starts_with("sst_")) {
          continue;
        }
        // 只核对存储模式，不在第二个对象上读取或修改数据库记录。
        auto sst = SST::open(++sst_count,
                             FileObj::open(entry.path().string(), false),
                             make_cache());
        EXPECT_EQ(sst->is_wisckey(), threshold != 0);
      }
      EXPECT_GT(sst_count, 0u);
      ASSERT_NO_FATAL_FAILURE(check_records(db, replacements, old_version));
    }
    // 销毁原 DB 后从磁盘重开，验证新引用指向新 VLog，旧记录不会复活。
    {
      LSM reopened(data_dir.string());
      ASSERT_NO_FATAL_FAILURE(check_records(reopened, replacements, old_version));
    }
  }

  static void run_child(const std::filesystem::path &dir, size_t threshold) {
    int exit_code = 0;
    try {
      run_clear(dir, threshold);
      exit_code = ::testing::Test::HasFailure() ? 1 : 0;
    } catch (const std::exception &error) {
      ADD_FAILURE() << error.what();
      exit_code = 90;
    } catch (...) {
      exit_code = 91;
    }
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::_Exit(error ? 99 : exit_code);
  }
};

TEST_F(WiscKeyClearDeathTest, RecreatesVLogAndPersistsNewValues) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(run_child(dir_, 64), ::testing::ExitedWithCode(0), "");
}

TEST_F(WiscKeyClearDeathTest, DisabledModeRemainsUsableAfterClear) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(run_child(dir_, 0), ::testing::ExitedWithCode(0), "");
}

class WiscKeyCompactionBenchmarkDeathTest : public VLogOpenTest {
protected:
  static constexpr size_t record_count = 10000;
  static constexpr size_t value_bytes = 4096;
  static constexpr size_t source_sst_count = 4;

  static std::string key_for(size_t index) {
    // 固定宽度保证每个分片按序号写入时，key 也是升序。
    return "bench_" + std::to_string(100000000 + index).substr(1);
  }

  static std::string value_for(size_t index) {
    std::string value(value_bytes, static_cast<char>('A' + index % 23));
    const auto key = key_for(index);
    value.replace(0, key.size(), key);
    value[100] = '\0';
    return value;
  }

  static void configure(const std::filesystem::path &dir) {
    // 子进程首次访问配置前写入完整配置，不依赖其他测试改过的单例。
    const auto path = dir / "benchmark.toml";
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << R"toml([lsm.core]
LSM_TOL_MEM_SIZE_LIMIT = 67108864
LSM_PER_MEM_SIZE_LIMIT = 4194304
LSM_BLOCK_SIZE = 32768
LSM_SST_LEVEL_RATIO = 4
[lsm.cache]
LSM_BLOCK_CACHE_CAPACITY = 256
LSM_BLOCK_CACHE_K = 2
[bloom_filter]
BLOOM_FILTER_EXPECTED_SIZE = 65536
BLOOM_FILTER_EXPECTED_ERROR_RATE = 0.1
[lsm.wisckey]
WISCKEY_VALUE_THRESHOLD = 64
)toml";
    output.close();
    ASSERT_TRUE(output.good());
    const auto &config = TomlConfig::getInstance(path.string());
    ASSERT_EQ(config.getWisckeyValueThreshold(), 64u);
    std::cout << "A15_CONFIG {\"threshold\":" << config.getWisckeyValueThreshold()
              << ",\"total_mem_limit\":" << config.getLsmTolMemSizeLimit()
              << ",\"per_mem_limit\":" << config.getLsmPerMemSizeLimit()
              << ",\"block_size\":" << config.getLsmBlockSize()
              << ",\"level_ratio\":" << config.getLsmSstLevelRatio()
              << ",\"cache_capacity\":" << config.getLsmBlockCacheCapacity()
              << ",\"cache_k\":" << config.getLsmBlockCacheK()
              << ",\"bloom_expected_size\":" << config.getBloomFilterExpectedSize()
              << ",\"bloom_error_rate\":" << config.getBloomFilterExpectedErrorRate()
              << "}" << std::endl;
  }

  static void prepare_seed(const std::filesystem::path &seed) {
    ASSERT_TRUE(std::filesystem::create_directory(seed));
    auto vlog = VLog::open((seed / "vlog.data").string());
    auto cache = make_cache();
    for (size_t shard = 0; shard < source_sst_count; ++shard) {
      SSTBuilder builder(32768, true, vlog, 64);
      for (size_t index = shard; index < record_count; index += source_sst_count) {
        builder.add(key_for(index), value_for(index), 1);
      }
      const auto path = seed / ("sst_" + std::to_string(shard + 1) + ".0");
      auto sst = builder.build(shard + 1, path.string(), cache);
      ASSERT_TRUE(sst->is_wisckey());
    }
    // 函数返回时释放全部文件句柄。后续各轮只复制这份未合并的输入，
    // 不复用上一轮已经生成的新 SST 或已增长的 VLog。
  }

  static void measure_round(const std::filesystem::path &round_dir,
                             int round, std::uintmax_t seed_vlog_size) {
    auto engine = std::make_shared<LSMEngine>(round_dir.string());
    auto manager = std::make_shared<TranManager>(round_dir.string());
    manager->set_engine(engine);
    engine->set_tran_manager(manager);
    // 这是 Engine 合并基准，不是事务/WAL 性能测试。源记录用版本1，
    // 触发记录用版本2；管理器只提供查询所需的空回滚集合，不启动 WAL。
    ASSERT_EQ(engine->level_sst_ids[0].size(), source_sst_count);
    ASSERT_TRUE(engine->level_sst_ids[1].empty());
    const std::string trigger_key = "zz_trigger";
    const std::string trigger_value(11, 't');
    ASSERT_EQ(engine->put(trigger_key, trigger_value, 2), 0u);
    const auto before = engine->vlog_->tail_offset();
    ASSERT_EQ(before, seed_vlog_size);

    // 只计时触发合并的 flush：输入生成/复制、打开数据库、put 触发记录
    // 和结果校验均不计入。该值仍包含这一条内联记录的固定刷盘开销，
    // 因此不能称为纯 compact 函数耗时。
    const auto start = std::chrono::steady_clock::now();
    const auto flushed_id = engine->flush();
    const auto finish = std::chrono::steady_clock::now();
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
            .count();
    const auto after = engine->vlog_->tail_offset();

    ASSERT_EQ(flushed_id, 2u);
    ASSERT_EQ(engine->level_sst_ids[0].size(), 1u);
    ASSERT_FALSE(engine->level_sst_ids[1].empty());
    ASSERT_EQ(engine->memtable.get_total_size(), 0u);
    for (const auto &[id, sst] : engine->ssts) {
      ASSERT_TRUE(sst->is_wisckey());
    }
    for (size_t index = 0; index < record_count; ++index) {
      const auto key = key_for(index);
      SCOPED_TRACE(key);
      const auto record = engine->get(key, 0);
      ASSERT_TRUE(record.has_value());
      ASSERT_EQ(record->first, value_for(index));
      ASSERT_EQ(record->second, 1u);
    }
    const auto trigger = engine->get(trigger_key, 0);
    ASSERT_TRUE(trigger.has_value());
    ASSERT_EQ(trigger->first, trigger_value);
    ASSERT_EQ(trigger->second, 2u);

    // 文件增长统计不等于物理磁盘 I/O。只输出观测值，不要求一定增长，
    // 也不设置耗时门槛；优化后原测试仍应通过，再由相同负载比较数据。
    std::cout << "A15_SAMPLE {\"round\":" << round
              << ",\"warmup\":" << (round == 0 ? "true" : "false")
              << ",\"flush_ns\":" << elapsed_ns
              << ",\"vlog_before\":" << before
              << ",\"vlog_after\":" << after
              << ",\"records\":" << record_count
              << ",\"value_bytes\":" << value_bytes << "}" << std::endl;
  }

  static void run_benchmark(const std::filesystem::path &dir) {
    ASSERT_NO_FATAL_FAILURE(configure(dir));
    const auto seed = dir / "seed";
    ASSERT_NO_FATAL_FAILURE(prepare_seed(seed));
    const auto seed_vlog_size = std::filesystem::file_size(seed / "vlog.data");
    ASSERT_GT(seed_vlog_size, 0u);
    // 第0轮预热，随后5轮正式采样；不清系统缓存，不宣称测量冷缓存性能。
    for (int round = 0; round <= 5; ++round) {
      SCOPED_TRACE(round);
      const auto round_dir = dir / ("round_" + std::to_string(round));
      ASSERT_TRUE(std::filesystem::create_directory(round_dir));
      for (const auto &entry : std::filesystem::directory_iterator(seed)) {
        ASSERT_TRUE(std::filesystem::copy_file(
            entry.path(), round_dir / entry.path().filename()));
      }
      ASSERT_NO_FATAL_FAILURE(measure_round(round_dir, round, seed_vlog_size));
      // measure_round 已释放引擎和管理器，再删除本轮副本，避免积累大文件。
      ASSERT_GT(std::filesystem::remove_all(round_dir), 0u);
    }
  }
};

TEST_F(WiscKeyCompactionBenchmarkDeathTest, MeasuresReferenceCompactionCost) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        int exit_code = 0;
        try {
          run_benchmark(dir_);
          exit_code = ::testing::Test::HasFailure() ? 1 : 0;
        } catch (const std::exception &error) {
          ADD_FAILURE() << error.what();
          exit_code = 90;
        } catch (...) {
          exit_code = 91;
        }
        std::error_code error;
        std::filesystem::remove_all(dir_, error);
        std::_Exit(error ? 99 : exit_code);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

// 原有三个 Engine 用例使用独占目录，保留原数据规模与断言。
// 默认阈值 0 下验证普通模式；真正开启分离的刷盘／合并／重开
// 由上方显式配置非零阈值的集成用例覆盖。
class WiscKeyDisabled : public VLogOpenTest {};
class WiscKeyPersistence : public VLogOpenTest {};
class WiscKeyCompaction : public VLogOpenTest {};

TEST_F(WiscKeyDisabled, AllInlineFull) {
    const std::string dir = dir_.string();
    {
        const_cast<TomlConfig &>(TomlConfig::getInstance())
            .modify_lsm_tol_mem_size_limit(64 * 1024 * 1024);

        LSM lsm(dir);

        for (int i = 0; i < 50; ++i) {
            lsm.put("key" + std::to_string(i), make_value(100, 'v'));
        }
        lsm.flush();

        // All values must be readable
        for (int i = 0; i < 50; ++i) {
            auto res = lsm.get("key" + std::to_string(i));
            ASSERT_TRUE(res.has_value()) << "missing key" << i;
            EXPECT_EQ(res.value(), make_value(100, 'v'));
        }
    }
}

TEST_F(WiscKeyPutGet, LargeValueViaVLog) {
    std::string dir = dir_.string();
    std::string vlog_path = dir + "/vlog.data";

    {
        auto vlog = VLog::open(vlog_path);
        auto cache = make_cache();

        // Build a WiscKey SST directly
        std::string sst_path = dir + "/sst_wk.0";
        SSTBuilder builder(4096, true, vlog, 64);
        std::string large = make_value(200, 'Z');
        builder.add("mykey", large, 1);
        auto sst = builder.build(0, sst_path, cache);

        EXPECT_TRUE(sst->is_wisckey());
        auto it = sst->get("mykey", 1);
        EXPECT_FALSE(it.is_end());
        EXPECT_EQ((*it).second, large);
    }
}

TEST_F(WiscKeyPutRemove, TombstoneCorrect) {
    std::string dir = dir_.string();
    std::string vlog_path = dir + "/vlog.data";

    {
        auto vlog = VLog::open(vlog_path);
        auto cache = make_cache();

        std::string sst_path = dir + "/sst_wk2.0";
        SSTBuilder builder(4096, true, vlog, 64);
        std::string large = make_value(200, 'R');
        builder.add("delkey", large,  1);
        builder.add("delkey", "",     2); // tombstone at newer tranc_id
        auto sst = builder.build(0, sst_path, cache);

        // Iterate with keep_all_versions=true to see all entries
        auto it = sst->begin(2, true);
        EXPECT_FALSE(it.is_end());
        // First entry seen is tranc_id=1 (insertion order), resolved via vlog
        EXPECT_EQ((*it).second, large);
        ++it;
        // Second entry is the tombstone at tranc_id=2
        EXPECT_EQ((*it).second, "");
    }
}

TEST_F(WiscKeyPersistence, FlushAndReopen) {
    const std::string dir = dir_.string();

    const int N = 200;
    const size_t SMALL = 10;
    const size_t LARGE = 128;

    // Phase 1: write
    {
        const_cast<TomlConfig &>(TomlConfig::getInstance())
            .modify_lsm_tol_mem_size_limit(1); // force flush every put
        LSM lsm(dir);

        for (int i = 0; i < N; ++i) {
            std::string key = "key" + std::to_string(i);
            size_t sz = (i % 2 == 0) ? SMALL : LARGE;
            lsm.put(key, make_value(sz, 'a' + (i % 26)));
        }
        lsm.flush_all();
    }

    // Phase 2: reopen and verify
    {
        const_cast<TomlConfig &>(TomlConfig::getInstance())
            .modify_lsm_tol_mem_size_limit(64 * 1024 * 1024);
        LSM lsm(dir);
        for (int i = 0; i < N; ++i) {
            std::string key = "key" + std::to_string(i);
            size_t sz = (i % 2 == 0) ? SMALL : LARGE;
            auto res = lsm.get(key);
            ASSERT_TRUE(res.has_value()) << "missing " << key;
            EXPECT_EQ(res.value(), make_value(sz, 'a' + (i % 26))) << key;
        }
    }

}

TEST_F(WiscKeyCompaction, L0L1Compact) {
    const std::string dir = dir_.string();

    {
        // Small mem limit forces many flushes -> compaction
        const_cast<TomlConfig &>(TomlConfig::getInstance())
            .modify_lsm_tol_mem_size_limit(1);
        LSM lsm(dir);

        for (int i = 0; i < 100; ++i) {
            std::string key = "ckey" + std::to_string(i);
            lsm.put(key, make_value(80, 'c'));
        }
        lsm.flush_all();

        // All values still accessible after compaction
        for (int i = 0; i < 100; ++i) {
            std::string key = "ckey" + std::to_string(i);
            auto res = lsm.get(key);
            ASSERT_TRUE(res.has_value()) << "missing " << key;
            EXPECT_EQ(res.value(), make_value(80, 'c'));
        }
    }

}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
