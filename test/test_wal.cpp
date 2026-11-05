#include "logger/logger.h"
#include "wal/record.h"
#include "lsm/engine.h"
#include "wal/wal.h"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace testing;
using namespace ::tiny_lsm;

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

TEST_F(WALTest, RecoverTest) {
  std::map<uint64_t, std::vector<Record>> expected;

  {
    MockWAL wal(test_dir, 10, 100);

    std::vector<Record> records;
    int record1_len = 50;

    uint64_t tranc_id = 0;
    for (int i = 0; i < record1_len; i++) {
      auto tranc_str = std::to_string(tranc_id);
      auto key = tranc_str + "-key-" + std::to_string(i);
      if (i == 0) {
        auto rec = Record::createRecord(tranc_id);
        records.push_back(rec);
        expected[tranc_id].push_back(rec);
      } else if (i % 10 == 0 || i == record1_len - 1) {
        // 每10条记录或者最后一条记录提交一次
        auto rec = Record::commitRecord(tranc_id);
        records.push_back(rec);
        expected[tranc_id].push_back(rec);
        tranc_id++;
      } else {
        auto value = tranc_str + "-value-" + std::to_string(i);
        auto rec = Record::putRecord(tranc_id, key, value);
        records.push_back(rec);
        expected[tranc_id].push_back(rec);
      }
      if (i % 20 == 0) {
        // 每20条记录写入一次
        wal.log(records);
        records.clear();
      }
    }
    if (!records.empty()) {
      wal.log(records);
    }
  }

  // 恢复
  auto tranc_records = WAL::recover(test_dir, 100);
  // 校验 Record
  uint64_t tranc_id = 0;
  for (auto &[tranc_id, records] : tranc_records) {
    EXPECT_EQ(records, expected[tranc_id]);
  }
}

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
