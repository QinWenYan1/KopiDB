// src/wal/record.cpp

#include "wal/record.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tiny_lsm {

// Lab 5.3 实现创建事务的Record
// OP_CREATE 表示“事务开始”这类记录，可以理解为 BEGIN
// 名字里的 CREATE 指的是创建事务
// OP_CREATE  ID=7                  // 事务 7 开始
// OP_PUT     ID=7  key=a  value=1  // 事务 7 写入 a=1
// OP_COMMIT  ID=7                  // 事务 7 提交
Record Record::createRecord(uint64_t tranc_id) {
  // 事务开始记录只描述“哪个事务开始了”，不携带 key/value
  // 两个 std::string 成员默认构造为空，不需要另外赋值
  Record record;

  // 保存调用方已经分配好的事务 ID
  // 后续 PUT、DELETE、COMMIT 等记录用同一个 ID 关联到这个事务
  record.tranc_id_ = tranc_id;
  record.operation_type_ = OperationType::OP_CREATE;

  // record_len_ 表示编码后的总字节数，包含长度字段自身：
  // record_len(2B) + tranc_id(8B) + operation_type(1B) = 11B
  //
  // 操作类型编码时固定保存为 uint8_t，因此这里按 1 字节计算
  // 而不是使用 OperationType 枚举在内存中占用的大小
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  return record;
}

// Lab 5.3 实现提交事务的Record
Record Record::commitRecord(uint64_t tranc_id) {
  // 提交记录描述“提交哪一笔事务”，不携带具体的 key/value
  // 两个字符串成员默认构造为空
  Record record;

  // 使用与该事务的 CREATE、PUT、DELETE 记录相同的 ID
  // 后续读取日志时，才能知道这条提交记录对应哪些操作
  record.tranc_id_ = tranc_id;
  record.operation_type_ = OperationType::OP_COMMIT;

  // COMMIT 只有公共头，没有 key/value 负载
  // 编码总长度包含长度字段自身：
  // record_len(2B) + tranc_id(8B) + operation_type(1B) = 11B
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  return record;
}

// Lab 5.3 实现回滚事务的Record
Record Record::rollbackRecord(uint64_t tranc_id) {
  // 回滚记录说明“取消哪一笔事务”，不携带具体的 key/value
  // 两个字符串成员默认构造为空
  Record record;

  // 保存被取消事务的 ID，使这条回滚记录能够关联到
  // 同一事务之前的 CREATE、PUT、DELETE 等记录
  record.tranc_id_ = tranc_id;
  record.operation_type_ = OperationType::OP_ROLLBACK;

  // ROLLBACK 与 CREATE、COMMIT 一样，只包含公共头
  // 总长度包含长度字段自身：
  // record_len(2B) + tranc_id(8B) + operation_type(1B) = 11B
  record.record_len_ = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  return record;
}

// Lab 5.3 实现插入键值对的Record
Record Record::putRecord(uint64_t tranc_id, const std::string &key,
                         const std::string &value) {
  constexpr size_t max_length = std::numeric_limits<uint16_t>::max();

  // key_len 和 value_len 都用 uint16_t 编码
  // 先限制各字段长度，再计算总长，避免超大输入在相加时发生溢出
  if (key.size() > max_length || value.size() > max_length)
    throw std::length_error(
        "Record::putRecord: key or value exceeds uint16_t length");

  // 固定部分包括：总长度、事务 ID、操作类型、key 长度、value 长度
  // key/value 的内容另行计入；总长度字段自身也包含在内
  constexpr size_t fixed_length =  
    sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t) +
    sizeof(uint16_t) + sizeof(uint16_t);
  
  const size_t total_length = fixed_length + key.size() + value.size(); 

  // 即使 key/value 各自能装进长度字段，整条记录仍可能超过 65535B
  // 必须在转成 uint16_t 之前拒绝，否则编码时会使用错误的总长度
  if (total_length > max_length)
    throw std::length_error(
        "Record::putRecord: total record length exceeds uint16_t");

  Record record; 
  record.tranc_id_ = tranc_id; 
  record.operation_type_ = OperationType::OP_PUT;
  
  // Record 保存自己的字符串副本，调用方之后修改原字符串不会影响它
  // std::string 赋值按完整长度复制，也能保留内容中的 '\\0'
  record.key_ = key; 
  record.value_ = value; 

  // 已确认总长度可以表示，此时转换不会截断
  record.record_len_ = static_cast<uint16_t>(total_length); 
  return record; 
  
}

// Lab 5.3 实现删除键值对的Record
Record Record::deleteRecord(uint64_t tranc_id, const std::string &key) {
  
}

std::vector<uint8_t> Record::encode() const {
  std::vector<uint8_t> record;

  size_t key_offset = sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t);
  // 记录长度本身(16) + 事务id(64) +
  // 操作类型(8), 固有的编码部分

  record.resize(record_len_, 0);

  // 编码 record_len
  std::memcpy(record.data(), &record_len_, sizeof(uint16_t));

  // 编码 tranc_id
  std::memcpy(record.data() + sizeof(uint16_t), &tranc_id_, sizeof(uint64_t));

  // 编码 operation_type
  auto type_byte = static_cast<uint8_t>(operation_type_);
  std::memcpy(record.data() + sizeof(uint16_t) + sizeof(uint64_t), &type_byte,
              sizeof(uint8_t));

  if (this->operation_type_ == OperationType::OP_PUT) {
    uint16_t key_len = key_.size();
    std::memcpy(record.data() + key_offset, &key_len, sizeof(uint16_t));
    std::memcpy(record.data() + key_offset + sizeof(uint16_t), key_.data(),
                key_.size());

    size_t value_offset = key_offset + sizeof(uint16_t) + key_.size();

    uint16_t value_len = value_.size();
    std::memcpy(record.data() + value_offset, &value_len, sizeof(uint16_t));
    std::memcpy(record.data() + value_offset + sizeof(uint16_t), value_.data(),
                value_.size());
  } else if (this->operation_type_ == OperationType::OP_DELETE) {
    uint16_t key_len = key_.size();
    std::memcpy(record.data() + key_offset, &key_len, sizeof(uint16_t));
    std::memcpy(record.data() + key_offset + sizeof(uint16_t), key_.data(),
                key_.size());
  }

  return record;
}

std::vector<Record> Record::decode(const std::vector<uint8_t> &data) {
  if (data.size() < sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t)) {
    return {};
  }

  std::vector<Record> records;
  size_t pos = 0;

  while (pos < data.size()) {
    // 读取 record_len
    uint16_t record_len;
    std::memcpy(&record_len, data.data() + pos, sizeof(uint16_t));
    pos += sizeof(uint16_t);

    // 检查数据长度是否足够
    if (data.size() < record_len) {
      throw std::runtime_error("Data length does not match record length");
    }

    // 读取 tranc_id
    uint64_t tranc_id;
    std::memcpy(&tranc_id, data.data() + pos, sizeof(uint64_t));
    pos += sizeof(uint64_t);

    // 读取 operation_type
    uint8_t op_type = data[pos++];
    OperationType operation_type = static_cast<OperationType>(op_type);

    Record record;
    record.tranc_id_ = tranc_id;
    record.operation_type_ = operation_type;
    record.record_len_ = record_len;

    if (operation_type == OperationType::OP_PUT) {
      // 读取 key_len
      uint16_t key_len;
      std::memcpy(&key_len, data.data() + pos, sizeof(uint16_t));
      pos += sizeof(uint16_t);

      // 读取 key
      record.key_ = std::string(
          reinterpret_cast<const char *>(data.data() + pos), key_len);
      pos += key_len;

      // 读取 value_len
      uint16_t value_len;
      std::memcpy(&value_len, data.data() + pos, sizeof(uint16_t));
      pos += sizeof(uint16_t);

      // 读取 value
      record.value_ = std::string(
          reinterpret_cast<const char *>(data.data() + pos), value_len);
      pos += value_len;
    } else if (operation_type == OperationType::OP_DELETE) {
      // 读取 key_len
      uint16_t key_len;
      std::memcpy(&key_len, data.data() + pos, sizeof(uint16_t));
      pos += sizeof(uint16_t);

      // 读取 key
      record.key_ = std::string(
          reinterpret_cast<const char *>(data.data() + pos), key_len);
      pos += key_len;
    }

    records.push_back(record);
  }
  return records;
}
void Record::print() const {
  std::cout << "Record: tranc_id=" << tranc_id_
            << ", operation_type=" << static_cast<int>(operation_type_)
            << ", key=" << key_ << ", value=" << value_ << std::endl;
}

bool Record::operator==(const Record &other) const {
  if (tranc_id_ != other.tranc_id_ ||
      operation_type_ != other.operation_type_) {
    return false;
  }

  // 不需要 key 和 value 比较的情况
  if (operation_type_ == OperationType::OP_CREATE ||
      operation_type_ == OperationType::OP_COMMIT ||
      operation_type_ == OperationType::OP_ROLLBACK) {
    return true;
  }

  // 需要 key 比较的情况
  if (operation_type_ == OperationType::OP_DELETE) {
    return key_ == other.key_;
  }

  // 需要 key 和 value 比较的情况
  return key_ == other.key_ && value_ == other.value_;
}

bool Record::operator!=(const Record &other) const { return !(*this == other); }
} // namespace tiny_lsm