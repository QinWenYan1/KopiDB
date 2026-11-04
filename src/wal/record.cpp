// src/wal/record.cpp

#include "wal/record.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
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
  constexpr size_t fixed_length = sizeof(uint16_t) + sizeof(uint64_t) +
                                  sizeof(uint8_t) + sizeof(uint16_t) +
                                  sizeof(uint16_t);

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
  constexpr size_t max_length = std::numeric_limits<uint16_t>::max();

  // key 的长度需要写入 uint16_t 字段。
  // 先检查字段长度，再计算整条记录的长度。
  if (key.size() > max_length)
    throw std::length_error(
        "Record::deleteRecord: key exceeds uint16_t length");

  // DELETE 只包含公共头、key 长度和 key 内容
  // 不需要 value 长度字段，因此固定开销为 13B
  constexpr size_t fixed_length =
      sizeof(uint16_t) + sizeof(uint64_t) + sizeof(uint8_t) + sizeof(uint16_t);

  const size_t total_length = fixed_length + key.size();

  // 总长度字段也只有 uint16_t，必须连同固定开销一起检查
  // 例如 key 占 65523B 时，总长为 65536B，已经无法表示
  if (total_length > max_length)
    throw std::length_error(
        "Record::deleteRecord: total record length exceeds uint16_t");

  Record record;
  record.tranc_id_ = tranc_id;
  record.operation_type_ = OperationType::OP_DELETE;

  // 保存待删除 key 的独立副本；value_ 默认保持为空
  record.key_ = key;

  // 已确认总长度能够表示，此时转换不会截断
  record.record_len_ = static_cast<uint16_t>(total_length);

  return record;
}

std::vector<uint8_t> Record::encode() const {
  std::vector<uint8_t> data; 

  // 创建记录时已经计算好总长度，一次预留足够的容量
  // reserve 不改变 size；后面的 push_back/insert 才真正追加字节
  data.reserve(record_len_); 

  // 将 value 拆成 byte_count 个字节，依次追加到 data
  // byte_count 决定写多少字节：长度字段写 2 字节，事务 ID 写 8 字节
  //
  // 例如 value 的末尾是：00010010 00110100，byte_count = 2
  // 第一次取出最右边的 00110100，写入数组，然后将 value 右移 8 位
  // 第二次取出剩下的   00010010，写入数组
  // 最终数组中的顺序是：[00110100] [00010010]
  auto append_uint = [&data](uint64_t value, size_t byte_count){
    for (size_t i = 0; i < byte_count; ++i){
      // 掩码末尾的八个 1 保留 value 最右边的 8 位
      // 其他位置清零，再将得到的这一个字节写入数组
      data.push_back(static_cast<uint8_t>(value & 0xffu)); 

      // 将刚才已经写完的 8 位移出去，让下一个字节来到最右边
      value >>= 8; 
    }
  }; 

  // 字符串先写 2 字节长度，再追加全部内容
  // 创建函数已检查长度可以用 uint16_t 表示
  // 按 begin/end 复制会保留内嵌空字节，不依赖字符串结束符
  // 因为 insert() 读取的是字符串中的每个 char，而 char 可以转换成 uint8_t。
  // 源容器和目标容器不需要类型相同
  auto append_string = [&data, &append_uint](const std::string &text){
    append_uint(static_cast<uint16_t>(text.size()), sizeof(uint16_t)); 
    data.insert(data.end(), text.begin(), text.end()); 
  }; 

  // 所有记录都有公共头：总长度、事务 ID、操作类型
  // 操作类型固定占 1 字节，不使用枚举在内存中的大小
  append_uint(record_len_, sizeof(uint16_t)); 
  append_uint(tranc_id_, sizeof(uint64_t));
  data.push_back(static_cast<uint8_t>(operation_type_)); 

  // PUT 和 DELETE 都需要指出操作的是哪个 key
  // CREATE、COMMIT、ROLLBACK 只有公共头，到这里已经写完
  if (operation_type_ == OperationType::OP_PUT || operation_type_ == OperationType::OP_DELETE)
    append_string(key_); 

  // 只有 PUT 携带 value；DELETE 连 value 的长度字段也不写
  if (operation_type_ == OperationType::OP_PUT)
    append_string(value_); 

  return data; 
}

std::vector<Record> Record::decode(const std::vector<uint8_t> &data) {
  std::vector<Record> records; 
  size_t pos = 0; // 下一次要读去的字节下标
  constexpr size_t header_size = sizeof(uint16_t) + sizeof(uint64_t) +sizeof(uint8_t);

  // 从 pos 开始读取 byte_count 个字节，还原成一个整数
  // end 表示允许读取范围的结束位置：最多只能读到 end - 1
  auto read_uint = [&data, &pos](size_t byte_count, size_t end){
    // 前面的读取始终保证 pos 不超过 end
    // 如果剩余字节不够，就不能继续访问 data[pos]
    if (byte_count > end - pos)
      throw std::runtime_error("Record::decode: truncated integer");

    uint64_t value = 0; 
    for (size_t i = 0; i < byte_count; ++i){
      // 小端编码的第一个字节放在最低 8 位，第二个字节左移 8 位……
      // 例如 [00110100][00010010]，拼回 00010010 00110100
      // 先转成 uint64_t 再左移，才能正确放入事务 ID 的高位
      value |= static_cast<uint64_t>(data[pos++]) << (8 * i); 
    }
    return value;
  }; 

  // 读取一个字符串：先读 2B 长度，再按长度复制内容
  // 两次读取都必须限制在当前记录内部
  auto read_string = [&data, &pos, &read_uint](size_t record_end){
    const size_t length = static_cast<size_t>(read_uint(sizeof(uint16_t), record_end)); 

    // 即使 data 后面还有字节，也不能越过当前记录的结束位置
    if (length > record_end - pos)
      throw std::runtime_error("Record::decode: truncated string");

    std::string text(reinterpret_cast<const char*>(data.data() + pos), length); 
    pos += length; 
    return text; 
  }; 

  while (pos < data.size()){
    const size_t record_start = pos; 

    // 此时还不知道本条记录多长，先以整个 data 的末尾为边界
    // 读取开头的 2 字节总长度；连这 2 字节都不够时会直接报错
    const uint16_t record_len = static_cast<uint16_t>(read_uint(sizeof(uint16_t), data.size()));
    
    // 一条记录至少要包含 11B 公共头
    // 声明的长度也不能超过从本条起点到 data 末尾的剩余字节
    if (record_len < header_size || record_len > data.size() - record_start) {
      throw std::runtime_error("Record::decode: invalid record length");
    }

    // 例如本条从下标 20 开始、长 14B，则只能读取下标 20～33
    // 下标 34 已经属于下一条记录，不能拿来填补本条缺失的字段
    const size_t record_end = record_start + record_len; 

    Record record; 
    record.record_len_ = record_len; 
    record.tranc_id_ = read_uint(sizeof(uint64_t), record_end); 
    record.operation_type_ = static_cast<OperationType>(read_uint(sizeof(uint8_t), record_end));

    // 根据类型读取对应字段，顺序必须与 encode 一致
    switch (record.operation_type_) {
    case OperationType::OP_CREATE:
    case OperationType::OP_COMMIT:
    case OperationType::OP_ROLLBACK:
      // 三种状态记录只有公共头，key/value 保持默认空字符串。
      break; 

    case OperationType::OP_PUT:
      record.key_ = read_string(record_end); 
      record.value_ = read_string(record_end); 
      break; 

    case OperationType::OP_DELETE:
      record.key_ = read_string(record_end); 
      break; 
    
    default:
      throw std::runtime_error("Record::decode: record length does not match payload");
    }

    // 将字符串等内容移动进结果，避免再复制一份 key/value
    // 此时 pos 已指向下一条记录的起点
    records.push_back(std::move(record)); 
  }

  // 空输入不会进入循环，自然返回空数组
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