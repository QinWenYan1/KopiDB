// include/utils/bloom_filter.cpp

#include "utils/bloom_filter.h"
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace tiny_lsm {

BloomFilter::BloomFilter(){};

// 构造函数，初始化布隆过滤器
// expected_elements: 预期插入的元素数量
// false_positive_rate: 允许的假阳性率
// 如：BloomFilter bf(1000, 0.01);
BloomFilter::BloomFilter(size_t expected_elements, double false_positive_rate)
    : expected_elements_(expected_elements),
      false_positive_rate_(false_positive_rate) {

  // 1. 检查参数:
  //    元素数量必须大于 0，后续计算需要用它作为除数
  if (expected_elements_ == 0)
    throw std::invalid_argument(
        "BloomFilter: expected_elements must be greater than 0");

  // 误判率必须在 (0, 1) 内；这种写法也能排除 NaN
  if (!(0.0 < (false_positive_rate_) && false_positive_rate_ < 1.0))
    throw std::invalid_argument(
        "BloomFilter: false_positive_rate must be between 0 and 1");

  // 2. 根据公式计算需要的位数：
  //    m = -n * ln(p) / (ln(2) * ln(2))
  //    n 是预计元素数量，p 是目标误判率
  const double ln2 = std::log(2);
  const double m = -static_cast<double>(expected_elements_) *
                   std::log(false_positive_rate_) / (ln2 * ln2);

  // 位数必须是整数，向上取整，避免分配得比公式要求的少
  const double round_bits = std::ceil(m);

  // 转成 size_t 前保守检查容量上界，避免越界转换
  // size() 和 max_size() 不是一回事
  // max_size = 容器理论上最多能容纳的元素数量，通常很大
  if (!std::isfinite(round_bits) ||
      round_bits >= static_cast<double>(bits_.max_size()))
    throw std::length_error("BloomFilter: requested bit array is too large");

  num_bits_ = static_cast<size_t>(round_bits);

  // 3. 根据公式计算哈希位置的数量：
  //    k = (m / n) * ln(2)
  //    沿用参考实现的向上取整方式
  num_hashes_ = static_cast<size_t>(
      std::ceil(m / static_cast<double>(expected_elements_) * ln2));

  // 4. 创建位数组：
  //    尚未插入任何 key，所以所有位都为 0
  //    =：通常是拿另一个完整的 vector / initializer_list 赋值
  //    assign()：更灵活，可以从迭代器区间赋值，或者“重复 N 个值”
  bits_.assign(num_bits_, false);
}

// Lab 4.9: 添加一个记录到布隆过滤器中
//          add(key) :
//          → bits_[3] = true
//          → bits_[7] = true
//          → bits_[1] = true
void BloomFilter::add(const std::string &key) {
  // 一个 key 需要计算 num_hashes_ 个哈希位置
  // i 从 0 开始，表示当前计算第几个位置
  for (size_t i = 0; i < num_hashes_; ++i) {
    // hash() 内部已经取模，返回合法的位数组下标
    const size_t bit_idx = hash(key, i);

    // 将对应位设为 1，其余位置保持原样
    // 即使这一位已经是 1，再次赋值也没有问题
    bits_[bit_idx] = true;
  }
}

//  如果key可能存在于布隆过滤器中，返回true；否则返回false
bool BloomFilter::possibly_contains(const std::string &key) const {
  // 对每个哈希函数计算哈希值，检查对应位置的位是否都为true
  for (size_t i = 0; i < num_hashes_; ++i) {
    auto bit_idx = hash(key, i);
    if (!bits_[bit_idx]) {
      return false;
    }
  }
  return true;
}

// 清空布隆过滤器
void BloomFilter::clear() { bits_.assign(bits_.size(), false); }

size_t BloomFilter::hash1(const std::string &key) const {
  std::hash<std::string> hasher;
  return hasher(key);
}

size_t BloomFilter::hash2(const std::string &key) const {
  std::hash<std::string> hasher;
  return hasher(key + "salt");
}

// Lab 4.9: 计算哈希值
//  根据 key 和哈希序号 idx，算出位数组中的一个下标
//  注意：不同 idx 算出的位置也可能重复，公式不保证下标互不相同
//  插入和查询使用相同的计算方式即可
//  h1 = 3，h2 = 4，位数组长度 = 10
//  idx = 0 → (3 + 0 × 4) % 10 = 3
//  idx = 1 → (3 + 1 × 4) % 10 = 7
//  idx = 2 → (3 + 2 × 4) % 10 = 1
//  num_hashes_ 决定调用几次 hash，idx 决定这一次计算第几个位置
size_t BloomFilter::hash(const std::string &key, size_t idx) const {
  // 同一个 key 对应两个基础哈希值
  const size_t h1 = hash1(key);
  const size_t h2 = hash2(key);

  // idx 表示第几个哈希位置，从 0 开始
  // 通过 h1 + idx * h2 组合出不同序号对应的哈希值
  // 不需要手动编写 num_hashes_ 个哈希函数
  //
  // 对位数组长度取模，保证最终下标在 [0, num_bits_) 内
  // 前提是过滤器已经正确初始化，num_bits_ > 0
  return (h1 + idx * h2) % num_bits_;
}

// 编码布隆过滤器为 std::vector<uint8_t>
// [预计元素数量][误判率][位数][哈希数量][打包后的位数组]
std::vector<uint8_t> BloomFilter::encode() {
  // 默认构造的对象还没有位数组，不能直接编码
  // 先检查 vector，避免读取尚未初始化的数值成员
  if (bits_.empty())
    throw std::logic_error("BloomFilter::encode: filter is not initialized");

  // [预计元素数量][误判率][位数][哈希数量]
  // 头部包含三个 size_t 和一个 double
  const size_t header_size = 3 * sizeof(size_t) + sizeof(double);

  // [打包后的位数组]
  // 每 8 位占一个字节，不足 8 位也需要一个字节
  // 这样计算也避免了 (num_bits_ + 7) 可能发生的加法溢出
  const size_t num_bytes = num_bits_ / 8 + (num_bits_ % 8 != 0);

  // 一次分配全部空间，初始字节全部为 0
  std::vector<uint8_t> data(header_size + num_bytes, 0);
  size_t off = 0;

  // 1. 按固定顺序写入四个元数据字段
  //    每写入一个字段，offset 就前进该字段占用的字节数
  std::memcpy(data.data() + off, &expected_elements_, sizeof(size_t));
  off += sizeof(expected_elements_);

  std::memcpy(data.data() + off, &false_positive_rate_, sizeof(double));
  off += sizeof(false_positive_rate_);

  std::memcpy(data.data() + off, &num_bits_, sizeof(size_t));
  off += sizeof(num_bits_);

  std::memcpy(data.data() + off, &num_hashes_, sizeof(size_t));
  off += sizeof(num_hashes_);

  // 2. 打包位数组。此时 offset 指向位数组数据的起点
  //    i / 8：这一位属于第几个字节
  //    i % 8：这一位位于该字节内的哪个位置，从最低位开始
  //    1u：无符号整数 1
  //    i % 8：确定这一位在字节内的位置，范围是 0～7
  //    <<：向左移动指定的位数，右边补 0
  //    1u << (i % 8) 的作用是：生成一个只有目标位是 1、其他位都是 0 的数
  for (size_t i = 0; i < num_bits_; ++i) {
    if (bits_[i])
      data[off + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
  }

  return data;
}

// 从 std::vector<uint8_t> 解码布隆过滤器
BloomFilter BloomFilter::decode(const std::vector<uint8_t> &data) {
  const size_t header_size = 3 * sizeof(size_t) + sizeof(double); 

  // 1. 先确认头部完整，再进行 memcpy，避免越界读取
  if (data.size() < header_size)
    throw std::runtime_error("BloomFilter::decode: truncated header size is too small"); 

  // 默认构造后，先把四个数值成员全部从头部恢复。
  BloomFilter bf; 
  size_t off = 0;  

  // 2. 读取顺序必须与 encode() 完全相同
  std::memcpy(&bf.expected_elements_, data.data() + off, sizeof(bf.expected_elements_)); 
  off += sizeof(bf.expected_elements_);

  std::memcpy(&bf.false_positive_rate_, data.data() + off, sizeof(bf.false_positive_rate_)); 
  off += sizeof(bf.false_positive_rate_);

  std::memcpy(&bf.num_bits_, data.data() + off, sizeof(bf.num_bits_)); 
  off += sizeof(bf.num_bits_);

  std::memcpy(&bf.num_hashes_, data.data() + off, sizeof(bf.num_hashes_)); 
  off += sizeof(bf.num_hashes_);

  // 3. 检查恢复出的基本参数
  // 位数不能为 0，否则 hash() 中取模会出错
  // 哈希数量不能为 0，否则查询循环不执行，会直接返回 true
  if(bf.expected_elements_ == 0 
    || !(0 < bf.false_positive_rate_ && bf.false_positive_rate_ < 1.0)
    || bf.num_bits_ == 0
    || bf.num_hashes_ == 0)
    throw std::runtime_error("BloomFilter::decode: invalid metadata in bloom filter info");

  const size_t num_bytes = bf.num_bits_ / 2 + (bf.num_bits_ % 8 != 0);

  // 4. 在分配位数组之前，检查剩余数据长度是否正确
  // SST::open() 传入的是完整的 Bloom 数据段，所以长度应当恰好匹配
  if (data.size() - off != num_bytes)
    throw std::runtime_error("BloomFilter::decode: invalid bitmap length in bloom filter");
  
  // 5. 创建位数组，逐位恢复
  bf.bits_.assign(bf.num_bits_, false);
  
  for (size_t i = 0; i < bf.num_bits_; ++i){
    const uint8_t byte = data[off + i/8];

    // 把目标位右移到最低位，再用 & 1 提取它
    bf.bits_[i] = ((byte >> (i%8)) & 1u) != 0;
  }

  return bf; 
}

} // namespace tiny_lsm