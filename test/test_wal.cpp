#include "logger/logger.h"
#include "wal/record.h"
#include "lsm/engine.h"
#include "lsm/transaction.h"
#include "wal/wal.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

using namespace testing;
using namespace ::tiny_lsm;

// ! SERIALIZABLE（未完整实现）：本文件涉及该级别的用例只验证现有单键行为及 WAL。
// 日志和提交测试通过，不代表已验证范围幻读保护或完整可串行化。
// 场景：事务上下文创建时生成开始记录，后续操作依靠同一个 ID 关联。
// 验证记录的身份和类型，并确保没有携带属于 PUT/DELETE 的 key/value。
// 本组只检查 Record，不创建尚未完成的 WAL 对象。
TEST(RecordTest, CreateRecordInitializesTransactionStart) {
  const auto record = Record::createRecord(7);

  EXPECT_EQ(record.getTrancId(), uint64_t{7});
  EXPECT_EQ(record.getOperationType(), OperationType::OP_CREATE);
  EXPECT_TRUE(record.getKey().empty());
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：日志记录接收完整的 uint64_t ID，不能自行分配、递增或截断 ID。
// 覆盖 0、超过 32 位的值、最高位为 1 的值和 uint64_t 最大值。
TEST(RecordTest, CreateRecordPreservesFullWidthId) {
  const uint64_t ids[] = {0, uint64_t{1} << 32, 0xFEDCBA9876543210ULL,
                          std::numeric_limits<uint64_t>::max()};
  for (uint64_t id : ids) {
    SCOPED_TRACE(id);
    const auto record = Record::createRecord(id);
    EXPECT_EQ(record.getTrancId(), id);
  }
}

// 场景：提交路径生成 COMMIT 记录，日志通过事务 ID 将它关联到前面的操作。
// 验证它与 CREATE 的类型不同，且不会多带一份 key/value。
TEST(RecordTest, CommitRecordInitializesTransactionCommit) {
  const auto record = Record::commitRecord(7);

  EXPECT_EQ(record.getTrancId(), uint64_t{7});
  EXPECT_EQ(record.getOperationType(), OperationType::OP_COMMIT);
  EXPECT_TRUE(record.getKey().empty());
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：提交记录必须保留原事务 ID，尤其不能将大 ID 截断后关联到其他事务。
// 与开始记录采用相同边界，检查完整 uint64_t 范围内的代表值。
TEST(RecordTest, CommitRecordPreservesFullWidthId) {
  const uint64_t ids[] = {0, uint64_t{1} << 32, 0xFEDCBA9876543210ULL,
                          std::numeric_limits<uint64_t>::max()};
  for (uint64_t id : ids) {
    SCOPED_TRACE(id);
    const auto record = Record::commitRecord(id);
    EXPECT_EQ(record.getTrancId(), id);
  }
}

// 场景：构造取消某笔事务的日志记录，必须明确标记为 ROLLBACK。
// 只记录被取消事务的 ID，不在这条记录里携带 key/value 或旧值。
TEST(RecordTest, RollbackRecordInitializesTransactionRollback) {
  const auto record = Record::rollbackRecord(7);

  EXPECT_EQ(record.getTrancId(), uint64_t{7});
  EXPECT_EQ(record.getOperationType(), OperationType::OP_ROLLBACK);
  EXPECT_TRUE(record.getKey().empty());
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：回滚记录必须准确指向原事务，不能丢失 ID 的高位。
// 覆盖与 CREATE、COMMIT 相同的边界，验证三种状态记录一致保留 ID。
TEST(RecordTest, RollbackRecordPreservesFullWidthId) {
  const uint64_t ids[] = {0, uint64_t{1} << 32, 0xFEDCBA9876543210ULL,
                          std::numeric_limits<uint64_t>::max()};
  for (uint64_t id : ids) {
    SCOPED_TRACE(id);
    const auto record = Record::rollbackRecord(id);
    EXPECT_EQ(record.getTrancId(), id);
  }
}

// 场景：调用方创建写入记录后复用自己的字符串缓冲区。
// 记录必须保存独立副本，同时保留 PUT 类型和完整事务 ID。
TEST(RecordTest, PutRecordPreservesFieldsAndOwnsStrings) {
  std::string key = "original-key";
  std::string value = "original-value";
  const auto id = std::numeric_limits<uint64_t>::max();
  const auto record = Record::putRecord(id, key, value);

  key.assign("changed-key");
  value.clear();

  EXPECT_EQ(record.getTrancId(), id);
  EXPECT_EQ(record.getOperationType(), OperationType::OP_PUT);
  EXPECT_EQ(record.getKey(), "original-key");
  EXPECT_EQ(record.getValue(), "original-value");
}

// 场景：Record 层接收零长度字符串，完整保留传入的操作内容。
// 即使 value 为空，记录类型仍为 PUT；公开 API 的输入限制由上层决定。
TEST(RecordTest, PutRecordPreservesEmptyStrings) {
  const std::pair<std::string, std::string> cases[] = {
      {"", ""}, {"", "value"}, {"key", ""}};
  for (const auto &[key, value] : cases) {
    SCOPED_TRACE("key size=" + std::to_string(key.size()) +
                 ", value size=" + std::to_string(value.size()));
    const auto record = Record::putRecord(0, key, value);
    EXPECT_EQ(record.getTrancId(), uint64_t{0});
    EXPECT_EQ(record.getOperationType(), OperationType::OP_PUT);
    EXPECT_EQ(record.getKey(), key);
    EXPECT_EQ(record.getValue(), value);
  }
}

// 场景：key/value 是带有空字节的二进制内容，不能按 C 字符串提前截断。
TEST(RecordTest, PutRecordPreservesEmbeddedNulls) {
  const std::string key("k\0x", 3);
  const std::string value("\0v\0z\0", 5);
  const auto record = Record::putRecord(7, key, value);

  EXPECT_EQ(record.getKey(), key);
  EXPECT_EQ(record.getValue(), value);
  EXPECT_EQ(record.getOperationType(), OperationType::OP_PUT);
}

// 场景：PUT 的固定字段占 15B，key/value 合计 65520B 时总长恰好为 65535B。
// 覆盖长度集中在一侧以及分摊到两侧，合法边界必须完整保留。
TEST(RecordTest, PutRecordAcceptsLargestRecord) {
  const std::pair<std::size_t, std::size_t> sizes[] = {
      {1, 65519}, {65519, 1}, {0, 65520}, {65520, 0}, {32760, 32760}};
  for (const auto &[key_size, value_size] : sizes) {
    SCOPED_TRACE("key size=" + std::to_string(key_size) +
                 ", value size=" + std::to_string(value_size));
    const std::string key(key_size, 'k');
    const std::string value(value_size, 'v');
    const auto record = Record::putRecord(7, key, value);
    EXPECT_EQ(record.getKey(), key);
    EXPECT_EQ(record.getValue(), value);
    EXPECT_EQ(record.getOperationType(), OperationType::OP_PUT);
  }
}

// 场景：每个字符串单独都能用 uint16_t 表示，但整条记录已经放不下。
// 检查恰好超出 1B、两字段合计超限，以及字段本身达到 65535B 的情况。
TEST(RecordTest, PutRecordRejectsTotalLengthOverflow) {
  const std::pair<std::size_t, std::size_t> sizes[] = {
      {1, 65520}, {65520, 1}, {40000, 40000}, {65535, 0}, {0, 65535}};
  for (const auto &[key_size, value_size] : sizes) {
    SCOPED_TRACE("key size=" + std::to_string(key_size) +
                 ", value size=" + std::to_string(value_size));
    const std::string key(key_size, 'k');
    const std::string value(value_size, 'v');
    EXPECT_THROW(Record::putRecord(7, key, value), std::length_error);
  }
}

// 场景：单个字段达到 65536B，长度字段已经无法表示，必须明确拒绝。
TEST(RecordTest, PutRecordRejectsFieldLengthOverflow) {
  const std::string oversized(65536, 'x');
  EXPECT_THROW(Record::putRecord(7, oversized, "v"), std::length_error);
  EXPECT_THROW(Record::putRecord(7, "k", oversized), std::length_error);
}

// 场景：生成删除记录后，调用方复用 key 缓冲区。
// 记录必须保留原 key、完整事务 ID 和 DELETE 类型，value 保持为空。
TEST(RecordTest, DeleteRecordPreservesFieldsAndOwnsKey) {
  std::string key = "original-key";
  const auto id = std::numeric_limits<uint64_t>::max();
  const auto record = Record::deleteRecord(id, key);
  key.assign("changed-key");

  EXPECT_EQ(record.getTrancId(), id);
  EXPECT_EQ(record.getOperationType(), OperationType::OP_DELETE);
  EXPECT_EQ(record.getKey(), "original-key");
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：Record 层按长度保存空 key，不在这里添加公开 API 的输入限制。
TEST(RecordTest, DeleteRecordPreservesEmptyKey) {
  const auto record = Record::deleteRecord(0, "");

  EXPECT_EQ(record.getTrancId(), uint64_t{0});
  EXPECT_EQ(record.getOperationType(), OperationType::OP_DELETE);
  EXPECT_TRUE(record.getKey().empty());
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：待删除的 key 包含空字节，必须保存完整内容才能准确定位目标。
TEST(RecordTest, DeleteRecordPreservesEmbeddedNulls) {
  const std::string key("\0k\0z\0", 5);
  const auto record = Record::deleteRecord(7, key);

  EXPECT_EQ(record.getKey(), key);
  EXPECT_EQ(record.getOperationType(), OperationType::OP_DELETE);
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：DELETE 固定字段占 13B，65522B 的 key 使记录恰好达到 65535B。
// 验证合法上限能够创建，key 没有被截断或误当成 PUT 的长度限制。
TEST(RecordTest, DeleteRecordAcceptsLargestRecord) {
  const std::string key(65522, 'k');
  const auto record = Record::deleteRecord(7, key);

  EXPECT_EQ(record.getKey(), key);
  EXPECT_EQ(record.getOperationType(), OperationType::OP_DELETE);
  EXPECT_TRUE(record.getValue().empty());
}

// 场景：key 长度字段仍能表示，但加上 13B 固定字段后整条记录超限。
TEST(RecordTest, DeleteRecordRejectsTotalLengthOverflow) {
  for (std::size_t key_size : {std::size_t{65523}, std::size_t{65535}}) {
    SCOPED_TRACE(key_size);
    const std::string key(key_size, 'k');
    EXPECT_THROW(Record::deleteRecord(7, key), std::length_error);
  }
}

// 场景：key 本身达到 65536B，无法用 uint16_t 长度字段表示。
TEST(RecordTest, DeleteRecordRejectsFieldLengthOverflow) {
  const std::string key(65536, 'k');
  EXPECT_THROW(Record::deleteRecord(7, key), std::length_error);
}

// 场景：三类状态记录只有 11B 公共头，类型编号分别为 0、1、2。
// ID 的八个字节互不相同，直接对照低位在前的固定样例，不依赖 decode。
TEST(RecordTest, EncodeStateRecordsMatchKnownBytes) {
  constexpr uint64_t id = 0x0102030405060708ULL;
  std::vector<uint8_t> expected = {11, 0, 8, 7, 6, 5, 4, 3, 2, 1, 0};
  EXPECT_EQ(Record::createRecord(id).encode(), expected);
  expected.back() = 1;
  EXPECT_EQ(Record::commitRecord(id).encode(), expected);
  expected.back() = 2;
  EXPECT_EQ(Record::rollbackRecord(id).encode(), expected);
}

// 场景：PUT(7, "a", "bc") 总长为 18B，按格式排列长度、ID、类型和字符串。
TEST(RecordTest, EncodePutMatchesKnownBytes) {
  const std::vector<uint8_t> expected = {
      18, 0,                      // 整条记录长度
      7, 0, 0, 0, 0, 0, 0, 0,   // 完整 8B 事务 ID
      3,                          // PUT
      1, 0, 97,                   // key 长度 1，内容 a
      2, 0, 98, 99};              // value 长度 2，内容 bc
  EXPECT_EQ(Record::putRecord(7, "a", "bc").encode(), expected);
}

// 场景：DELETE(7, "a") 总长为 14B，不能额外编码 value 的长度字段。
TEST(RecordTest, EncodeDeleteMatchesKnownBytes) {
  const std::vector<uint8_t> expected = {
      14, 0, 7, 0, 0, 0, 0, 0, 0, 0, 4, 1, 0, 97};
  EXPECT_EQ(Record::deleteRecord(7, "a").encode(), expected);
}

// 场景：空字符串仍需写入值为 0 的长度字段，PUT 与 DELETE 的布局不同。
TEST(RecordTest, EncodePreservesZeroLengthFields) {
  const std::vector<uint8_t> empty_put = {
      15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0};
  const std::vector<uint8_t> empty_delete = {
      13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0};
  EXPECT_EQ(Record::putRecord(0, "", "").encode(), empty_put);
  EXPECT_EQ(Record::deleteRecord(0, "").encode(), empty_delete);
}

// 场景：char 可能有符号，但转成 uint8_t 后应保存原始字节内容。
// 同时检查空字节、最高位为 1 的字节，以及 uint64_t 最大 ID。
TEST(RecordTest, EncodePreservesBinaryBytes) {
  const std::string key("\0\x80\xff", 3);
  const std::string value("\xff\0\x81", 3);
  const std::vector<uint8_t> expected = {
      21, 0,
      255, 255, 255, 255, 255, 255, 255, 255,
      3,
      3, 0, 0, 128, 255,
      3, 0, 255, 0, 129};
  EXPECT_EQ(Record::putRecord(std::numeric_limits<uint64_t>::max(), key, value)
                .encode(),
            expected);
}

// 场景：长度超过 255 时需要第二个字节，不能只写最低 8 位。
// key=258B、value=257B、总长=530B，对应长度字节分别为 [2,1]、[1,1]、[18,2]。
TEST(RecordTest, EncodeLengthsUseTwoLittleEndianBytes) {
  const std::string key(258, 'k');
  const std::string value(257, 'v');
  std::vector<uint8_t> expected = {
      18, 2, 7, 0, 0, 0, 0, 0, 0, 0, 3, 2, 1};
  expected.insert(expected.end(), 258, 107);
  expected.push_back(1);
  expected.push_back(1);
  expected.insert(expected.end(), 257, 118);
  EXPECT_EQ(Record::putRecord(7, key, value).encode(), expected);
}

// 场景：合法最大 PUT、DELETE 实际编码后都必须恰好为 65535B。
// 对照固定头部和完整内容，验证工厂计算的总长以及每个字符串长度。
TEST(RecordTest, EncodeLargestRecordsHaveExactLength) {
  const auto put = Record::putRecord(7, "k", std::string(65519, 'v')).encode();
  std::vector<uint8_t> expected_put = {
      255, 255, 7, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0, 107, 239, 255};
  expected_put.insert(expected_put.end(), 65519, 118);
  ASSERT_EQ(put.size(), std::size_t{65535});
  EXPECT_EQ(put, expected_put);

  const auto deletion = Record::deleteRecord(7, std::string(65522, 'k')).encode();
  std::vector<uint8_t> expected_delete = {
      255, 255, 7, 0, 0, 0, 0, 0, 0, 0, 4, 242, 255};
  expected_delete.insert(expected_delete.end(), 65522, 107);
  ASSERT_EQ(deletion.size(), std::size_t{65535});
  EXPECT_EQ(deletion, expected_delete);
}

// 场景：没有日志字节时，没有任何记录需要还原。
TEST(RecordTest, DecodeEmptyInputReturnsNoRecords) {
  EXPECT_TRUE(Record::decode({}).empty());
}

// 场景：直接读取约定格式的状态记录，独立验证类型、完整 ID 和空负载。
TEST(RecordTest, DecodeKnownStateRecords) {
  const std::pair<uint8_t, OperationType> cases[] = {
      {0, OperationType::OP_CREATE}, {1, OperationType::OP_COMMIT},
      {2, OperationType::OP_ROLLBACK}};
  for (const auto &[type_byte, type] : cases) {
    SCOPED_TRACE(static_cast<unsigned>(type_byte));
    const std::vector<uint8_t> data = {11, 0, 8, 7, 6, 5, 4, 3, 2, 1, type_byte};
    const auto records = Record::decode(data);
    ASSERT_EQ(records.size(), std::size_t{1});
    EXPECT_EQ(records[0].getTrancId(), uint64_t{0x0102030405060708ULL});
    EXPECT_EQ(records[0].getOperationType(), type);
    EXPECT_TRUE(records[0].getKey().empty());
    EXPECT_TRUE(records[0].getValue().empty());
  }
}

// 场景：两条不同长度的 PUT、DELETE 连续存放，逐字段核对其还原结果。
TEST(RecordTest, DecodeKnownPutAndDeleteBytes) {
  const std::vector<uint8_t> data = {
      18, 0, 7, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0, 97, 2, 0, 98, 99,
      14, 0, 8, 0, 0, 0, 0, 0, 0, 0, 4, 1, 0, 97};
  const auto records = Record::decode(data);
  ASSERT_EQ(records.size(), std::size_t{2});
  EXPECT_EQ(records[0].getTrancId(), uint64_t{7});
  EXPECT_EQ(records[0].getOperationType(), OperationType::OP_PUT);
  EXPECT_EQ(records[0].getKey(), "a");
  EXPECT_EQ(records[0].getValue(), "bc");
  EXPECT_EQ(records[1].getTrancId(), uint64_t{8});
  EXPECT_EQ(records[1].getOperationType(), OperationType::OP_DELETE);
  EXPECT_EQ(records[1].getKey(), "a");
  EXPECT_TRUE(records[1].getValue().empty());
}

// 场景：不同事务交错，重复 key、状态记录和零长度字段必须原样保留顺序。
// 编解码负责保存记录，不在这里合并操作或丢弃回滚事务。
TEST(RecordTest, DecodeMixedStreamPreservesOrderAndDuplicates) {
  const std::vector<Record> expected = {
      Record::createRecord(7), Record::putRecord(7, "a", "old"),
      Record::putRecord(9, "a", "new"), Record::deleteRecord(7, "a"),
      Record::commitRecord(9), Record::rollbackRecord(7),
      Record::putRecord(0, "", ""), Record::deleteRecord(0, ""),
      Record::putRecord(1, "", "value"), Record::putRecord(2, "key", "")};
  std::vector<uint8_t> data;
  for (const auto &record : expected) {
    const auto bytes = record.encode();
    data.insert(data.end(), bytes.begin(), bytes.end());
  }
  const auto records = Record::decode(data);
  ASSERT_EQ(records.size(), expected.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(records[i].getTrancId(), expected[i].getTrancId());
    EXPECT_EQ(records[i].getOperationType(), expected[i].getOperationType());
    EXPECT_EQ(records[i].getKey(), expected[i].getKey());
    EXPECT_EQ(records[i].getValue(), expected[i].getValue());
    EXPECT_EQ(records[i].encode(), expected[i].encode());
  }
}

// 场景：解码最大 ID 和包含空字节、高位字节的内容，结果拥有独立字符串。
TEST(RecordTest, DecodeBinaryBytesOwnTheirStorage) {
  std::vector<uint8_t> data = {
      21, 0, 255, 255, 255, 255, 255, 255, 255, 255,
      3, 3, 0, 0, 128, 255, 3, 0, 255, 0, 129};
  const auto records = Record::decode(data);
  ASSERT_EQ(records.size(), std::size_t{1});
  data.assign(data.size(), 0);
  EXPECT_EQ(records[0].getTrancId(), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(records[0].getOperationType(), OperationType::OP_PUT);
  EXPECT_EQ(records[0].getKey(), std::string("\0\x80\xff", 3));
  EXPECT_EQ(records[0].getValue(), std::string("\xff\0\x81", 3));
}

// 场景：每条记录最多 65535B，但多条记录拼接后的输入可以超过这个长度。
// 同时验证字符串长度高字节和后续记录的绝对边界。
TEST(RecordTest, DecodeLargestRecordsRoundTrip) {
  const std::vector<Record> expected = {
      Record::putRecord(std::numeric_limits<uint64_t>::max(), "k",
                        std::string(65519, 'v')),
      Record::deleteRecord(7, std::string(65522, 'k'))};
  std::vector<uint8_t> data;
  for (const auto &record : expected) {
    const auto bytes = record.encode();
    data.insert(data.end(), bytes.begin(), bytes.end());
  }
  ASSERT_EQ(data.size(), std::size_t{131070});
  const auto records = Record::decode(data);
  ASSERT_EQ(records.size(), expected.size());
  for (std::size_t i = 0; i < records.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(records[i].getTrancId(), expected[i].getTrancId());
    EXPECT_EQ(records[i].getOperationType(), expected[i].getOperationType());
    EXPECT_EQ(records[i].getKey(), expected[i].getKey());
    EXPECT_EQ(records[i].getValue(), expected[i].getValue());
    EXPECT_EQ(records[i].encode(), expected[i].encode());
  }
}

// 场景：首条记录在任意位置被截断，非空残片都必须明确报错。
TEST(RecordTest, DecodeRejectsTruncatedFirstRecord) {
  const std::vector<uint8_t> full = {
      18, 0, 7, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0, 97, 2, 0, 98, 99};
  for (std::size_t size = 1; size < full.size(); ++size) {
    SCOPED_TRACE(size);
    const std::vector<uint8_t> truncated(full.begin(), full.begin() + size);
    EXPECT_THROW(Record::decode(truncated), std::runtime_error);
  }
}

// 场景：前一条完整、后一条截断，长度检查必须使用当前位置之后的剩余数据。
TEST(RecordTest, DecodeRejectsTruncatedLaterRecord) {
  const auto prefix = Record::createRecord(6).encode();
  const std::vector<uint8_t> full = {
      18, 0, 7, 0, 0, 0, 0, 0, 0, 0, 3, 1, 0, 97, 2, 0, 98, 99};
  for (std::size_t size = 1; size < full.size(); ++size) {
    SCOPED_TRACE(size);
    auto data = prefix;
    data.insert(data.end(), full.begin(), full.begin() + size);
    EXPECT_THROW(Record::decode(data), std::runtime_error);
  }
}

// 场景：总长为 0 或不足公共头大小时，不能接受，也不能让解析停滞。
TEST(RecordTest, DecodeRejectsInvalidRecordLengths) {
  for (uint8_t length = 0; length < 11; ++length) {
    SCOPED_TRACE(static_cast<unsigned>(length));
    const std::vector<uint8_t> data = {length, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_THROW(Record::decode(data), std::runtime_error);
  }
}

// 场景：类型字段出现协议之外的编号，不能默认为无负载的状态记录。
TEST(RecordTest, DecodeRejectsUnknownOperations) {
  for (uint8_t type : {uint8_t{5}, uint8_t{255}}) {
    SCOPED_TRACE(static_cast<unsigned>(type));
    const std::vector<uint8_t> data = {11, 0, 7, 0, 0, 0, 0, 0, 0, 0, type};
    EXPECT_THROW(Record::decode(data), std::runtime_error);
  }
}

// 场景：本条记录的 key_len/value_len 缺失或只有 1B。
// 后面即使还有完整记录，也不能借用下一条的字节来补齐长度字段。
TEST(RecordTest, DecodeRejectsTruncatedStringLengthFields) {
  const std::vector<std::vector<uint8_t>> malformed = {
      {11, 0, 7, 0, 0, 0, 0, 0, 0, 0, 4},
      {12, 0, 7, 0, 0, 0, 0, 0, 0, 0, 4, 1},
      {13, 0, 7, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0},
      {14, 0, 7, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 1}};
  const auto tail = Record::createRecord(9).encode();
  for (const auto &record : malformed) {
    SCOPED_TRACE(record.size());
    auto data = record;
    data.insert(data.end(), tail.begin(), tail.end());
    EXPECT_THROW(Record::decode(data), std::runtime_error);
  }
}

// 场景：字符串声明 2B，本条内却只剩 1B；下一条记录的内容不能被越界读入。
TEST(RecordTest, DecodeRejectsStringContentOutsideRecord) {
  const std::vector<std::vector<uint8_t>> malformed = {
      {14, 0, 7, 0, 0, 0, 0, 0, 0, 0, 4, 2, 0, 97},
      {16, 0, 7, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 2, 0, 97}};
  const auto tail = Record::createRecord(9).encode();
  for (const auto &record : malformed) {
    SCOPED_TRACE(record.size());
    auto data = record;
    data.insert(data.end(), tail.begin(), tail.end());
    EXPECT_THROW(Record::decode(data), std::runtime_error);
  }
}

// 场景：把首条记录声明的长度故意扩大，包住后一条完整记录。
// 按类型读完字段后若仍有剩余字节，应拒绝长度矛盾，不能把剩余部分当新记录。
TEST(RecordTest, DecodeRejectsUnconsumedBytesInsideRecord) {
  const std::vector<Record> first_records = {
      Record::createRecord(7), Record::commitRecord(7), Record::rollbackRecord(7),
      Record::putRecord(7, "a", "bc"), Record::deleteRecord(7, "a")};
  const auto tail = Record::createRecord(9).encode();
  for (const auto &record : first_records) {
    SCOPED_TRACE(static_cast<int>(record.getOperationType()));
    auto data = record.encode();
    data.insert(data.end(), tail.begin(), tail.end());
    // 本测试的总长均小于 256，首条长度字段写入整个数组的长度。
    data[0] = static_cast<uint8_t>(data.size());
    data[1] = 0;
    EXPECT_THROW(Record::decode(data), std::runtime_error);
  }
}

class MockWAL : public WAL {
public:
  MockWAL(const std::string &log_path, size_t buffer_size,
          uint64_t checkpoint_tranc_id)
      : WAL(log_path, buffer_size, checkpoint_tranc_id, 1, 100) {}

  MOCK_METHOD(void, cleaner, ());
  MOCK_METHOD(void, cleanWALFile, ());
  MOCK_METHOD(void, reset_file, ());

  FileObj *get_log_file() { return &log_file_; }

  std::vector<Record> &get_log_buffer() { return log_buffer_; }

  size_t get_file_size_limit() { return file_size_limit_; }

  void set_checkpoint_tranc_id(uint64_t checkpoint_tranc_id) {
    checkpoint_tranc_id_ = checkpoint_tranc_id;
  }

  uint64_t get_checkpoint_tranc_id() { return checkpoint_tranc_id_; }
};

namespace {

// 只用于测试：关闭后台清理，观察缓冲，并通过关闭文件制造真实 I/O 失败。
// 不替换 WAL 的非虚函数，也不修改业务类的公开接口。
class WALBufferProbe : public WAL {
public:
  WALBufferProbe(const std::string &dir, size_t buffer_size,
                 uint64_t file_size_limit = 0)
      : WAL(dir, buffer_size, 0, 0, file_size_limit) {}

  const std::vector<Record> &buffered_records() const { return log_buffer_; }
  const std::string &active_path() const { return active_log_path_; }
  std::vector<Record> active_file_records() {
    return Record::decode(log_file_.read_to_slice(0, log_file_.size()));
  }
  void close_file_for_test() { log_file_.close(); }
  void reopen_file_for_test() {
    log_file_ = FileObj::open(active_log_path_, false);
  }
};

std::vector<Record> read_wal_records(const std::filesystem::path &path) {
  auto file = FileObj::open(path.string());
  return Record::decode(file.read_to_slice(0, file.size()));
}

void write_wal_records(const std::filesystem::path &path,
                       const std::vector<Record> &records) {
  std::vector<uint8_t> bytes;
  for (const auto &record : records) {
    const auto encoded = record.encode();
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
  }
  FileObj::create_and_write(path.string(), std::move(bytes));
}

// 等待真实清理线程产生可观察结果，不直接调用或替换私有清理函数。
// 使用截止时间，允许调度延迟，不依赖固定 sleep 后恰好已经完成清理。
template <typename Predicate>
bool wait_for_wal_condition(Predicate condition) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (std::chrono::steady_clock::now() < deadline);
  return condition();
}

} // namespace

class WALTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 创建测试目录
    if (!std::filesystem::exists(test_dir)) {
      std::filesystem::create_directory(test_dir);
    } else {
      // 清空测试目录
      for (const auto &entry : std::filesystem::directory_iterator(test_dir)) {
        std::filesystem::remove_all(entry.path());
      }
    }
  }

  void TearDown() override {
    // 清理测试文件
    if (std::filesystem::exists(test_dir)) {
      std::filesystem::remove_all(test_dir);
    }
  }

  std::string test_dir = "test_wal_dir";
};

// 目的：新库的日志目录尚不存在时，构造函数创建目录和空的 wal.0。
// 本组构造测试将清理间隔设为 0，单独验证文件初始化路径。
TEST_F(WALTest, ConstructorCreatesNestedDirectory) {
  const auto log_dir = std::filesystem::path(test_dir) / "nested" / "logs";
  {
    WAL wal(log_dir.string(), 8, 0, 0, 4096);
    ASSERT_TRUE(std::filesystem::is_regular_file(log_dir / "wal.0"));
    EXPECT_EQ(std::filesystem::file_size(log_dir / "wal.0"), 0u);
  }
  EXPECT_TRUE(std::filesystem::is_regular_file(log_dir / "wal.0"));
}

// 目的：目录里已有日志时，从最大的数字序号继续，旧文件内容保持原样。
// wal.9 的字典序大于 wal.3000000000，但数字序号更小；大序号也超过 int 范围。
// wal.backup、wal.4000000000.bak 和 wal. 都不是合法序号，不应被误解析。
TEST_F(WALTest, ConstructorPreservesFilesAndUsesNumericSequence) {
  const std::vector<std::string> names = {
      "wal.9", "wal.10", "wal.3000000000", "wal.backup",
      "wal.4000000000.bak", "wal.", "unrelated"};
  const std::vector<uint8_t> original = {17, 0, 34, 255};
  for (const auto &name : names) {
    FileObj::create_and_write(test_dir + "/" + name, original);
  }

  {
    WAL wal(test_dir, 8, 0, 0, 4096);
    const auto new_path = std::filesystem::path(test_dir) / "wal.3000000001";
    ASSERT_TRUE(std::filesystem::is_regular_file(new_path));
    EXPECT_EQ(std::filesystem::file_size(new_path), 0u);
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) /
                                         "wal.0"));
  }

  for (const auto &name : names) {
    SCOPED_TRACE(name);
    auto file = FileObj::open(test_dir + "/" + name);
    EXPECT_EQ(file.read_to_slice(0, file.size()), original);
  }
}

// 目的：传入的目录路径实际是普通文件时，应明确失败，不能破坏原文件。
TEST_F(WALTest, ConstructorRejectsFileUsedAsDirectory) {
  const auto path = test_dir + "/not_a_directory";
  const std::vector<uint8_t> original = {1, 2, 3};
  FileObj::create_and_write(path, original);

  EXPECT_THROW((WAL(path, 8, 0, 0, 4096)),
               std::filesystem::filesystem_error);

  auto file = FileObj::open(path);
  EXPECT_EQ(file.read_to_slice(0, file.size()), original);
}

// 目的：目录可以访问，但目标日志路径已被子目录占用，文件打开必须报告失败。
// 这个场景直接覆盖 FileObj::open 的失败传播，不依赖运行用户的权限设置。
TEST_F(WALTest, ConstructorReportsLogOpenFailure) {
  const auto occupied = std::filesystem::path(test_dir) / "wal.0";
  std::filesystem::create_directory(occupied);

  EXPECT_THROW((WAL(test_dir, 8, 0, 0, 4096)), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_directory(occupied));
}

// 目的：已有序号达到 uint64_t 上限时，拒绝继续创建，避免加一回绕覆盖 wal.0。
TEST_F(WALTest, ConstructorRejectsExhaustedSequence) {
  const std::vector<uint8_t> original = {4, 5, 6};
  const auto last_path = test_dir + "/wal." +
                         std::to_string(std::numeric_limits<uint64_t>::max());
  FileObj::create_and_write(last_path, original);

  EXPECT_THROW((WAL(test_dir, 8, 0, 0, 4096)), std::overflow_error);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) /
                                       "wal.0"));
  auto file = FileObj::open(last_path);
  EXPECT_EQ(file.read_to_slice(0, file.size()), original);
}

// 目的：构造函数实际创建线程后，离开作用域能够正常完成，不能因漏掉 join 而终止。
// 当前 cleaner 仍为空桩，因此这里只验证线程对象的回收；等待唤醒在阶段③验收。
TEST_F(WALTest, DestructorJoinsStartedCleaner) {
  const auto log_path = std::filesystem::path(test_dir) / "wal.0";
  {
    WAL wal(test_dir, 8, 0, 60, 4096);
    ASSERT_TRUE(std::filesystem::is_regular_file(log_path));
  }

  auto file = FileObj::open(log_path.string());
  EXPECT_EQ(file.size(), 0u);
}

// 目的：调用方抛出异常时，栈上 WAL 仍应回收已创建的线程，原异常正常到达调用方。
// 不把这个场景当作“刷盘异常处理已验证”；flush 的 I/O 失败测试属于阶段②。
TEST_F(WALTest, DestructorHandlesExceptionUnwinding) {
  EXPECT_THROW(
      {
        WAL wal(test_dir, 8, 0, 60, 4096);
        throw std::logic_error("caller failure");
      },
      std::logic_error);

  EXPECT_TRUE(std::filesystem::is_regular_file(
      std::filesystem::path(test_dir) / "wal.0"));
}

// 目的：缓冲阈值按记录条数计算；大 value 不应让未满的记录缓冲提前写出。
// 第三条记录刚好达到阈值时，前面缓冲的记录与新记录按顺序完整写入。
TEST_F(WALTest, LogBuffersUntilRecordCountThreshold) {
  WALBufferProbe wal(test_dir, 3, 4096);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  const std::vector<Record> pending = {
      Record::createRecord(7), Record::putRecord(7, "key", std::string(1024, 'v'))};

  wal.log(pending);
  EXPECT_EQ(wal.buffered_records(), pending);
  EXPECT_EQ(std::filesystem::file_size(path), 0u);

  const auto commit = Record::commitRecord(7);
  wal.log({commit});
  EXPECT_TRUE(wal.buffered_records().empty());
  auto expected = pending;
  expected.push_back(commit);
  EXPECT_EQ(read_wal_records(path), expected);
}

// 目的：强制写入必须包含旧缓冲和新批次，保留不同事务、重复 key 的输入顺序。
// 混合五类记录及内嵌空字节，验证真实文件内容可以被完整解码。
TEST_F(WALTest, LogForceWritesBufferedAndNewRecordsInOrder) {
  WALBufferProbe wal(test_dir, 128);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  const std::vector<Record> pending = {
      Record::createRecord(7), Record::putRecord(7, "key", "old")};
  const std::vector<Record> incoming = {
      Record::createRecord(8), Record::deleteRecord(8, "key"),
      Record::rollbackRecord(8),
      Record::putRecord(7, "key", std::string("new\0value", 9)),
      Record::commitRecord(7)};

  wal.log(pending);
  wal.log(incoming, true);

  auto expected = pending;
  expected.insert(expected.end(), incoming.begin(), incoming.end());
  EXPECT_EQ(read_wal_records(path), expected);
  EXPECT_TRUE(wal.buffered_records().empty());
}

// 目的：没有新增记录时也能强制写出旧缓冲；成功后重复调用不能重复追加。
// 这里只调用 log({}, true)，显式 flush 的接通在下一函数验收。
TEST_F(WALTest, LogEmptyForceDrainsBufferWithoutDuplicates) {
  WALBufferProbe wal(test_dir, 128);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  const std::vector<Record> pending = {
      Record::createRecord(11), Record::putRecord(11, "key", "value"),
      Record::commitRecord(11)};

  wal.log(pending);
  ASSERT_EQ(std::filesystem::file_size(path), 0u);
  wal.log({}, true);
  EXPECT_TRUE(wal.buffered_records().empty());
  EXPECT_EQ(read_wal_records(path), pending);

  const auto written_size = std::filesystem::file_size(path);
  wal.log({}, true);
  wal.log({});
  EXPECT_EQ(std::filesystem::file_size(path), written_size);
  EXPECT_EQ(read_wal_records(path), pending);
}

// 目的：缓冲阈值为 0 时，每次非空调用立即写入，后一次追加不能覆盖前一次。
TEST_F(WALTest, LogZeroThresholdAppendsEachCall) {
  WALBufferProbe wal(test_dir, 0);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  const auto begin = Record::createRecord(1);
  const auto put = Record::putRecord(1, "k", "v");

  wal.log({});
  EXPECT_EQ(std::filesystem::file_size(path), 0u);
  wal.log({begin});
  EXPECT_EQ(read_wal_records(path), std::vector<Record>{begin});
  wal.log({put});
  const std::vector<Record> expected = {begin, put};
  EXPECT_EQ(read_wal_records(path), expected);
  EXPECT_TRUE(wal.buffered_records().empty());
}

// 目的：多个线程共享 WAL 时，每个调用的整批记录完整写入，不交错、不丢失或重复。
// 不规定线程之间的先后顺序，只核对每批内部顺序及最终记录集合。
TEST_F(WALTest, LogConcurrentBatchesStayComplete) {
  constexpr size_t worker_count = 8;
  WAL wal(test_dir, 0, 0, 0, 0);
  std::vector<std::vector<Record>> batches;
  for (size_t i = 0; i < worker_count; ++i) {
    const uint64_t id = i + 1;
    batches.push_back({Record::createRecord(id),
                       Record::putRecord(id, "key-" + std::to_string(id), "v"),
                       Record::commitRecord(id)});
  }

  std::vector<std::exception_ptr> errors(worker_count);
  std::vector<std::thread> workers;
  for (size_t i = 0; i < worker_count; ++i) {
    workers.emplace_back([&, i] {
      try {
        wal.log(batches[i], true);
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  for (const auto &error : errors) {
    ASSERT_FALSE(error);
  }

  const auto actual = read_wal_records(std::filesystem::path(test_dir) / "wal.0");
  ASSERT_EQ(actual.size(), worker_count * 3);
  std::vector<bool> seen(worker_count, false);
  for (size_t offset = 0; offset < actual.size(); offset += 3) {
    const auto id = actual[offset].getTrancId();
    ASSERT_GE(id, 1u);
    ASSERT_LE(id, worker_count);
    EXPECT_FALSE(seen[id - 1]);
    seen[id - 1] = true;
    for (size_t j = 0; j < 3; ++j) {
      EXPECT_EQ(actual[offset + j], batches[id - 1][j]);
    }
  }
}

// 目的：关闭文件后触发追加失败，缓冲必须保留，并拒绝继续接受新记录。
// 即使测试重新打开文件，也不能让失败的 WAL 悄悄重试，避免部分写入后的重复追加。
TEST_F(WALTest, LogAppendFailureStopsFurtherWrites) {
  WALBufferProbe wal(test_dir, 128);
  const std::vector<Record> pending = {Record::createRecord(1)};
  wal.log(pending);
  wal.close_file_for_test();

  EXPECT_THROW(wal.log({}, true), std::runtime_error);
  EXPECT_EQ(wal.buffered_records(), pending);

  wal.reopen_file_for_test();
  EXPECT_THROW(wal.log({Record::createRecord(2)}), std::runtime_error);
  EXPECT_THROW(wal.log({}, true), std::runtime_error);
  EXPECT_EQ(wal.buffered_records(), pending);
  EXPECT_EQ(std::filesystem::file_size(std::filesystem::path(test_dir) / "wal.0"),
            0u);
}

// 目的：启用文件大小检查时，获取大小失败不能被当作“文件超大”而执行轮换。
// 验证 size_t 错误值的检查，并确认失败后仍然拒绝继续追加。
TEST_F(WALTest, LogSizeFailureDoesNotRotateOrContinue) {
  WALBufferProbe wal(test_dir, 128, 4096);
  const std::vector<Record> pending = {Record::createRecord(1)};
  wal.log(pending);
  wal.close_file_for_test();

  EXPECT_THROW(wal.log({}, true), std::runtime_error);
  EXPECT_EQ(wal.buffered_records(), pending);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) / "wal.1"));

  wal.reopen_file_for_test();
  EXPECT_THROW(wal.log({Record::createRecord(2)}, true), std::runtime_error);
  EXPECT_EQ(wal.buffered_records(), pending);
  EXPECT_EQ(std::filesystem::file_size(std::filesystem::path(test_dir) / "wal.0"),
            0u);
}

// 目的：显式 flush 能写出尚未达到条数阈值的缓冲；空缓冲和重复 flush 不追加记录。
TEST_F(WALTest, FlushDrainsPartialBufferWithoutDuplicates) {
  WALBufferProbe wal(test_dir, 128);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  const std::vector<Record> pending = {
      Record::createRecord(3), Record::putRecord(3, "key", "value"),
      Record::commitRecord(3)};

  wal.flush();
  EXPECT_EQ(std::filesystem::file_size(path), 0u);
  wal.log(pending);
  ASSERT_EQ(std::filesystem::file_size(path), 0u);

  wal.flush();
  EXPECT_TRUE(wal.buffered_records().empty());
  EXPECT_EQ(read_wal_records(path), pending);
  const auto written_size = std::filesystem::file_size(path);

  wal.flush();
  EXPECT_EQ(std::filesystem::file_size(path), written_size);
  EXPECT_EQ(read_wal_records(path), pending);
}

// 目的：显式 flush 不吞掉文件写入异常；失败后保留缓冲，后续 flush 继续报告故障。
TEST_F(WALTest, FlushPropagatesFileFailure) {
  WALBufferProbe wal(test_dir, 128);
  const std::vector<Record> pending = {Record::createRecord(3)};
  wal.log(pending);
  wal.close_file_for_test();

  EXPECT_THROW(wal.flush(), std::runtime_error);
  EXPECT_EQ(wal.buffered_records(), pending);

  wal.reopen_file_for_test();
  EXPECT_THROW(wal.flush(), std::runtime_error);
  EXPECT_EQ(wal.buffered_records(), pending);
  EXPECT_EQ(std::filesystem::file_size(std::filesystem::path(test_dir) / "wal.0"),
            0u);
}

// 目的：调用方没有显式 flush 时，正常析构仍写出不足阈值的剩余记录。
// 分别覆盖未创建清理线程、已创建清理线程两条关闭路径，不依赖日志恢复函数。
TEST_F(WALTest, DestructorFlushesPendingRecords) {
  const std::vector<Record> pending = {
      Record::createRecord(5), Record::putRecord(5, "key", "value"),
      Record::commitRecord(5)};

  for (uint64_t interval : {uint64_t{0}, uint64_t{60}}) {
    SCOPED_TRACE(interval);
    const auto dir = std::filesystem::path(test_dir) / std::to_string(interval);
    const auto path = dir / "wal.0";
    {
      WAL wal(dir.string(), 128, 0, interval, 0);
      wal.log(pending);
      ASSERT_EQ(std::filesystem::file_size(path), 0u);
    }
    EXPECT_EQ(read_wal_records(path), pending);
  }
}

// 目的：析构触发实际文件写入失败时，捕获异常并正常完成关闭，不终止进程。
// 文件保持为空表示保存确实失败，不能把“析构没有抛异常”解释为持久化成功。
TEST_F(WALTest, DestructorContainsFlushFailure) {
  EXPECT_NO_THROW({
    WALBufferProbe wal(test_dir, 128);
    wal.log({Record::createRecord(7)});
    wal.close_file_for_test();
  });

  EXPECT_EQ(std::filesystem::file_size(std::filesystem::path(test_dir) / "wal.0"),
            0u);
}

// 目的：调用方已经抛出异常时，析构中的刷盘失败也不能盖掉原异常或终止进程。
TEST_F(WALTest, DestructorPreservesCallerExceptionWhenFlushFails) {
  EXPECT_THROW(
      {
        WALBufferProbe wal(test_dir, 128);
        wal.log({Record::createRecord(9)});
        wal.close_file_for_test();
        throw std::logic_error("caller failure");
      },
      std::logic_error);

  EXPECT_EQ(std::filesystem::file_size(std::filesystem::path(test_dir) / "wal.0"),
            0u);
}

// 目的：文件恰好达到阈值时，在下一批非空写入前轮换；空 flush 不创建新文件。
// 连续跨三个文件，验证每批记录完整保留、顺序正确，旧文件没有被覆盖或删除。
TEST_F(WALTest, RotationAtBoundaryPreservesAllFiles) {
  WALBufferProbe wal(test_dir, 0, 22);
  for (uint64_t id = 1; id <= 3; ++id) {
    const std::vector<Record> batch = {
        Record::createRecord(id), Record::commitRecord(id)};
    const auto path = std::filesystem::path(test_dir) /
                      ("wal." + std::to_string(id - 1));
    wal.log(batch);
    EXPECT_EQ(std::filesystem::file_size(path), 22u);
    EXPECT_EQ(read_wal_records(path), batch);
    EXPECT_EQ(wal.active_path(), path.string());
    EXPECT_TRUE(wal.buffered_records().empty());

    wal.flush();
    EXPECT_FALSE(std::filesystem::exists(
        std::filesystem::path(test_dir) / ("wal." + std::to_string(id))));
  }

  for (uint64_t id = 1; id <= 3; ++id) {
    const std::vector<Record> expected = {
        Record::createRecord(id), Record::commitRecord(id)};
    EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) /
                               ("wal." + std::to_string(id - 1))),
              expected);
  }
}

// 目的：单条记录超过轮换阈值仍应完整写入当前文件，不被拆开或拒绝。
// 下一批才切换文件，两个文件都能独立解码完整记录。
TEST_F(WALTest, RotationKeepsOversizedRecordWhole) {
  WALBufferProbe wal(test_dir, 0, 16);
  const auto large = Record::putRecord(7, "key", std::string(128, 'v'));
  wal.log({large});
  EXPECT_GT(std::filesystem::file_size(std::filesystem::path(test_dir) / "wal.0"),
            16u);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) / "wal.1"));

  const auto commit = Record::commitRecord(7);
  wal.log({commit});
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.0"),
            std::vector<Record>{large});
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.1"),
            std::vector<Record>{commit});
}

// 目的：构造与轮换都支持超过 int 范围的序号，避免构造成功后在 stoi 处失败。
TEST_F(WALTest, RotationContinuesFullWidthSequence) {
  const auto old_path = std::filesystem::path(test_dir) / "wal.3000000000";
  const auto old = Record::createRecord(1);
  FileObj::create_and_write(old_path.string(), old.encode());

  WALBufferProbe wal(test_dir, 0, 11);
  const auto first = Record::createRecord(2);
  const auto second = Record::commitRecord(2);
  wal.log({first});
  wal.log({second});

  EXPECT_EQ(read_wal_records(old_path), std::vector<Record>{old});
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.3000000001"),
            std::vector<Record>{first});
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.3000000002"),
            std::vector<Record>{second});
}

// 目的：下一文件名意外被占用时拒绝覆盖，保留原文件句柄、路径与待写记录。
TEST_F(WALTest, RotationRejectsExistingTargetWithoutChangingState) {
  WALBufferProbe wal(test_dir, 0, 11);
  const auto first = Record::createRecord(3);
  const auto pending = Record::commitRecord(3);
  wal.log({first});
  const auto original_path = wal.active_path();

  const auto occupied = std::filesystem::path(test_dir) / "wal.1";
  const auto unrelated = Record::createRecord(99);
  FileObj::create_and_write(occupied.string(), unrelated.encode());

  EXPECT_THROW(wal.log({pending}), std::runtime_error);
  EXPECT_EQ(wal.active_path(), original_path);
  EXPECT_EQ(wal.active_file_records(), std::vector<Record>{first});
  EXPECT_EQ(wal.buffered_records(), std::vector<Record>{pending});
  EXPECT_EQ(read_wal_records(occupied), std::vector<Record>{unrelated});
}

// 目的：目标尚不存在，但创建新文件实际失败时，也不能提前修改活动路径或句柄。
// 重命名目录保留旧文件的打开句柄，同时使原目录下的新文件创建以 ENOENT 失败。
TEST_F(WALTest, RotationCreateFailurePreservesOldHandleAndPath) {
  const auto live = std::filesystem::path(test_dir) / "live";
  const auto moved = std::filesystem::path(test_dir) / "moved";
  WALBufferProbe wal(live.string(), 0, 11);
  const auto first = Record::createRecord(5);
  const auto pending = Record::commitRecord(5);
  wal.log({first});
  const auto original_path = wal.active_path();
  std::filesystem::rename(live, moved);

  EXPECT_THROW(wal.log({pending}), std::runtime_error);
  EXPECT_EQ(wal.active_path(), original_path);
  EXPECT_EQ(wal.active_file_records(), std::vector<Record>{first});
  EXPECT_EQ(wal.buffered_records(), std::vector<Record>{pending});
  EXPECT_EQ(read_wal_records(moved / "wal.0"), std::vector<Record>{first});
  EXPECT_FALSE(std::filesystem::exists(live / "wal.1"));
}

// 目的：最后一个合法序号仍可写入，但再次轮换必须报错，不能回绕到 wal.0。
TEST_F(WALTest, RotationRejectsExhaustedSequenceWithoutWrapping) {
  const auto last = std::numeric_limits<uint64_t>::max();
  const auto old_path = std::filesystem::path(test_dir) /
                        ("wal." + std::to_string(last - 1));
  FileObj::create_and_write(old_path.string(), Record::createRecord(1).encode());

  WALBufferProbe wal(test_dir, 0, 11);
  const auto first = Record::createRecord(2);
  const auto pending = Record::commitRecord(2);
  wal.log({first});
  const auto original_path = wal.active_path();

  EXPECT_THROW(wal.log({pending}), std::overflow_error);
  EXPECT_EQ(wal.active_path(), original_path);
  EXPECT_EQ(wal.active_file_records(), std::vector<Record>{first});
  EXPECT_EQ(wal.buffered_records(), std::vector<Record>{pending});
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) / "wal.0"));
}

// 目的：并发写入每一批都触发下一次轮换时，文件切换与写入仍串行完成。
// 每个文件恰好保存一个完整批次；线程先后不固定，但事务不能丢失或重复。
TEST_F(WALTest, RotationSerializesConcurrentBatches) {
  constexpr size_t worker_count = 4;
  WAL wal(test_dir, 0, 0, 0, 1);
  std::vector<std::exception_ptr> errors(worker_count);
  std::vector<std::thread> workers;
  for (size_t i = 0; i < worker_count; ++i) {
    workers.emplace_back([&, i] {
      try {
        const uint64_t id = i + 1;
        wal.log({Record::createRecord(id), Record::commitRecord(id)});
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  for (const auto &error : errors) {
    ASSERT_FALSE(error);
  }

  std::vector<bool> seen(worker_count, false);
  for (size_t seq = 0; seq < worker_count; ++seq) {
    const auto records = read_wal_records(std::filesystem::path(test_dir) /
                                          ("wal." + std::to_string(seq)));
    ASSERT_EQ(records.size(), 2u);
    const auto id = records.front().getTrancId();
    ASSERT_GE(id, 1u);
    ASSERT_LE(id, worker_count);
    EXPECT_FALSE(seen[id - 1]);
    seen[id - 1] = true;
    const std::vector<Record> expected = {
        Record::createRecord(id), Record::commitRecord(id)};
    EXPECT_EQ(records, expected);
  }
  EXPECT_FALSE(std::filesystem::exists(
      std::filesystem::path(test_dir) / ("wal." + std::to_string(worker_count))));
}

// 目的：清理线程等待较长间隔时，不应一直占着 mutex_ 阻止前台写入。
// 预留短暂调度时间；5 秒上限远小于 60 秒清理间隔，不要求毫秒级调度精度。
// 此用例不等待清理超时，不把旧文件删除逻辑纳入本轮验收。
TEST_F(WALTest, CleanerWaitDoesNotBlockWriting) {
  WAL wal(test_dir, 128, 0, 60, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const std::vector<Record> records = {
      Record::createRecord(1), Record::putRecord(1, "key", "value"),
      Record::commitRecord(1)};

  const auto start = std::chrono::steady_clock::now();
  wal.log(records, true);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(5));
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.0"), records);
}

// 目的：关闭 WAL 能提前结束长间隔等待，不必等待 60 秒后才能 join。
// 配合已有的“创建后立即析构”测试，分别覆盖通知较早与线程有机会等待后的关闭。
TEST_F(WALTest, CleanerShutdownInterruptsLongWait) {
  auto wal = std::make_unique<WAL>(test_dir, 128, 0, 60, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const auto start = std::chrono::steady_clock::now();
  wal.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(5));
  EXPECT_TRUE(std::filesystem::is_regular_file(
      std::filesystem::path(test_dir) / "wal.0"));
}

// 目的：真实周期清理只删除完整且全部 ID 被检查点覆盖的历史文件。
// 验证检查点相等边界、乱序 ID、空历史文件，以及活动／更大序号文件保护。
TEST_F(WALTest, CleanupRespectsCheckpointAndProtectsActiveFile) {
  const auto dir = std::filesystem::path(test_dir);
  const std::vector<Record> safe = {
      Record::createRecord(3), Record::commitRecord(10)};
  const std::vector<Record> pending = {
      Record::createRecord(12), Record::commitRecord(4)};
  const std::vector<Record> active = {Record::createRecord(5)};
  const std::vector<Record> future = {Record::createRecord(1)};
  write_wal_records(dir / "wal.0", safe);
  write_wal_records(dir / "wal.1", pending);
  write_wal_records(dir / "wal.2", {});

  {
    WAL wal(test_dir, 0, 10, 1, 0);
    wal.log(active);
    // 活动文件是 wal.3，即使又出现更大文件名，也不能把 wal.3 当作旧文件删除。
    write_wal_records(dir / "wal.9", future);
    ASSERT_TRUE(wait_for_wal_condition([&] {
      return !std::filesystem::exists(dir / "wal.0") &&
             !std::filesystem::exists(dir / "wal.2");
    }));
  }

  EXPECT_EQ(read_wal_records(dir / "wal.1"), pending);
  EXPECT_EQ(read_wal_records(dir / "wal.3"), active);
  EXPECT_EQ(read_wal_records(dir / "wal.9"), future);
}

// 目的：第一次清理保留检查点之外的记录；公开 setter 推进检查点后，后续周期可删除。
// 通过先删除另一个安全文件确认首次清理已发生，而不是只检查“文件暂时还在”。
TEST_F(WALTest, CleanupUsesUpdatedCheckpoint) {
  const auto dir = std::filesystem::path(test_dir);
  const std::vector<Record> pending = {
      Record::createRecord(20), Record::commitRecord(2)};
  const std::vector<Record> later = {Record::createRecord(21)};
  write_wal_records(dir / "wal.0", pending);
  write_wal_records(dir / "wal.1", {Record::createRecord(8)});

  {
    WAL wal(test_dir, 0, 10, 1, 0);
    ASSERT_TRUE(wait_for_wal_condition([&] {
      return !std::filesystem::exists(dir / "wal.1");
    }));
    ASSERT_TRUE(std::filesystem::exists(dir / "wal.0"));
    EXPECT_EQ(read_wal_records(dir / "wal.0"), pending);

    wal.set_checkpoint_tranc_id(20);
    ASSERT_TRUE(wait_for_wal_condition([&] {
      return !std::filesystem::exists(dir / "wal.0");
    }));
    // 历史文件清完后，唯一的活动文件仍能继续写入。
    wal.log(later);
  }
  EXPECT_EQ(read_wal_records(dir / "wal.2"), later);
}

// 目的：损坏日志和非日志文件保持原样，且不能阻断同一轮对正常历史文件的清理。
// 覆盖一字节尾部、零记录长度、截断记录、未知类型和有效记录后的残缺尾部。
TEST_F(WALTest, CleanupPreservesMalformedAndUnrelatedFiles) {
  const auto dir = std::filesystem::path(test_dir);
  auto truncated = Record::createRecord(7).encode();
  truncated.pop_back();
  auto unknown = Record::createRecord(7).encode();
  unknown[10] = 255;
  auto trailing = Record::createRecord(7).encode();
  trailing.push_back(1);
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> preserved = {
      {"wal.0", {1}}, {"wal.1", std::vector<uint8_t>(11, 0)},
      {"wal.2", truncated}, {"wal.3", unknown}, {"wal.4", trailing},
      {"wal.backup", {1, 2}}, {"wal.12.bak", {3, 4}},
      {"wal.", {5, 6}}, {"notes.txt", {7, 8}}};
  for (const auto &[name, bytes] : preserved) {
    FileObj::create_and_write((dir / name).string(), bytes);
  }
  write_wal_records(dir / "wal.5", {Record::createRecord(1)});

  {
    WAL wal(test_dir, 128, 10, 1, 0);
    ASSERT_TRUE(wait_for_wal_condition([&] {
      return !std::filesystem::exists(dir / "wal.5");
    }));
  }

  for (const auto &[name, bytes] : preserved) {
    SCOPED_TRACE(name);
    auto file = FileObj::open((dir / name).string());
    EXPECT_EQ(file.read_to_slice(0, file.size()), bytes);
  }
  EXPECT_TRUE(std::filesystem::is_regular_file(dir / "wal.6"));
}

// 目的：后台清理与前台轮换、检查点更新同时运行时，不丢失仍需保留的事务日志。
// 低版本历史文件用于观察清理确实执行；前台写入的 ID 始终高于本测试的检查点。
TEST_F(WALTest, CleanupCoordinatesWithWritesRotationAndCheckpointUpdates) {
  constexpr size_t batch_count = 32;
  const auto dir = std::filesystem::path(test_dir);
  write_wal_records(dir / "wal.0", {Record::createRecord(1)});
  {
    WAL wal(test_dir, 0, 0, 1, 1);
    std::vector<std::exception_ptr> errors(2);
    std::thread writer([&] {
      try {
        for (size_t i = 0; i < batch_count; ++i) {
          const uint64_t id = 1000 + i;
          wal.log({Record::createRecord(id), Record::commitRecord(id)});
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
      } catch (...) {
        errors[0] = std::current_exception();
      }
    });
    std::thread checkpoint_writer([&] {
      try {
        for (uint64_t id = 1; id <= batch_count; ++id) {
          wal.set_checkpoint_tranc_id(id);
          std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
      } catch (...) {
        errors[1] = std::current_exception();
      }
    });
    writer.join();
    checkpoint_writer.join();
    for (const auto &error : errors) {
      ASSERT_FALSE(error);
    }
    ASSERT_TRUE(wait_for_wal_condition([&] {
      return !std::filesystem::exists(dir / "wal.0");
    }));
  }

  for (size_t i = 0; i < batch_count; ++i) {
    const uint64_t id = 1000 + i;
    const std::vector<Record> expected = {
        Record::createRecord(id), Record::commitRecord(id)};
    EXPECT_EQ(read_wal_records(dir / ("wal." + std::to_string(i + 1))), expected);
  }
}

// 管理器与 WAL 的桥接测试：只在新建测试目录初始化 WAL，避免依赖尚未实现的恢复入口。
class TranManagerWALTest : public WALTest {};

// 目的：未初始化 WAL 时，空／非空写入都报告失败，不解引用空指针或伪装成功。
TEST_F(TranManagerWALTest, MissingWalReturnsFalse) {
  auto manager = std::make_shared<TranManager>(test_dir);
  EXPECT_FALSE(manager->write_to_wal({}));
  EXPECT_FALSE(manager->write_to_wal({Record::createRecord(1)}));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) / "wal.0"));
}

// 目的：不到 128 条的批次也必须立即写出；读取真实文件验证 force_flush 生效。
// 只写日志不能推进 SST 检查点或分配新事务 ID。
TEST_F(TranManagerWALTest, ForcesSmallBatchWithoutAdvancingTransactionState) {
  auto manager = std::make_shared<TranManager>(test_dir);
  manager->init_new_wal();
  const std::vector<Record> records = {
      Record::createRecord(7), Record::putRecord(7, "key", "value"),
      Record::commitRecord(7)};

  ASSERT_TRUE(manager->write_to_wal(records));
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.0"), records);
  EXPECT_EQ(manager->get_checkpoint_tranc_id(), 0u);
  EXPECT_EQ(manager->get_max_flushed_tranc_id(), 0u);
  EXPECT_EQ(manager->getNextTransactionId(), 1u);
}

// 目的：已初始化且无故障时，空批次允许成功，不产生额外或重复的日志记录。
TEST_F(TranManagerWALTest, EmptyBatchDoesNotDuplicateRecords) {
  auto manager = std::make_shared<TranManager>(test_dir);
  manager->init_new_wal();
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  ASSERT_TRUE(manager->write_to_wal({}));
  EXPECT_EQ(std::filesystem::file_size(path), 0u);

  const std::vector<Record> records = {
      Record::createRecord(1), Record::commitRecord(1)};
  ASSERT_TRUE(manager->write_to_wal(records));
  const auto written_size = std::filesystem::file_size(path);
  ASSERT_TRUE(manager->write_to_wal({}));
  EXPECT_EQ(std::filesystem::file_size(path), written_size);
  EXPECT_EQ(read_wal_records(path), records);
}

// 目的：WAL 真正抛出文件操作异常时，管理器必须返回 false，而不是只记录错误后返回 true。
// 先写满 wal.0，再占用 wal.1 触发轮换失败；验证旧数据保留，后续故障也继续报告失败。
TEST_F(TranManagerWALTest, FileFailureReturnsFalseAndPreservesPreviousLogs) {
  auto manager = std::make_shared<TranManager>(test_dir);
  manager->init_new_wal();
  const auto dir = std::filesystem::path(test_dir);
  const std::vector<Record> successful = {
      Record::createRecord(1), Record::putRecord(1, "key", std::string(5000, 'v')),
      Record::commitRecord(1)};
  ASSERT_TRUE(manager->write_to_wal(successful));
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);

  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);
  EXPECT_FALSE(manager->write_to_wal({Record::createRecord(2)}));
  EXPECT_FALSE(manager->write_to_wal({Record::createRecord(3)}));
  EXPECT_FALSE(manager->write_to_wal({}));

  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
  EXPECT_EQ(manager->get_checkpoint_tranc_id(), 0u);
}

// 通过公开事务接口验收 WAL 接入；每个测试使用新目录，不调用尚未实现的启动恢复。
class TranPutWALTest : public WALTest {
protected:
  void SetUp() override {
    WALTest::SetUp();
    engine = std::make_shared<LSMEngine>(test_dir);
    manager = std::make_shared<TranManager>(test_dir);
    manager->set_engine(engine);
    engine->set_tran_manager(manager);
    manager->init_new_wal();
  }

  void TearDown() override {
    manager.reset();
    engine.reset();
    WALTest::TearDown();
  }

  std::shared_ptr<LSMEngine> engine;
  std::shared_ptr<TranManager> manager;
};

// 目的：公开 put 的记录长度检查先于 WAL 和引擎写入，非法输入不留下半条操作记录。
// 正常写入的先后顺序由代码检查，以及后面的 WAL 故障不改数据用例共同验证。
TEST_F(TranPutWALTest, OversizedRecordDoesNotChangeWalOrEngine) {
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  EXPECT_THROW(transaction->put("key", std::string(65536, 'v')), std::length_error);
  EXPECT_EQ(std::filesystem::file_size(path), 0u);
  EXPECT_FALSE(engine->get("key", 0));
  ASSERT_EQ(transaction->operations.size(), 1u);
  EXPECT_EQ(transaction->operations.front().getOperationType(), OperationType::OP_CREATE);
}

// 目的：同一 RU 事务多次 put 时只发送新增记录，CREATE 与旧 PUT 不会被重复写入。
// operations 保留完整历史；写出 WAL 不等于提交，严格读取仍排除这笔活跃事务。
TEST_F(TranPutWALTest, RepeatedPutsWriteOnlyNewRecords) {
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  std::vector<Record> expected = {Record::createRecord(id)};

  transaction->put("key", "first");
  expected.push_back(Record::putRecord(id, "key", "first"));
  EXPECT_EQ(read_wal_records(path), expected);
  transaction->put("key", "last");
  expected.push_back(Record::putRecord(id, "key", "last"));
  EXPECT_EQ(read_wal_records(path), expected);
  transaction->put("new", "created");
  expected.push_back(Record::putRecord(id, "new", "created"));

  EXPECT_EQ(read_wal_records(path), expected);
  EXPECT_EQ(transaction->operations, expected);
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_EQ(engine->get("key", 0),
            std::make_optional(std::make_pair(std::string("last"), id)));
  EXPECT_FALSE(engine->get("key", 0, true));
}

// 目的：暂存型事务的 put 不提前写 WAL 或修改引擎，重复 key 仍保留最后一次暂存值。
TEST_F(TranPutWALTest, StagedLevelsDoNotWriteWalBeforeCommit) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("existing", "old", base_id);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                    IsolationLevel::REPEATABLE_READ,
                    IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    auto transaction = manager->new_tranc(level);
    transaction->put("existing", "first");
    transaction->put("existing", "last");
    transaction->put("new", "pending");

    EXPECT_EQ(std::filesystem::file_size(path), 0u);
    EXPECT_EQ(transaction->operations.size(), 4u);
    EXPECT_EQ(transaction->temp_map_.at("existing"), "last");
    EXPECT_EQ(transaction->get("existing"), std::make_optional(std::string("last")));
    EXPECT_EQ(engine->get("existing", 0),
              std::make_optional(std::make_pair(std::string("old"), base_id)));
    EXPECT_FALSE(engine->get("new", 0));
  }
}

// 目的：有管理器但 WAL 未初始化时，RU put 明确失败，不能先把数据写入内存。
TEST_F(TranPutWALTest, MissingWalDoesNotApplyWrite) {
  const auto dir = (std::filesystem::path(test_dir) / "without_wal").string();
  auto other_engine = std::make_shared<LSMEngine>(dir);
  auto other_manager = std::make_shared<TranManager>(dir);
  other_manager->set_engine(other_engine);
  other_engine->set_tran_manager(other_manager);
  auto transaction = other_manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);

  EXPECT_THROW(transaction->put("blocked", "value"), std::runtime_error);
  EXPECT_FALSE(other_engine->get("blocked", 0));
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(dir) / "wal.0"));
}

// 目的：已成功写过日志后，后续真实 WAL 文件故障不能覆盖旧值或新增数据。
// 填满 wal.0 并占用 wal.1，触发轮换失败；后续请求也不能绕过故障状态写引擎。
TEST_F(TranPutWALTest, WalFailurePreservesEarlierValueAndRejectsNewWrite) {
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  transaction->put("key", "before");
  auto padding_transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  padding_transaction->put("padding", std::string(5000, 'v'));
  const auto dir = std::filesystem::path(test_dir);
  const auto successful = read_wal_records(dir / "wal.0");
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  EXPECT_THROW(transaction->put("key", "after"), std::runtime_error);
  EXPECT_THROW(transaction->put("new", "blocked"), std::runtime_error);
  EXPECT_EQ(engine->get("key", 0),
            std::make_optional(std::make_pair(std::string("before"), id)));
  EXPECT_FALSE(engine->get("new", 0));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
  EXPECT_FALSE(transaction->isCommited);
}

// 删除日志测试复用已初始化 Engine、TranManager 和 WAL 的事务夹具。
class TranRemoveWALTest : public TranPutWALTest {};

// 目的：第一次操作就是 remove 时，日志包含 CREATE 和 DELETE，内存写入对应版本的墓碑。
// 该事务尚未提交，严格读取仍可见旧值；历史版本也不能被物理删除。
TEST_F(TranRemoveWALTest, FirstRemoveWritesBeginAndDeleteRecord) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("existing", "old", base_id);
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;

  transaction->remove("existing");
  const std::vector<Record> expected = {
      Record::createRecord(id), Record::deleteRecord(id, "existing")};
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.0"), expected);
  EXPECT_EQ(transaction->operations, expected);
  EXPECT_TRUE(transaction->temp_map_.empty());
  EXPECT_FALSE(engine->get("existing", 0));
  EXPECT_EQ(engine->memtable.get_record("existing", 0),
            std::make_optional(std::make_pair(std::string{}, id)));
  EXPECT_EQ(engine->get("existing", base_id),
            std::make_optional(std::make_pair(std::string("old"), base_id)));
  EXPECT_EQ(engine->get("existing", 0, true),
            std::make_optional(std::make_pair(std::string("old"), base_id)));
  EXPECT_FALSE(transaction->isCommited);
}

// 目的：put/remove 共用同一个日志进度，交替调用不会重发 CREATE 或此前的操作。
// 重复删除不存在的 key 仍是新的删除请求，按调用顺序保留在日志中。
TEST_F(TranRemoveWALTest, MixedOperationsPreserveOrderWithoutResendingPrefix) {
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  transaction->remove("missing");
  transaction->put("key", "first");
  transaction->remove("key");
  transaction->put("key", "last");
  transaction->remove("missing");

  const std::vector<Record> expected = {
      Record::createRecord(id), Record::deleteRecord(id, "missing"),
      Record::putRecord(id, "key", "first"), Record::deleteRecord(id, "key"),
      Record::putRecord(id, "key", "last"), Record::deleteRecord(id, "missing")};
  EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.0"), expected);
  EXPECT_EQ(transaction->operations, expected);
  EXPECT_EQ(engine->get("key", 0),
            std::make_optional(std::make_pair(std::string("last"), id)));
  EXPECT_FALSE(engine->get("missing", 0));
  EXPECT_EQ(engine->memtable.get_record("missing", 0),
            std::make_optional(std::make_pair(std::string{}, id)));
}

// 目的：暂存型删除保留空值墓碑，覆盖本事务的暂存值和读缓存；提交前不写 WAL 或引擎。
TEST_F(TranRemoveWALTest, StagedDeletesStayLocalBeforeCommit) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("existing", "old", base_id);
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                    IsolationLevel::REPEATABLE_READ,
                    IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    auto transaction = manager->new_tranc(level);
    ASSERT_EQ(transaction->get("existing"), std::make_optional(std::string("old")));
    transaction->put("existing", "pending");
    transaction->remove("existing");
    transaction->remove("missing");

    EXPECT_EQ(transaction->temp_map_.at("existing"), "");
    EXPECT_EQ(transaction->temp_map_.at("missing"), "");
    EXPECT_FALSE(transaction->get("existing"));
    EXPECT_FALSE(transaction->get("missing"));
    EXPECT_EQ(transaction->operations.size(), 4u);
    EXPECT_EQ(std::filesystem::file_size(path), 0u);
    EXPECT_EQ(engine->get("existing", 0),
              std::make_optional(std::make_pair(std::string("old"), base_id)));
    EXPECT_FALSE(engine->memtable.get_record("missing", 0));
  }
}

// 目的：WAL 未初始化时，删除已有或不存在的 key 都应失败，不能先写入墓碑。
TEST_F(TranRemoveWALTest, MissingWalDoesNotApplyTombstone) {
  const auto dir = (std::filesystem::path(test_dir) / "without_wal").string();
  auto other_engine = std::make_shared<LSMEngine>(dir);
  auto other_manager = std::make_shared<TranManager>(dir);
  other_manager->set_engine(other_engine);
  other_engine->set_tran_manager(other_manager);
  const auto base_id = other_manager->getNextTransactionId();
  other_engine->put("existing", "old", base_id);
  auto transaction = other_manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);

  EXPECT_THROW(transaction->remove("existing"), std::runtime_error);
  EXPECT_THROW(transaction->remove("missing"), std::runtime_error);
  EXPECT_EQ(other_engine->memtable.get_record("existing", 0),
            std::make_optional(std::make_pair(std::string("old"), base_id)));
  EXPECT_FALSE(other_engine->memtable.get_record("missing", 0));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(dir) / "wal.0"));
}

// 目的：真实 WAL 轮换失败时，remove 不得删除此前成功写入的值，也不能新增墓碑。
TEST_F(TranRemoveWALTest, WalFailurePreservesValueAndDoesNotCreateTombstone) {
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  transaction->put("existing", "before");
  auto padding_transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  padding_transaction->put("padding", std::string(5000, 'v'));
  const auto dir = std::filesystem::path(test_dir);
  const auto successful = read_wal_records(dir / "wal.0");
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  EXPECT_THROW(transaction->remove("existing"), std::runtime_error);
  EXPECT_THROW(transaction->remove("missing"), std::runtime_error);
  EXPECT_EQ(engine->memtable.get_record("existing", 0),
            std::make_optional(std::make_pair(std::string("before"), id)));
  EXPECT_FALSE(engine->memtable.get_record("missing", 0));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

class TranCommitWALTest : public TranPutWALTest {};

// 目的：暂存事务提交时写出完整操作历史和 COMMIT，成功后才对严格读取可见。
// 三个暂存级别都覆盖重复 put、删除和事务状态；中间值仍留在日志中供重放。
TEST_F(TranCommitWALTest, StagedCommitPersistsHistoryAndPublishesFinalValues) {
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  std::vector<Record> expected;
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    const auto base_id = manager->getNextTransactionId();
    const auto suffix = std::to_string(base_id);
    const auto key = "key-" + suffix;
    const auto removed = "removed-" + suffix;
    engine->put(removed, "old", base_id);
    auto transaction = manager->new_tranc(level);
    const auto id = transaction->tranc_id_;
    transaction->put(key, "first");
    transaction->put(key, "last");
    transaction->remove(removed);
    EXPECT_EQ(read_wal_records(path), expected);
    EXPECT_FALSE(engine->get(key, 0, true));
    ASSERT_TRUE(engine->get(removed, 0, true));
    EXPECT_TRUE(manager->should_skip_read(id, true));

    ASSERT_TRUE(transaction->commit());
    const std::vector<Record> batch = {
        Record::createRecord(id), Record::putRecord(id, key, "first"),
        Record::putRecord(id, key, "last"), Record::deleteRecord(id, removed),
        Record::commitRecord(id)};
    expected.insert(expected.end(), batch.begin(), batch.end());
    EXPECT_EQ(read_wal_records(path), expected);
    EXPECT_EQ(transaction->operations, batch);
    EXPECT_TRUE(transaction->isCommited);
    EXPECT_FALSE(transaction->isAborted);
    EXPECT_FALSE(manager->should_skip_read(id, true));
    EXPECT_EQ(engine->get(key, 0, true),
              std::make_optional(std::make_pair(std::string("last"), id)));
    EXPECT_FALSE(engine->get(removed, 0, true));
    EXPECT_EQ(engine->memtable.get_record(removed, 0),
              std::make_optional(std::make_pair(std::string{}, id)));
  }
}

// 目的：RU 的数据操作已预写，commit 只能追加 COMMIT，不能重发 CREATE/PUT/DELETE。
// 提交前严格查询排除活跃事务，提交后看到本次值和删除。
TEST_F(TranCommitWALTest, ReadUncommittedCommitOnlyAppendsCommit) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("removed", "old", base_id);
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  transaction->put("key", "value");
  transaction->remove("removed");
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  std::vector<Record> expected = {
      Record::createRecord(id), Record::putRecord(id, "key", "value"),
      Record::deleteRecord(id, "removed")};
  EXPECT_EQ(read_wal_records(path), expected);
  EXPECT_FALSE(engine->get("key", 0, true));
  ASSERT_TRUE(engine->get("removed", 0, true));

  ASSERT_TRUE(transaction->commit());
  expected.push_back(Record::commitRecord(id));
  EXPECT_EQ(read_wal_records(path), expected);
  EXPECT_EQ(transaction->operations, expected);
  EXPECT_FALSE(manager->should_skip_read(id, true));
  EXPECT_EQ(engine->get("key", 0, true),
            std::make_optional(std::make_pair(std::string("value"), id)));
  EXPECT_FALSE(engine->get("removed", 0, true));
}

// 目的：没有写操作的事务也能正常结束；重复 commit/abort 不追加任何终态日志。
// RU 从未调用过 put/remove 时，也需要在首次提交中发送 CREATE。
TEST_F(TranCommitWALTest, ReadOnlyCommitAndRepeatedEndDoNotDuplicateRecords) {
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  std::vector<Record> expected;
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    auto transaction = manager->new_tranc(level);
    const auto id = transaction->tranc_id_;
    EXPECT_FALSE(transaction->get("missing"));
    ASSERT_TRUE(transaction->commit());
    expected.push_back(Record::createRecord(id));
    expected.push_back(Record::commitRecord(id));
    EXPECT_EQ(read_wal_records(path), expected);
    EXPECT_FALSE(transaction->commit());
    EXPECT_FALSE(transaction->abort());
    EXPECT_EQ(transaction->operations.size(), 2u);
    EXPECT_EQ(read_wal_records(path), expected);
    EXPECT_FALSE(manager->should_skip_read(id, true));
    EXPECT_FALSE(manager->is_aborted(id));
  }
}

// 目的：旧事务遇到已经提交的新版本时，取消全部暂存修改，不能写出 COMMIT。
// 同时放一个没有冲突的 key，确保检查完整个事务之前没有部分应用数据。
TEST_F(TranCommitWALTest, WriteConflictDoesNotWriteCommitOrPendingValues) {
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    const auto suffix = std::to_string(static_cast<int>(level));
    const auto key = "conflict-" + suffix;
    const auto extra = "extra-" + suffix;
    auto older = manager->new_tranc(level);
    older->put(key, "stale");
    older->put(extra, "pending");
    auto newer = manager->new_tranc(IsolationLevel::READ_OP_COMMITTED);
    newer->put(key, "new");
    ASSERT_TRUE(newer->commit());
    const auto successful = read_wal_records(path);
    const auto operation_count = older->operations.size();

    EXPECT_FALSE(older->commit());
    EXPECT_TRUE(older->isAborted);
    EXPECT_FALSE(older->isCommited);
    EXPECT_TRUE(manager->is_aborted(older->tranc_id_));
    EXPECT_FALSE(engine->get(extra, 0));
    EXPECT_EQ(engine->get(key, 0, true),
              std::make_optional(std::make_pair(std::string("new"),
                                                newer->tranc_id_)));
    EXPECT_EQ(older->operations.size(), operation_count);
    EXPECT_EQ(read_wal_records(path), successful);
  }
}

// 目的：确认当前 SERIALIZABLE 分支采用提交时验证，不是在 get 后一直锁住读取的 key。
// T1 读 A 后暂存 B；T2 修改 A 并成功提交；T1 仍读自己的缓存，但提交失败且 B 不入库。
// 这是现有单键验证的测试，不把它当作完整可串行化或范围幻读验证。
// ! SERIALIZABLE（未完整实现）：本例只证明读过的单个 key 变化时会拒绝提交。
TEST_F(TranCommitWALTest, ReadChangeRejectsCommitWithoutBlockingOtherWriter) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("A", "old", base_id);
  auto reader = manager->new_tranc(IsolationLevel::SERIALIZABLE);
  EXPECT_EQ(reader->get("A"), std::make_optional(std::string("old")));
  reader->put("B", "based-on-old-A");
  auto writer = manager->new_tranc(IsolationLevel::READ_OP_COMMITTED);
  writer->put("A", "new");
  ASSERT_TRUE(writer->commit());
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  const std::vector<Record> expected = {
      Record::createRecord(writer->tranc_id_),
      Record::putRecord(writer->tranc_id_, "A", "new"),
      Record::commitRecord(writer->tranc_id_)};

  EXPECT_EQ(reader->get("A"), std::make_optional(std::string("old")));
  EXPECT_FALSE(reader->commit());
  EXPECT_TRUE(manager->is_aborted(reader->tranc_id_));
  EXPECT_FALSE(engine->get("B", 0));
  EXPECT_EQ(read_wal_records(path), expected);
}

// 目的：尚未初始化 WAL 的提交不能应用暂存数据，也不能登记提交或自动改成回滚。
TEST_F(TranCommitWALTest, MissingWalDoesNotApplyOrFinishTransaction) {
  const auto dir = std::filesystem::path(test_dir) / "uninitialized";
  auto missing_engine = std::make_shared<LSMEngine>(dir.string());
  auto missing_manager = std::make_shared<TranManager>(dir.string());
  missing_manager->set_engine(missing_engine);
  missing_engine->set_tran_manager(missing_manager);
  auto transaction = missing_manager->new_tranc(IsolationLevel::READ_OP_COMMITTED);
  transaction->put("key", "pending");

  EXPECT_THROW(transaction->commit(), std::runtime_error);
  EXPECT_FALSE(missing_engine->get("key", 0));
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_TRUE(missing_manager->should_skip_read(transaction->tranc_id_, true));
  EXPECT_FALSE(missing_manager->is_aborted(transaction->tranc_id_));
  EXPECT_FALSE(std::filesystem::exists(dir / "wal.0"));
}

// 目的：真实轮换失败发生在暂存数据应用之前，旧值、旧日志及活跃状态都保留。
// 结束尝试已经留下 COMMIT 后，不自动再次提交；避免重复或相互矛盾的终态记录。
TEST_F(TranCommitWALTest, WalFailurePreservesDataAndRejectsRepeatedCommit) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("key", "old", base_id);
  auto transaction = manager->new_tranc(IsolationLevel::REPEATABLE_READ);
  transaction->remove("key");
  transaction->put("new", "pending");
  auto padding = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  padding->put("padding", std::string(5000, 'v'));
  const auto dir = std::filesystem::path(test_dir);
  const auto successful = read_wal_records(dir / "wal.0");
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  EXPECT_THROW(transaction->commit(), std::runtime_error);
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_TRUE(manager->should_skip_read(transaction->tranc_id_, true));
  EXPECT_FALSE(manager->is_aborted(transaction->tranc_id_));
  EXPECT_FALSE(manager->get_flushed_tranc_ids().contains(transaction->tranc_id_));
  EXPECT_EQ(engine->get("key", 0, true),
            std::make_optional(std::make_pair(std::string("old"), base_id)));
  EXPECT_FALSE(engine->get("new", 0));
  ASSERT_EQ(transaction->operations.back().getOperationType(), OperationType::OP_COMMIT);
  const auto count = transaction->operations.size();
  EXPECT_THROW(transaction->commit(), std::logic_error);
  // COMMIT 的写入尝试尚未正常结束，不能随后追加相反的 ROLLBACK。
  EXPECT_THROW(transaction->abort(), std::logic_error);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_FALSE(manager->is_aborted(transaction->tranc_id_));
  EXPECT_EQ(transaction->operations.size(), count);
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

class TranAbortWALTest : public TranPutWALTest {};

// 目的：暂存型事务的修改尚未写入日志或引擎，取消只需登记并清理上下文。
// 覆盖修改、删除、新增和读取缓存；SERIALIZABLE 仅验证当前实验性分支。
TEST_F(TranAbortWALTest, StagedAbortKeepsWalEmptyAndPreservesStoredValues) {
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  for (auto level : {IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    const auto base_id = manager->getNextTransactionId();
    const auto suffix = std::to_string(base_id);
    const auto key = "key-" + suffix;
    const auto removed = "removed-" + suffix;
    const auto added = "added-" + suffix;
    engine->put(key, "old", base_id);
    engine->put(removed, "keep", base_id);
    auto transaction = manager->new_tranc(level);
    EXPECT_EQ(transaction->get(key), std::make_optional(std::string("old")));
    transaction->put(key, "pending");
    transaction->remove(removed);
    transaction->put(added, "pending");

    ASSERT_TRUE(transaction->abort());
    EXPECT_TRUE(transaction->isAborted);
    EXPECT_FALSE(transaction->isCommited);
    EXPECT_TRUE(manager->is_aborted(transaction->tranc_id_));
    EXPECT_TRUE(transaction->operations.empty());
    EXPECT_TRUE(transaction->temp_map_.empty());
    EXPECT_EQ(engine->get(key, 0),
              std::make_optional(std::make_pair(std::string("old"), base_id)));
    EXPECT_EQ(engine->get(removed, 0),
              std::make_optional(std::make_pair(std::string("keep"), base_id)));
    EXPECT_FALSE(engine->get(added, 0));
    EXPECT_FALSE(transaction->abort());
    EXPECT_FALSE(transaction->commit());
    EXPECT_THROW(transaction->get(key), std::logic_error);
    EXPECT_EQ(std::filesystem::file_size(path), 0u);
  }
}

// 目的：只读事务的 CREATE 仅在上下文中，取消时无需为了结束而写一组日志。
TEST_F(TranAbortWALTest, ReadOnlyAbortDoesNotCreateWalRecords) {
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    auto transaction = manager->new_tranc(level);
    EXPECT_FALSE(transaction->get("missing"));
    ASSERT_TRUE(transaction->abort());
    EXPECT_TRUE(manager->is_aborted(transaction->tranc_id_));
    EXPECT_TRUE(transaction->operations.empty());
    EXPECT_FALSE(transaction->abort());
  }
  EXPECT_TRUE(read_wal_records(std::filesystem::path(test_dir) / "wal.0").empty());
}

// 目的：RU 的修改已进入 SST 后，取消仍追加一次 ROLLBACK，查询按回滚名单恢复旧版本。
// 同时检查修改、墓碑、新增记录；重复结束不能重发日志，也不能把取消改成提交。
TEST_F(TranAbortWALTest, RollbackAppendsOnceAndFiltersFlushedVersions) {
  const auto base_id = manager->getNextTransactionId();
  engine->put("updated", "old", base_id);
  engine->put("removed", "keep", base_id);
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  transaction->put("updated", "dirty");
  transaction->remove("removed");
  transaction->put("added", "dirty");
  engine->flush();
  ASSERT_EQ(engine->memtable.get_total_size(), 0u);
  ASSERT_FALSE(engine->ssts.empty());
  const auto path = std::filesystem::path(test_dir) / "wal.0";
  std::vector<Record> expected = {
      Record::createRecord(id), Record::putRecord(id, "updated", "dirty"),
      Record::deleteRecord(id, "removed"), Record::putRecord(id, "added", "dirty")};
  ASSERT_EQ(read_wal_records(path), expected);

  ASSERT_TRUE(transaction->abort());
  expected.push_back(Record::rollbackRecord(id));
  EXPECT_EQ(read_wal_records(path), expected);
  EXPECT_TRUE(manager->is_aborted(id));
  EXPECT_TRUE(transaction->isAborted);
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_TRUE(transaction->operations.empty());
  EXPECT_TRUE(transaction->temp_map_.empty());
  EXPECT_EQ(engine->get("updated", 0),
            std::make_optional(std::make_pair(std::string("old"), base_id)));
  EXPECT_EQ(engine->get("removed", 0),
            std::make_optional(std::make_pair(std::string("keep"), base_id)));
  EXPECT_FALSE(engine->get("added", 0));
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  EXPECT_FALSE(transaction->abort());
  EXPECT_FALSE(transaction->commit());
  EXPECT_EQ(read_wal_records(path), expected);
}

// 目的：从未发送日志的事务，取消时不应强制依赖已初始化的 WAL。
// RU 只读；其他三个级别暂存写入后取消，均可直接丢弃，不产生数据或日志文件。
TEST_F(TranAbortWALTest, LocalOnlyAbortWorksWithoutInitializedWal) {
  const auto dir = std::filesystem::path(test_dir) / "uninitialized";
  auto local_engine = std::make_shared<LSMEngine>(dir.string());
  auto local_manager = std::make_shared<TranManager>(dir.string());
  local_manager->set_engine(local_engine);
  local_engine->set_tran_manager(local_manager);
  for (auto level : {IsolationLevel::READ_UNOP_COMMITTED,
                     IsolationLevel::READ_OP_COMMITTED,
                     IsolationLevel::REPEATABLE_READ,
                     IsolationLevel::SERIALIZABLE}) {
    SCOPED_TRACE(static_cast<int>(level));
    auto transaction = local_manager->new_tranc(level);
    if (level != IsolationLevel::READ_UNOP_COMMITTED)
      transaction->put("key", "pending");
    ASSERT_TRUE(transaction->abort());
    EXPECT_TRUE(local_manager->is_aborted(transaction->tranc_id_));
    EXPECT_TRUE(transaction->operations.empty());
    EXPECT_FALSE(local_engine->get("key", 0));
  }
  EXPECT_FALSE(std::filesystem::exists(dir / "wal.0"));
}

// 目的：回滚日志真正写入失败时，不能登记取消、清空现场或隐藏仍然活跃的 RU 记录。
// 占用下一日志文件制造轮换错误；再次结束必须被拒绝，不能重复追加 ROLLBACK/COMMIT。
TEST_F(TranAbortWALTest, WalFailurePreservesActiveContextAndRejectsRepeatedEnd) {
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;
  transaction->put("key", "dirty");
  auto padding = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  padding->put("padding", std::string(5000, 'v'));
  const auto dir = std::filesystem::path(test_dir);
  const auto successful = read_wal_records(dir / "wal.0");
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);
  const auto before_count = transaction->operations.size();

  EXPECT_THROW(transaction->abort(), std::runtime_error);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_FALSE(manager->is_aborted(id));
  EXPECT_TRUE(manager->should_skip_read(id, true));
  EXPECT_EQ(engine->get("key", 0),
            std::make_optional(std::make_pair(std::string("dirty"), id)));
  EXPECT_FALSE(engine->get("key", 0, true));
  ASSERT_EQ(transaction->operations.size(), before_count + 1);
  EXPECT_EQ(transaction->operations.back(), Record::rollbackRecord(id));
  EXPECT_THROW(transaction->abort(), std::logic_error);
  EXPECT_THROW(transaction->commit(), std::logic_error);
  EXPECT_EQ(transaction->operations.size(), before_count + 1);
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

// 目的：首次 RU 写入失败，已写计数仍为 0，abort 不能因此跳过日志而报告成功。
// 通过另一事务填满活动文件，让本事务的第一次 put 就遇到真实文件轮换故障。
TEST_F(TranAbortWALTest, FailedFirstWriteCannotSkipRollbackWal) {
  auto padding = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  padding->put("padding", std::string(5000, 'v'));
  const auto dir = std::filesystem::path(test_dir);
  const auto successful = read_wal_records(dir / "wal.0");
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);
  auto transaction = manager->new_tranc(IsolationLevel::READ_UNOP_COMMITTED);
  const auto id = transaction->tranc_id_;

  EXPECT_THROW(transaction->put("key", "pending"), std::runtime_error);
  EXPECT_FALSE(engine->memtable.get_record("key", 0));
  EXPECT_THROW(transaction->abort(), std::runtime_error);
  EXPECT_FALSE(transaction->isAborted);
  EXPECT_FALSE(transaction->isCommited);
  EXPECT_FALSE(manager->is_aborted(id));
  EXPECT_TRUE(manager->should_skip_read(id, true));
  const std::vector<Record> expected_context = {
      Record::createRecord(id), Record::putRecord(id, "key", "pending"),
      Record::rollbackRecord(id)};
  EXPECT_EQ(transaction->operations, expected_context);
  EXPECT_THROW(transaction->abort(), std::logic_error);
  EXPECT_EQ(transaction->operations, expected_context);
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

TEST_F(WALTest, LogAndFlush) {
  MockWAL wal(test_dir, 10, 100);

  auto mock_file = wal.get_log_file();

  std::vector<Record> records1;
  std::vector<Record> records2;
  int record1_len = 8;
  int record2_len = 12;

  uint64_t tranc_id = 0;
  for (int i = 0; i < record1_len; i++) {
    auto tranc_str = std::to_string(tranc_id);
    auto key = tranc_str + "-key-" + std::to_string(i);
    if (i == 0) {
      auto rec = Record::createRecord(tranc_id);
      records1.push_back(rec);
    } else if (i == record1_len - 1) {
      auto rec = Record::commitRecord(tranc_id++);
      records1.push_back(rec);
    } else {
      auto value = tranc_str + "-value-" + std::to_string(i);
      auto rec = Record::putRecord(tranc_id, key, value);
      records1.push_back(rec);
    }
  }

  for (int i = 0; i < record2_len; i++) {
    auto tranc_str = std::to_string(tranc_id);
    auto key = tranc_str + "-key-" + std::to_string(i);
    if (i == 0) {
      auto rec = Record::createRecord(tranc_id);
      records2.push_back(rec);
    } else if (i == record2_len - 1) {
      auto rec = Record::commitRecord(tranc_id++);
      records2.push_back(rec);
    } else {
      auto value = tranc_str + "-value-" + std::to_string(i);
      auto rec = Record::putRecord(tranc_id, key, value);
      records2.push_back(rec);
    }
  }

  wal.log(records1, false);
  EXPECT_EQ(wal.get_log_buffer().size(), 8);

  wal.log(records2, false);
  EXPECT_EQ(wal.get_log_buffer().size(), 0);

  wal.flush();
  EXPECT_EQ(wal.get_log_buffer().size(), 0);
}

// 目的：用真实 WAL 写入并轮换后，恢复函数必须完整收集三个事务的历史。
// 包括已提交、已回滚和未结束事务；本函数收集记录，不提前决定是否重放。
// 旧用例的检查点大于全部写入 ID，而且只遍历实际结果，返回空也会通过。
TEST_F(WALTest, RecoverTest) {
  const auto create7 = Record::createRecord(7);
  const auto put7 = Record::putRecord(7, "key", std::string(300, 'v'));
  const auto delete7 = Record::deleteRecord(7, "key");
  const auto rewrite7 = Record::putRecord(7, "key", "last");
  const auto commit7 = Record::commitRecord(7);
  const auto create8 = Record::createRecord(8);
  const auto put8 = Record::putRecord(8, "cancelled", "value");
  const auto rollback8 = Record::rollbackRecord(8);
  const auto create9 = Record::createRecord(9);
  const auto put9 = Record::putRecord(9, "unfinished", "value");
  const std::map<uint64_t, std::vector<Record>> expected = {
      {7, {create7, put7, delete7, rewrite7, commit7}},
      {8, {create8, put8, rollback8}},
      {9, {create9, put9}}};

  {
    // 关闭后台清理，避免测试收集日志时引入无关的周期删除行为。
    WAL wal(test_dir, 0, 0, 0, 64);
    wal.log({create7, put7, create8, put8});
    wal.log({delete7, rollback8, create9, put9});
    wal.log({rewrite7, commit7});
  }

  EXPECT_EQ(WAL::recover(test_dir, 0), expected);
}

// 目的：新库没有日志时返回空结果；不因为执行恢复而创建目录或日志。
TEST_F(WALTest, RecoverMissingDirectoryAndEmptyLog) {
  const auto missing = std::filesystem::path(test_dir) / "missing";
  EXPECT_TRUE(WAL::recover(missing.string(), 0).empty());
  EXPECT_FALSE(std::filesystem::exists(missing));
  EXPECT_TRUE(WAL::recover(test_dir, 0).empty());

  const auto empty_log = std::filesystem::path(test_dir) / "wal.0";
  FileObj::create_and_write(empty_log.string(), {});
  EXPECT_TRUE(WAL::recover(test_dir, 0).empty());
  EXPECT_EQ(std::filesystem::file_size(empty_log), 0u);
}

// 目的：按数字序号读取 wal.2、wal.10 和超过 32 位的序号，保持事务内顺序。
// 不读取名字相似的备份文件或目录；事务 ID 同样要保留完整的 64 位。
TEST_F(WALTest, RecoverNumericOrderAndGrouping) {
  const auto dir = std::filesystem::path(test_dir);
  const uint64_t id = (uint64_t{1} << 40) + 7;
  const auto create = Record::createRecord(id);
  const auto first = Record::putRecord(id, "key", "first");
  const auto second = Record::putRecord(id, "key", "second");
  const auto commit = Record::commitRecord(id);
  write_wal_records(dir / "wal.10", {second});
  write_wal_records(dir / "wal.4294967296", {commit});
  write_wal_records(dir / "wal.2", {create, first});

  for (const char *name : {"wal.", "wal.backup", "wal.-1", "wal.1.tmp"})
    FileObj::create_and_write((dir / name).string(), {0, 0});
  std::filesystem::create_directory(dir / "wal.1");

  const std::map<uint64_t, std::vector<Record>> expected = {
      {id, {create, first, second, commit}}};
  EXPECT_EQ(WAL::recover(test_dir, 0), expected);
}

// 目的：检查点为 2 时保留事务 3、4，不能把零散完成的 4 当成连续检查点。
// 4 是否已经刷盘由上层判断；这里也检查恰好等于检查点的事务会被排除。
TEST_F(WALTest, RecoverCheckpointLeavesGapsForUpperLayer) {
  std::vector<Record> records;
  std::map<uint64_t, std::vector<Record>> expected;
  for (uint64_t id = 1; id <= 4; ++id) {
    const auto create = Record::createRecord(id);
    const auto commit = Record::commitRecord(id);
    records.push_back(create);
    records.push_back(commit);
    if (id > 2)
      expected[id] = {create, commit};
  }
  write_wal_records(std::filesystem::path(test_dir) / "wal.0", records);

  EXPECT_EQ(WAL::recover(test_dir, 2), expected);
  EXPECT_TRUE(WAL::recover(test_dir, 4).empty());
}

// 目的：空路径与 WAL 构造函数一致，表示当前目录。
// 测试临时切换工作目录，离开本用例时自动恢复，避免影响后续用例。
TEST_F(WALTest, RecoverEmptyPathUsesCurrentDirectory) {
  struct RestoreDirectory {
    std::filesystem::path path;
    ~RestoreDirectory() { std::filesystem::current_path(path); }
  } restore{std::filesystem::current_path()};

  const auto create = Record::createRecord(7);
  write_wal_records(std::filesystem::path(test_dir) / "wal.0", {create});
  std::filesystem::current_path(test_dir);
  const std::map<uint64_t, std::vector<Record>> expected = {{7, {create}}};
  EXPECT_EQ(WAL::recover("", 0), expected);
}

// 目的：COMMIT 只写了一部分时，保留前面的完整记录，不伪造提交成功。
// 枚举尾记录各个截断位置；旧文件有残缺尾部、后面已有新日志时也能重复读取。
// 每次恢复后核对原始字节，保证没有截断、改写或删除旧日志。
TEST_F(WALTest, RecoverPartialTailsAcrossRepeatedRecovery) {
  const auto dir = std::filesystem::path(test_dir);
  const auto old_path = dir / "wal.2";
  const auto new_path = dir / "wal.10";
  const std::vector<Record> unfinished = {
      Record::createRecord(7), Record::putRecord(7, "key", std::string(300, 'v'))};
  const std::vector<Record> committed = {
      Record::createRecord(8), Record::putRecord(8, "other", "value"),
      Record::commitRecord(8)};
  const std::map<uint64_t, std::vector<Record>> expected = {
      {7, unfinished}, {8, committed}};
  write_wal_records(new_path, committed);

  std::vector<uint8_t> prefix;
  for (const auto &record : unfinished) {
    const auto encoded = record.encode();
    prefix.insert(prefix.end(), encoded.begin(), encoded.end());
  }
  const auto tail = Record::commitRecord(7).encode();
  for (size_t kept = 0; kept < tail.size(); ++kept) {
    SCOPED_TRACE(kept);
    auto bytes = prefix;
    bytes.insert(bytes.end(), tail.begin(), tail.begin() + kept);
    FileObj::create_and_write(old_path.string(), bytes);

    ASSERT_EQ(WAL::recover(test_dir, 0), expected);
    EXPECT_EQ(WAL::recover(test_dir, 0), expected);
    auto old_file = FileObj::open(old_path.string());
    EXPECT_EQ(old_file.size(), bytes.size());
    EXPECT_EQ(old_file.read_to_slice(0, bytes.size()), bytes);
    EXPECT_EQ(read_wal_records(new_path), committed);
  }
}

// 目的：完整记录的非法长度、操作类型和字段长度仍要报错，不能当作尾部缺失。
// 即使该 ID 已被检查点覆盖，也不能让完整记录的格式错误悄悄通过。
TEST_F(WALTest, RecoverRejectsMalformedCompleteRecords) {
  auto short_header = Record::createRecord(7).encode();
  short_header[0] = 10;
  auto unknown_type = Record::createRecord(7).encode();
  unknown_type[10] = 255;
  auto invalid_string = Record::putRecord(7, "key", "value").encode();
  invalid_string[11] = 250;
  const std::vector<std::vector<uint8_t>> malformed = {
      short_header, unknown_type, invalid_string};
  const auto path = std::filesystem::path(test_dir) / "wal.0";

  for (size_t i = 0; i < malformed.size(); ++i) {
    SCOPED_TRACE(i);
    FileObj::create_and_write(path.string(), malformed[i]);
    EXPECT_THROW(WAL::recover(test_dir, 0), std::runtime_error);
    EXPECT_THROW(WAL::recover(test_dir, 7), std::runtime_error);
    auto file = FileObj::open(path.string());
    EXPECT_EQ(file.read_to_slice(0, file.size()), malformed[i]);
  }
}

// 目的：路径指向普通文件、或日志数字序号超过 uint64_t 时，明确报告错误。
// 不把无法识别顺序的日志静默跳过，也不把错误路径当作没有日志的新库。
TEST_F(WALTest, RecoverRejectsOverflowingSequenceAndNonDirectory) {
  const auto dir = std::filesystem::path(test_dir);
  const auto not_dir = dir / "not_a_directory";
  FileObj::create_and_write(not_dir.string(), {});
  EXPECT_THROW(WAL::recover(not_dir.string(), 0),
               std::filesystem::filesystem_error);

  write_wal_records(dir / "wal.18446744073709551616",
                    {Record::createRecord(7)});
  EXPECT_THROW(WAL::recover(test_dir, 0), std::out_of_range);
}

// 管理器日志检查入口：先恢复已保存状态，不初始化新 WAL，避免清理旧日志。
// 本组只验收检查点传递和候选记录返回，实际状态恢复／重放在后续启动阶段验收。
class TranManagerRecoveryTest : public WALTest {};

// 目的：状态文件记录 flushed={2,4} 时，使用检查点 2 而不是最大值 4。
// 3 和 4 都作为候选返回；回滚、未结束事务也保留，交给后续流程判断。
// 重复检查不修改状态文件内容、回滚名单、待处理集合或 next ID。
TEST_F(TranManagerRecoveryTest,
       CheckRecoverUsesSavedCheckpointAndPreservesCandidates) {
  const auto dir = std::filesystem::path(test_dir);
  {
    TranManager manager(test_dir);
    for (uint64_t id = 1; id <= 6; ++id)
      ASSERT_EQ(manager.getNextTransactionId(), id);
    for (uint64_t id = 1; id <= 4; ++id)
      manager.add_ready_to_flush_tranc_id(id, TransactionState::OP_COMMITTED);
    manager.add_flushed_tranc_id(1);
    manager.add_flushed_tranc_id(2);
    manager.add_flushed_tranc_id(4);
    manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  }

  std::vector<Record> records;
  std::map<uint64_t, std::vector<Record>> expected;
  for (uint64_t id = 1; id <= 6; ++id) {
    std::vector<Record> group = {
        Record::createRecord(id),
        Record::putRecord(id, "key" + std::to_string(id), "value")};
    if (id <= 4)
      group.push_back(Record::commitRecord(id));
    else if (id == 5)
      group.push_back(Record::rollbackRecord(id));
    records.insert(records.end(), group.begin(), group.end());
    if (id > 2)
      expected[id] = group;
  }
  write_wal_records(dir / "wal.0", records);

  const auto read_state = [&] {
    auto file = FileObj::open((dir / "tranc_id").string());
    return file.read_to_slice(0, file.size());
  };
  const auto saved_state = read_state();
  TranManager manager(test_dir);
  const auto saved_flushed = manager.get_flushed_tranc_ids();
  ASSERT_EQ(manager.get_checkpoint_tranc_id(), 2u);
  ASSERT_EQ(manager.get_max_flushed_tranc_id(), 4u);
  ASSERT_TRUE(manager.is_aborted(5));
  ASSERT_FALSE(manager.is_aborted(6));

  EXPECT_EQ(manager.check_recover(), expected);
  EXPECT_EQ(manager.check_recover(), expected);
  EXPECT_EQ(manager.get_flushed_tranc_ids(), saved_flushed);
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_FALSE(manager.is_aborted(6));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), records);
  EXPECT_EQ(read_state(), saved_state);

  // 将检查后的内存状态重新编码，确认私有的 ready 集合等也没有变化。
  manager.write_tranc_id_file();
  EXPECT_EQ(read_state(), saved_state);
  EXPECT_EQ(manager.getNextTransactionId(), 7u);
  EXPECT_FALSE(std::filesystem::exists(dir / "wal.1"));
}

// 目的：没有 WAL 时返回空候选，不创建 WAL、不推进检查点，也不消耗事务 ID。
TEST_F(TranManagerRecoveryTest, CheckRecoverWithoutWalLeavesProgressUnchanged) {
  TranManager manager(test_dir);
  for (uint64_t id = 1; id <= 2; ++id) {
    ASSERT_EQ(manager.getNextTransactionId(), id);
    manager.add_ready_to_flush_tranc_id(id, TransactionState::OP_COMMITTED);
    manager.add_flushed_tranc_id(id);
  }

  EXPECT_TRUE(manager.check_recover().empty());
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 2u);
  EXPECT_EQ(manager.getNextTransactionId(), 3u);
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(test_dir) / "wal.0"));
}

// 目的：完整 WAL 记录损坏时继续向调用方报告错误，不伪装成“没有日志”。
// 检查失败也不能修改已保存／内存中的事务状态或原日志文件。
TEST_F(TranManagerRecoveryTest,
       CheckRecoverPropagatesFailureWithoutChangingState) {
  const auto dir = std::filesystem::path(test_dir);
  TranManager manager(test_dir);
  ASSERT_EQ(manager.getNextTransactionId(), 1u);
  manager.add_ready_to_flush_tranc_id(1, TransactionState::ABORTED);
  manager.write_tranc_id_file();
  const auto read_state = [&] {
    auto file = FileObj::open((dir / "tranc_id").string());
    return file.read_to_slice(0, file.size());
  };
  const auto saved_state = read_state();
  auto malformed = Record::createRecord(2).encode();
  malformed[10] = 255;
  FileObj::create_and_write((dir / "wal.0").string(), malformed);

  EXPECT_THROW(manager.check_recover(), std::runtime_error);
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 0u);
  EXPECT_TRUE(manager.is_aborted(1));
  EXPECT_FALSE(manager.is_aborted(2));
  EXPECT_EQ(read_state(), saved_state);
  manager.write_tranc_id_file();
  EXPECT_EQ(read_state(), saved_state);
  EXPECT_EQ(manager.getNextTransactionId(), 2u);
  auto file = FileObj::open((dir / "wal.0").string());
  EXPECT_EQ(file.read_to_slice(0, file.size()), malformed);
}

// 目的：初始化会使用旧文件之后的新序号，旧日志保留，新 WAL 能实际写入。
// 保存启动状态后再初始化，整个过程不能重置刷盘集合或修改事务 ID。
TEST_F(TranManagerRecoveryTest, InitWalPreservesOldLogsAndWritesNewFile) {
  const auto dir = std::filesystem::path(test_dir);
  const std::vector<Record> old2 = {
      Record::createRecord(7), Record::putRecord(7, "old", "value")};
  const std::vector<Record> old10 = {Record::commitRecord(7)};
  write_wal_records(dir / "wal.2", old2);
  write_wal_records(dir / "wal.10", old10);
  TranManager manager(test_dir);
  for (uint64_t id = 1; id <= 7; ++id)
    ASSERT_EQ(manager.getNextTransactionId(), id);
  manager.write_tranc_id_file();

  manager.init_new_wal();
  EXPECT_TRUE(std::filesystem::exists(dir / "wal.11"));
  EXPECT_EQ(read_wal_records(dir / "wal.2"), old2);
  EXPECT_EQ(read_wal_records(dir / "wal.10"), old10);
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 0u);
  EXPECT_EQ(manager.get_max_flushed_tranc_id(), 0u);
  EXPECT_EQ(manager.getNextTransactionId(), 8u);

  const std::vector<Record> fresh = {
      Record::createRecord(8), Record::putRecord(8, "new", "value"),
      Record::commitRecord(8)};
  ASSERT_TRUE(manager.write_to_wal(fresh));
  EXPECT_EQ(read_wal_records(dir / "wal.11"), fresh);
}

// 目的：flushed={2,4} 时，后台清理只能使用连续检查点 2。
// 等待真实清理线程删除覆盖范围内的日志，同时确认事务 3、4、5 的日志仍在。
// 初始化前保存完整状态，清理线程才能依据该检查点安全工作。
TEST_F(TranManagerRecoveryTest, InitWalCleanerUsesSavedContinuousCheckpoint) {
  const auto dir = std::filesystem::path(test_dir);
  TranManager manager(test_dir);
  for (uint64_t id = 1; id <= 6; ++id)
    ASSERT_EQ(manager.getNextTransactionId(), id);
  for (uint64_t id = 1; id <= 4; ++id)
    manager.add_ready_to_flush_tranc_id(id, TransactionState::OP_COMMITTED);
  manager.add_flushed_tranc_id(1);
  manager.add_flushed_tranc_id(2);
  manager.add_flushed_tranc_id(4);
  manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  manager.write_tranc_id_file();
  const auto read_state = [&] {
    auto file = FileObj::open((dir / "tranc_id").string());
    return file.read_to_slice(0, file.size());
  };
  const auto saved_state = read_state();
  const auto saved_flushed = manager.get_flushed_tranc_ids();
  write_wal_records(dir / "wal.0",
                    {Record::createRecord(1), Record::commitRecord(1)});
  const std::vector<Record> pending3 = {
      Record::createRecord(3), Record::commitRecord(3)};
  const std::vector<Record> flushed4 = {
      Record::createRecord(4), Record::commitRecord(4)};
  const std::vector<Record> aborted5 = {
      Record::createRecord(5), Record::rollbackRecord(5)};
  write_wal_records(dir / "wal.1", pending3);
  write_wal_records(dir / "wal.2", flushed4);
  write_wal_records(dir / "wal.3", aborted5);

  manager.init_new_wal();
  ASSERT_TRUE(wait_for_wal_condition([&] {
    return !std::filesystem::exists(dir / "wal.0");
  }));
  EXPECT_EQ(read_wal_records(dir / "wal.1"), pending3);
  EXPECT_EQ(read_wal_records(dir / "wal.2"), flushed4);
  EXPECT_EQ(read_wal_records(dir / "wal.3"), aborted5);
  EXPECT_TRUE(std::filesystem::exists(dir / "wal.4"));
  EXPECT_EQ(manager.get_flushed_tranc_ids(), saved_flushed);
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 2u);
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_EQ(read_state(), saved_state);
  manager.write_tranc_id_file();
  EXPECT_EQ(read_state(), saved_state);
  EXPECT_EQ(manager.getNextTransactionId(), 7u);
}

// 目的：新日志文件创建失败时抛异常，保留旧 WAL 和事务状态，不假装初始化成功。
// 用同名目录占住下一文件路径，触发真实文件打开失败，无需依赖权限或磁盘空间。
TEST_F(TranManagerRecoveryTest, InitWalFileFailurePreservesLogsAndState) {
  const auto dir = std::filesystem::path(test_dir);
  const std::vector<Record> old = {
      Record::createRecord(7), Record::commitRecord(7)};
  write_wal_records(dir / "wal.2", old);
  std::filesystem::create_directory(dir / "wal.3");
  TranManager manager(test_dir);
  for (uint64_t id = 1; id <= 7; ++id)
    ASSERT_EQ(manager.getNextTransactionId(), id);
  manager.write_tranc_id_file();

  EXPECT_THROW(manager.init_new_wal(), std::runtime_error);
  EXPECT_FALSE(manager.write_to_wal({Record::createRecord(8)}));
  EXPECT_EQ(read_wal_records(dir / "wal.2"), old);
  EXPECT_TRUE(std::filesystem::is_directory(dir / "wal.3"));
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 0u);
  EXPECT_EQ(manager.getNextTransactionId(), 8u);
}

// 目的：恢复提交、回滚和未结束三种事务，并让 next ID 越过全部已使用编号。
// 已提交事务只进入待刷盘集合；真正收到刷盘通知之前，检查点不能提前推进。
TEST_F(TranManagerRecoveryTest, RestoreClassifiesTransactionsAndAdvancesId) {
  const auto dir = std::filesystem::path(test_dir);
  TranManager manager(test_dir);
  for (uint64_t id = 1; id <= 2; ++id) {
    ASSERT_EQ(manager.getNextTransactionId(), id);
    manager.add_ready_to_flush_tranc_id(id, TransactionState::OP_COMMITTED);
    manager.add_flushed_tranc_id(id);
  }
  manager.write_tranc_id_file();
  const auto read_state = [&] {
    auto file = FileObj::open((dir / "tranc_id").string());
    return file.read_to_slice(0, file.size());
  };
  const auto saved_state = read_state();
  const std::vector<Record> log = {
      Record::createRecord(3), Record::putRecord(3, "committed", "value"),
      Record::commitRecord(3), Record::createRecord(4),
      Record::putRecord(4, "cancelled", "value"), Record::rollbackRecord(4),
      Record::createRecord(5), Record::putRecord(5, "unfinished", "value")};
  write_wal_records(dir / "wal.0", log);
  const auto records = manager.check_recover();

  manager.restore_recovery_state(records);
  EXPECT_FALSE(manager.is_aborted(3));
  EXPECT_TRUE(manager.is_aborted(4));
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 2u);
  EXPECT_EQ(manager.get_max_flushed_tranc_id(), 2u);
  EXPECT_EQ(manager.getNextTransactionId(), 6u);
  EXPECT_EQ(read_state(), saved_state);
  EXPECT_EQ(read_wal_records(dir / "wal.0"), log);
  EXPECT_EQ(manager.check_recover(), records);

  // 通知 5 时可以结算已取消的 4、5，但提交事务 3 仍等待自己的通知。
  manager.add_flushed_tranc_id(5);
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 2u);
  EXPECT_TRUE(manager.get_flushed_tranc_ids().contains(4));
  EXPECT_TRUE(manager.get_flushed_tranc_ids().contains(5));
  manager.add_flushed_tranc_id(3);
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 5u);
  EXPECT_TRUE(manager.is_aborted(4));
  EXPECT_TRUE(manager.is_aborted(5));
}

// 目的：保留状态文件已有的回滚名单、零散刷盘 ID 和更大的 next ID。
// 重复恢复结果不变；已经结算的 4 不能重新进入待处理表。
TEST_F(TranManagerRecoveryTest, RestorePreservesSavedStateAndIsRepeatable) {
  const auto dir = std::filesystem::path(test_dir);
  TranManager manager(test_dir);
  for (uint64_t id = 1; id <= 7; ++id)
    ASSERT_EQ(manager.getNextTransactionId(), id);
  manager.add_ready_to_flush_tranc_id(1, TransactionState::ABORTED);
  manager.add_ready_to_flush_tranc_id(2, TransactionState::OP_COMMITTED);
  manager.add_flushed_tranc_id(2);
  manager.add_ready_to_flush_tranc_id(4, TransactionState::OP_COMMITTED);
  manager.add_flushed_tranc_id(4);
  manager.add_ready_to_flush_tranc_id(5, TransactionState::ABORTED);
  const auto saved_flushed = manager.get_flushed_tranc_ids();
  write_wal_records(dir / "wal.0",
                    {Record::createRecord(3), Record::commitRecord(3),
                     Record::createRecord(4), Record::commitRecord(4),
                     Record::createRecord(6)});
  const auto records = manager.check_recover();
  const auto read_state = [&] {
    auto file = FileObj::open((dir / "tranc_id").string());
    return file.read_to_slice(0, file.size());
  };

  manager.restore_recovery_state(records);
  manager.write_tranc_id_file();
  const auto restored_state = read_state();
  manager.restore_recovery_state(records);
  manager.write_tranc_id_file();
  EXPECT_EQ(read_state(), restored_state);
  EXPECT_EQ(manager.get_flushed_tranc_ids(), saved_flushed);
  EXPECT_TRUE(manager.is_aborted(1));
  EXPECT_TRUE(manager.is_aborted(5));
  EXPECT_TRUE(manager.is_aborted(6));
  EXPECT_FALSE(manager.is_aborted(3));
  EXPECT_FALSE(manager.is_aborted(4));

  // 如果恢复误把 4 再次登记为 ready，这次通知就会改变状态文件。
  manager.add_flushed_tranc_id(4);
  manager.write_tranc_id_file();
  EXPECT_EQ(read_state(), restored_state);
  EXPECT_EQ(manager.getNextTransactionId(), 8u);
}

// 目的：没有候选日志时保持原有进度和回滚名单，不能重置成“新数据库”。
TEST_F(TranManagerRecoveryTest, RestoreEmptyCandidatesPreservesState) {
  const auto dir = std::filesystem::path(test_dir);
  TranManager manager(test_dir);
  ASSERT_EQ(manager.getNextTransactionId(), 1u);
  manager.add_ready_to_flush_tranc_id(1, TransactionState::ABORTED);
  manager.write_tranc_id_file();
  auto file = FileObj::open((dir / "tranc_id").string());
  const auto saved_state = file.read_to_slice(0, file.size());

  manager.restore_recovery_state({});
  EXPECT_TRUE(manager.is_aborted(1));
  EXPECT_EQ(manager.get_checkpoint_tranc_id(), 0u);
  manager.write_tranc_id_file();
  auto after = FileObj::open((dir / "tranc_id").string());
  EXPECT_EQ(after.read_to_slice(0, after.size()), saved_state);
  EXPECT_EQ(manager.getNextTransactionId(), 2u);
}

// 目的：WAL 中出现 uint64_t 最大 ID 时，不能加一回绕到 0。
// 在登记任何新事务前报错，并保持已有状态和 ID 分配器不变。
TEST_F(TranManagerRecoveryTest, RestoreRejectsIdOverflowBeforeChangingState) {
  const auto dir = std::filesystem::path(test_dir);
  TranManager manager(test_dir);
  ASSERT_EQ(manager.getNextTransactionId(), 1u);
  manager.add_ready_to_flush_tranc_id(1, TransactionState::ABORTED);
  manager.write_tranc_id_file();
  auto file = FileObj::open((dir / "tranc_id").string());
  const auto saved_state = file.read_to_slice(0, file.size());
  const auto max_id = std::numeric_limits<uint64_t>::max();
  write_wal_records(dir / "wal.0", {Record::createRecord(max_id)});
  const auto records = manager.check_recover();

  EXPECT_THROW(manager.restore_recovery_state(records), std::overflow_error);
  EXPECT_TRUE(manager.is_aborted(1));
  EXPECT_FALSE(manager.is_aborted(max_id));
  manager.write_tranc_id_file();
  auto after = FileObj::open((dir / "tranc_id").string());
  EXPECT_EQ(after.read_to_slice(0, after.size()), saved_state);
  EXPECT_EQ(manager.getNextTransactionId(), 2u);
}

// 目的：取消／未结束事务的值和墓碑已经在 SST 中时，恢复名单仍能让它们失效。
// 直接准备存储层记录，验收管理器与 SST 查询的衔接；不冒充 LSM 启动流程已完成。
TEST_F(TranManagerRecoveryTest, RestoreFiltersInvalidVersionsAlreadyInSst) {
  auto engine = std::make_shared<LSMEngine>(test_dir);
  auto manager = std::make_shared<TranManager>(test_dir);
  manager->set_engine(engine);
  engine->set_tran_manager(manager);
  engine->put("changed", "old", 1);
  engine->put("deleted", "old", 1);
  engine->put("unfinished", "old", 1);
  engine->put("changed", "dirty", 3);
  engine->remove("deleted", 3);
  engine->put("new", "dirty", 3);
  engine->put("unfinished", "dirty", 4);
  while (engine->memtable.get_total_size() > 0)
    engine->flush();
  for (const char *key : {"changed", "unfinished"}) {
    const auto result = engine->get(key, 0);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->first, "dirty");
  }

  write_wal_records(std::filesystem::path(test_dir) / "wal.0",
                    {Record::createRecord(3),
                     Record::putRecord(3, "changed", "dirty"),
                     Record::deleteRecord(3, "deleted"),
                     Record::putRecord(3, "new", "dirty"),
                     Record::rollbackRecord(3), Record::createRecord(4),
                     Record::putRecord(4, "unfinished", "dirty")});
  manager->restore_recovery_state(manager->check_recover());

  for (const char *key : {"changed", "deleted", "unfinished"}) {
    SCOPED_TRACE(key);
    const auto result = engine->get(key, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "old");
    EXPECT_EQ(result->second, 1u);
  }
  EXPECT_FALSE(engine->get("new", 0).has_value());
  EXPECT_EQ(engine->memtable.get_total_size(), 0u);
  EXPECT_TRUE(manager->is_aborted(3));
  EXPECT_TRUE(manager->is_aborted(4));
  EXPECT_EQ(manager->getNextTransactionId(), 5u);
}

class LSMStartupWALTest : public WALTest {};

// 目的：批量删除日志写入失败时，所有已有值都保留，不能提前应用前几个墓碑。
// 真实轮换错误之后再次批删也必须失败，不能绕过已经进入故障状态的 WAL。
TEST_F(LSMStartupWALTest, DirectRemoveBatchWalFailurePreservesAllValues) {
  const auto dir = std::filesystem::path(test_dir);
  LSM db(test_dir);
  db.put("a", "old-a");
  db.put("b", "old-b");
  const std::string padding(5000, 'v');
  db.put("padding", padding);
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const auto successful = read_wal_records(dir / "wal.0");
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  const std::vector<std::string> keys{"a", "b", "missing"};
  EXPECT_THROW(db.remove_batch(keys), std::runtime_error);
  EXPECT_EQ(db.get("a"), std::make_optional(std::string("old-a")));
  EXPECT_EQ(db.get("b"), std::make_optional(std::string("old-b")));
  EXPECT_FALSE(db.get("missing"));
  EXPECT_THROW(db.remove_batch({"padding"}), std::runtime_error);
  EXPECT_EQ(db.get("padding"), std::make_optional(padding));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

// 目的：删除日志未确认成功时，不能提前写墓碑遮住旧值。
// 占用下一个 WAL 文件制造真实轮换失败，后续删除也不能绕过 WAL 的故障状态。
TEST_F(LSMStartupWALTest, DirectRemoveWalFailurePreservesExistingValues) {
  const auto dir = std::filesystem::path(test_dir);
  LSM db(test_dir);
  db.put("victim", "old");
  const std::string padding(5000, 'v');
  db.put("padding", padding);
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const auto successful = read_wal_records(dir / "wal.0");
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  EXPECT_THROW(db.remove("victim"), std::runtime_error);
  EXPECT_THROW(db.remove("padding"), std::runtime_error);
  EXPECT_EQ(db.get("victim"), std::make_optional(std::string("old")));
  EXPECT_EQ(db.get("padding"), std::make_optional(padding));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

// 目的：普通 put 也必须先写 WAL；日志轮换失败不能覆盖旧值或写入新 key。
// 填满当前日志后占用下一个文件名，制造真实轮换错误，保留旧日志验证未被覆盖。
TEST_F(LSMStartupWALTest, DirectPutWalFailureDoesNotApplyData) {
  const auto dir = std::filesystem::path(test_dir);
  LSM db(test_dir);
  db.put("key", "before");
  db.put("padding", std::string(5000, 'v'));
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const auto successful = read_wal_records(dir / "wal.0");
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  EXPECT_THROW(db.put("key", "after"), std::runtime_error);
  EXPECT_THROW(db.put("new", "blocked"), std::runtime_error);
  EXPECT_EQ(db.get("key"), std::make_optional(std::string("before")));
  EXPECT_FALSE(db.get("new"));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

// 目的：整批日志未确认写出时，批次中的覆盖和新增都不能提前应用。
// 占用下一个 WAL 文件名制造真实轮换失败，再检查后续批次同样不能绕过故障。
TEST_F(LSMStartupWALTest, DirectPutBatchWalFailureDoesNotApplyAnyEntry) {
  const auto dir = std::filesystem::path(test_dir);
  LSM db(test_dir);
  db.put("kept", "old");
  db.put("padding", std::string(5000, 'v'));
  ASSERT_GT(std::filesystem::file_size(dir / "wal.0"), 4096u);
  const auto successful = read_wal_records(dir / "wal.0");
  const std::vector<Record> occupied = {Record::createRecord(99)};
  write_wal_records(dir / "wal.1", occupied);

  const std::vector<std::pair<std::string, std::string>> batch{
      {"kept", "changed"}, {"new", "blocked"}};
  EXPECT_THROW(db.put_batch(batch), std::runtime_error);
  EXPECT_EQ(db.get("kept"), std::make_optional(std::string("old")));
  EXPECT_FALSE(db.get("new"));
  const std::vector<std::pair<std::string, std::string>> later{
      {"later", "blocked"}};
  EXPECT_THROW(db.put_batch(later), std::runtime_error);
  EXPECT_FALSE(db.get("later"));
  EXPECT_EQ(read_wal_records(dir / "wal.0"), successful);
  EXPECT_EQ(read_wal_records(dir / "wal.1"), occupied);
}

// 目的：遇到完整但非法的 WAL 记录，构造失败，不创建新日志或删除旧日志。
// 不把捕获错误后返回空候选当作一次成功恢复。
TEST_F(LSMStartupWALTest, MalformedWalStopsStartupAndPreservesLogs) {
  const auto dir = std::filesystem::path(test_dir);
  {
    TranManager saved(test_dir);
    saved.write_tranc_id_file();
  }
  auto malformed = Record::createRecord(7).encode();
  malformed[10] = 255;
  FileObj::create_and_write((dir / "wal.0").string(), malformed);

  EXPECT_THROW({ LSM db(test_dir); }, std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(dir / "wal.1"));
  auto file = FileObj::open((dir / "wal.0").string());
  EXPECT_EQ(file.read_to_slice(0, file.size()), malformed);
}

#ifndef _WIN32
// 目的：整批墓碑尚未刷盘就退出进程，恢复后仍能遮住 SST 中的全部旧值。
// 批次包含重复 key 和不存在的 key，WAL 顺序与共同 ID 保持；重启后继续分配新 ID。
TEST_F(LSMStartupWALTest, DirectRemoveBatchSurvivesProcessExitWithoutDestructors) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  const std::vector<std::string> keys{"a", "missing", "a", "b"};
  ASSERT_EXIT(
      {
        try {
          LSM db(test_dir);
          db.put("a", "old-a");
          db.put("b", "old-b");
          db.put("survivor", "kept");
          db.flush_all();
          db.remove_batch(keys);
          std::_Exit(0);
        } catch (...) {
          std::_Exit(94);
        }
      },
      ::testing::ExitedWithCode(0), "");

  const auto records = read_wal_records(
      std::filesystem::path(test_dir) / "wal.0");
  const std::vector<Record> expected = {
      Record::createRecord(1), Record::putRecord(1, "a", "old-a"),
      Record::commitRecord(1), Record::createRecord(2),
      Record::putRecord(2, "b", "old-b"), Record::commitRecord(2),
      Record::createRecord(3), Record::putRecord(3, "survivor", "kept"),
      Record::commitRecord(3), Record::createRecord(4),
      Record::deleteRecord(4, "a"), Record::deleteRecord(4, "missing"),
      Record::deleteRecord(4, "a"), Record::deleteRecord(4, "b"),
      Record::commitRecord(4)};
  ASSERT_EQ(records, expected);

  for (int restart = 0; restart < 2; ++restart) {
    SCOPED_TRACE(restart);
    LSM db(test_dir);
    EXPECT_FALSE(db.get("a"));
    EXPECT_FALSE(db.get("b"));
    EXPECT_FALSE(db.get("missing"));
    EXPECT_EQ(db.get("survivor"), std::make_optional(std::string("kept")));
    if (restart == 0) {
      db.put("after", "restored");
      const std::vector<Record> expected_fresh = {
          Record::createRecord(5), Record::putRecord(5, "after", "restored"),
          Record::commitRecord(5)};
      EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.1"),
                expected_fresh);
    } else {
      EXPECT_EQ(db.get("after"), std::make_optional(std::string("restored")));
    }
  }
}

// 目的：旧值已落入 SST、删除仍在内存时直接退出进程，重启不能让旧值重新出现。
// 同时验证 DELETE 日志、删除不存在的 key、恢复后 ID 延续以及删除后的重新写入。
TEST_F(LSMStartupWALTest, DirectRemoveSurvivesProcessExitWithoutDestructors) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(
      {
        try {
          LSM db(test_dir);
          db.put("victim", "old");
          db.put("survivor", "kept");
          db.flush_all();
          db.remove("victim");
          db.remove("absent");
          std::_Exit(0);
        } catch (...) {
          std::_Exit(93);
        }
      },
      ::testing::ExitedWithCode(0), "");

  const auto records = read_wal_records(
      std::filesystem::path(test_dir) / "wal.0");
  const std::vector<Record> expected = {
      Record::createRecord(1), Record::putRecord(1, "victim", "old"),
      Record::commitRecord(1), Record::createRecord(2),
      Record::putRecord(2, "survivor", "kept"), Record::commitRecord(2),
      Record::createRecord(3), Record::deleteRecord(3, "victim"),
      Record::commitRecord(3), Record::createRecord(4),
      Record::deleteRecord(4, "absent"), Record::commitRecord(4)};
  ASSERT_EQ(records, expected);

  for (int restart = 0; restart < 2; ++restart) {
    SCOPED_TRACE(restart);
    LSM db(test_dir);
    EXPECT_EQ(db.get("survivor"), std::make_optional(std::string("kept")));
    EXPECT_FALSE(db.get("absent"));
    if (restart == 0) {
      EXPECT_FALSE(db.get("victim"));
      db.put("victim", "reborn");
      const std::vector<Record> expected_fresh = {
          Record::createRecord(5), Record::putRecord(5, "victim", "reborn"),
          Record::commitRecord(5)};
      EXPECT_EQ(read_wal_records(std::filesystem::path(test_dir) / "wal.1"),
                expected_fresh);
    } else {
      EXPECT_EQ(db.get("victim"), std::make_optional(std::string("reborn")));
    }
  }
}

// 目的：普通批量写入返回后直接退出进程，恢复仍得到整批内容和重复 key 的最后值。
// 每批只有一个 ID；重启后再次批量写入应使用下一个 ID，并能经过第二次重启。
TEST_F(LSMStartupWALTest, DirectPutBatchSurvivesProcessExitWithoutDestructors) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  const std::vector<std::pair<std::string, std::string>> batch{
      {"a", "first"}, {"b", "kept"}, {"a", "last"}};
  ASSERT_EXIT(
      {
        try {
          LSM db(test_dir);
          db.put_batch(batch);
          std::_Exit(0);
        } catch (...) {
          std::_Exit(92);
        }
      },
      ::testing::ExitedWithCode(0), "");

  const auto records = read_wal_records(
      std::filesystem::path(test_dir) / "wal.0");
  const std::vector<Record> expected = {
      Record::createRecord(1), Record::putRecord(1, "a", "first"),
      Record::putRecord(1, "b", "kept"), Record::putRecord(1, "a", "last"),
      Record::commitRecord(1)};
  ASSERT_EQ(records, expected);

  for (int restart = 0; restart < 2; ++restart) {
    SCOPED_TRACE(restart);
    LSM db(test_dir);
    EXPECT_EQ(db.get("b"), std::make_optional(std::string("kept")));
    if (restart == 0) {
      EXPECT_EQ(db.get("a"), std::make_optional(std::string("last")));
      db.put_batch({{"a", "continued"}, {"c", "created"}});
      const auto fresh = read_wal_records(
          std::filesystem::path(test_dir) / "wal.1");
      const std::vector<Record> expected_fresh = {
          Record::createRecord(2), Record::putRecord(2, "a", "continued"),
          Record::putRecord(2, "c", "created"), Record::commitRecord(2)};
      EXPECT_EQ(fresh, expected_fresh);
    } else {
      EXPECT_EQ(db.get("a"), std::make_optional(std::string("continued")));
      EXPECT_EQ(db.get("c"), std::make_optional(std::string("created")));
    }
  }
}

// 目的：普通 put 返回后，即使进程直接退出、不执行析构刷盘，写入仍可通过 WAL 恢复。
// 同时覆盖同 key 连续更新；重启两次并继续普通写入，确认版本号没有复用旧编号。
TEST_F(LSMStartupWALTest, DirectPutSurvivesProcessExitWithoutDestructors) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(
      {
        try {
          LSM db(test_dir);
          db.put("stable", "value");
          db.put("changed", "old");
          db.put("changed", "new");
          std::_Exit(0);
        } catch (...) {
          std::_Exit(91);
        }
      },
      ::testing::ExitedWithCode(0), "");

  const auto records = read_wal_records(
      std::filesystem::path(test_dir) / "wal.0");
  const std::vector<Record> expected = {
      Record::createRecord(1), Record::putRecord(1, "stable", "value"),
      Record::commitRecord(1), Record::createRecord(2),
      Record::putRecord(2, "changed", "old"), Record::commitRecord(2),
      Record::createRecord(3), Record::putRecord(3, "changed", "new"),
      Record::commitRecord(3)};
  ASSERT_EQ(records, expected);

  for (int restart = 0; restart < 2; ++restart) {
    SCOPED_TRACE(restart);
    LSM db(test_dir);
    EXPECT_EQ(db.get("stable"), std::make_optional(std::string("value")));
    EXPECT_EQ(db.get("changed"), std::make_optional(std::string("new")));
    if (restart == 0) {
      db.put("continued", "after-recovery");
      const auto fresh = read_wal_records(
          std::filesystem::path(test_dir) / "wal.1");
      const std::vector<Record> expected_fresh = {
          Record::createRecord(4),
          Record::putRecord(4, "continued", "after-recovery"),
          Record::commitRecord(4)};
      EXPECT_EQ(fresh, expected_fresh);
    } else {
      EXPECT_EQ(db.get("continued"),
                std::make_optional(std::string("after-recovery")));
    }
  }
}

// 目的：进程通过 _Exit 退出，不执行 LSM／管理器析构，因此不能依赖正常关闭保存状态。
// RU 的修改、墓碑和未结束写入已进入 SST；重启必须结合 WAL 恢复取消状态。
TEST_F(LSMStartupWALTest, ProcessExitRestoresTransactionsWithoutDestructors) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(
      {
        try {
          LSM db(test_dir);
          auto committed = db.begin_tran(IsolationLevel::READ_OP_COMMITTED);
          committed->put("changed", "old");
          committed->put("deleted", "old");
          if (!committed->commit())
            std::_Exit(71);

          auto cancelled = db.begin_tran(IsolationLevel::READ_UNOP_COMMITTED);
          cancelled->put("changed", "dirty");
          cancelled->remove("deleted");
          cancelled->put("new", "dirty");
          db.flush_all();
          if (!cancelled->abort())
            std::_Exit(72);

          auto unfinished = db.begin_tran(IsolationLevel::READ_UNOP_COMMITTED);
          unfinished->put("unfinished", "dirty");
          db.flush_all();
          std::_Exit(0);
        } catch (...) {
          std::_Exit(73);
        }
      },
      ::testing::ExitedWithCode(0), "");

  for (int restart = 0; restart < 2; ++restart) {
    SCOPED_TRACE(restart);
    LSM db(test_dir);
    auto next = db.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    EXPECT_GT(next->tranc_id_, 3u);
    ASSERT_TRUE(next->abort());
    EXPECT_EQ(db.get("changed"), std::make_optional(std::string("old")));
    EXPECT_EQ(db.get("deleted"), std::make_optional(std::string("old")));
    EXPECT_FALSE(db.get("new").has_value());
    EXPECT_FALSE(db.get("unfinished").has_value());
  }
}

// 目的：恢复后的事务状态保存失败时，不能启动新 WAL 或删除仍需要的旧日志。
// 用子进程的文件长度限制制造实际写入／刷新失败，不改变父进程的资源限制。
// 状态文件覆盖失败后的可重试性另行记录，不把“WAL 保留”当作完整故障恢复保证。
TEST_F(LSMStartupWALTest, StateSaveFailurePreservesWalAndStopsInitialization) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  const auto dir = std::filesystem::path(test_dir);
  {
    // 先准备引擎的辅助文件和完整的初始事务状态，再注入文件写失败。
    auto engine = std::make_shared<LSMEngine>(test_dir);
    TranManager saved(test_dir);
    saved.write_tranc_id_file();
  }
  const std::vector<Record> records = {
      Record::createRecord(7), Record::putRecord(7, "survivor", "value"),
      Record::commitRecord(7)};
  write_wal_records(dir / "wal.0", records);
  const auto original_state_size = std::filesystem::file_size(dir / "tranc_id");

  ASSERT_EXIT(
      {
        struct rlimit limits;
        if (getrlimit(RLIMIT_FSIZE, &limits) != 0)
          std::_Exit(81);
        limits.rlim_cur = 16;
        if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR ||
            setrlimit(RLIMIT_FSIZE, &limits) != 0)
          std::_Exit(82);
        try {
          LSM db(test_dir);
        } catch (const std::exception &e) {
          const std::string message = e.what();
          if (message.find("failed to save transaction state") !=
              std::string::npos)
            std::_Exit(0);
          std::_Exit(83);
        }
        std::_Exit(84);
      },
      ::testing::ExitedWithCode(0), "");

  EXPECT_EQ(read_wal_records(dir / "wal.0"), records);
  EXPECT_FALSE(std::filesystem::exists(dir / "wal.1"));
  // 记录本次故障后的文件大小供审计，不把当前的损坏行为写成期望结果。
  RecordProperty("state_bytes_before", std::to_string(original_state_size));
  RecordProperty("state_bytes_after",
                 std::to_string(std::filesystem::file_size(dir / "tranc_id")));
}
#endif

TEST_F(WALTest, PartialFlushRecoveryTest) {
  {
    LSM lsm(test_dir);
    auto tran_ctx1 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    auto tran_ctx2 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    auto tran_ctx3 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    auto tran_ctx4 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);

    tran_ctx4->put("key0", "value0");

    tran_ctx2->put("key1", "value1");
    tran_ctx2->put("key2", "value2");
    // 提交并刷新wal
    tran_ctx2->commit();
    tran_ctx4->commit();
    tran_ctx1->put("key3", "value3");

    tran_ctx3->put("key4", "value4");
    // 模拟写入到WAL后系统崩溃
    tran_ctx1->commit(true);
  }
  {
    LSM lsm(test_dir);
    EXPECT_EQ(lsm.get("key0").value(), "value0");
    EXPECT_EQ(lsm.get("key1").value(), "value1");
    EXPECT_EQ(lsm.get("key2").value(), "value2");
    EXPECT_EQ(lsm.get("key3").value(), "value3");
    EXPECT_FALSE(lsm.get("key4").has_value());
  }
}

TEST_F(WALTest, ConcurrentTransactionsTest) {
  {
    LSM lsm(test_dir);
    std::vector<std::thread> threads;
    auto global_ctx = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    for (int j = 0; j < 10; ++j) {
      global_ctx->put("key" + std::to_string(-1) + "-" + std::to_string(j),
                "value" + std::to_string(j));
    }
    for (int i = 0; i < 40; ++i) {
      threads.emplace_back([&, i]() {
        auto ctx = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
        for (int j = 0; j < 1000; ++j) {
          ctx->put("key" + std::to_string(i) + "-" + std::to_string(j),
                   "value" + std::to_string(j));
        }
        ctx->commit();
      });
    }
    for (auto& t : threads) t.join();
    lsm.flush_all();
    for (int j = 10; j < 20; ++j) {
      global_ctx->put("key" + std::to_string(-1) + "-" + std::to_string(j),
                "value" + std::to_string(j));
    }
    global_ctx->commit(true);
  }

  // 崩溃恢复后验证
  
  {
    LSM lsm(test_dir);
    for (int i = 0; i < 40; ++i) {
      for (int j = 0; j < 100; ++j) {
        auto val = lsm.get("key" + std::to_string(i) + "-" + std::to_string(j));
        EXPECT_TRUE(val.has_value());
        EXPECT_EQ(val.value(), "value" + std::to_string(j));
      }
    }
    for (int j = 0; j < 20; ++j) {
      auto val = lsm.get("key" + std::to_string(-1) + "-" + std::to_string(j));
      EXPECT_TRUE(val.has_value());
      EXPECT_EQ(val.value(), "value" + std::to_string(j));
    }
  }
}

TEST_F(WALTest, HighConcurrencyWithAborts) {
  // 测试高并发下混合提交和abort
  const int THREAD_COUNT = 20;
  const int OPS_PER_THREAD = 100;
  std::atomic<int> commit_count{0};
  std::atomic<int> abort_count{0};
  
  {
    LSM lsm(test_dir);
    std::vector<std::thread> threads;
    
    for (int i = 0; i < THREAD_COUNT; i++) {
      threads.emplace_back([&, i] {
        auto ctx = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
        bool do_abort = (i % 5 == 0);  // 20%的事务abort
        
        for (int j = 0; j < OPS_PER_THREAD; j++) {
          std::string key = "k" + std::to_string(i) + "_" + std::to_string(j);
          ctx->put(key, "v" + std::to_string(j));
          
          // 随机插入abort
          if (j == OPS_PER_THREAD / 2 && do_abort) {
            ctx->abort();
            abort_count++;
            return;
          }
        }
        
        if (do_abort) {
          ctx->abort();
          abort_count++;
        } else {
          ctx->commit();
          commit_count++;
        }
      });
    }
    
    for (auto& t : threads) t.join();
  }

  // 恢复验证
  LSM recovered_lsm(test_dir);
  int recovered_count = 0;
  
  // 验证提交的事务存在，abort的事务不存在
  for (int i = 0; i < THREAD_COUNT; i++) {
    bool should_exist = (i % 5 != 0);  // 非abort线程
    
    for (int j = 0; j < OPS_PER_THREAD; j++) {
      std::string key = "k" + std::to_string(i) + "_" + std::to_string(j);
      auto result = recovered_lsm.get(key);
      
      if (should_exist) {
        EXPECT_TRUE(result.has_value()) << "Missing key: " << key;
        EXPECT_EQ(result.value(), "v" + std::to_string(j));
        if (result.has_value()) recovered_count++;
      } else {
        EXPECT_FALSE(result.has_value()) << "Aborted key present: " << key;
      }
    }
  }
  
  EXPECT_EQ(recovered_count, commit_count * OPS_PER_THREAD);
}

TEST_F(WALTest, MixedTransactionCommitAbortAndCrashRecovery) {
  {
    LSM lsm(test_dir);

    auto t1 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    t1->put("k1", "v1");
    t1->commit();  // 提交

    auto t2 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    t2->put("k2", "v2");
    t2->abort();  // 中止

    auto t3 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    // 未提交，也未中止
    t3->put("k3", "v3");

    auto t5 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    t5->put("k5", "v5");
    t5->commit();


    auto t6 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    t6->put("k6", "v6");
    t6->commit();

    auto t4 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    t4->put("k4", "v4");
    t4->commit();

    auto t7 = lsm.begin_tran(IsolationLevel::READ_OP_COMMITTED);
    t7->put("k7", "v7");
    t7->commit();

    // 崩溃，t3未提交，t1、t4~t7已提交，t2已中止
  }

  // 模拟系统重启后的恢复
  LSM lsm(test_dir);

  // t1、t4~t7 的数据应恢复
  EXPECT_EQ(lsm.get("k1").value(), "v1");
  EXPECT_EQ(lsm.get("k4").value(), "v4");
  EXPECT_EQ(lsm.get("k5").value(), "v5");
  EXPECT_EQ(lsm.get("k6").value(), "v6");
  EXPECT_EQ(lsm.get("k7").value(), "v7");

  // t2中止，t3未提交，应该被丢弃
  EXPECT_FALSE(lsm.get("k2").has_value());
  EXPECT_FALSE(lsm.get("k3").has_value());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
