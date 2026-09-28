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

void BloomFilter::add(const std::string &key) {
  // 对每个哈希函数计算哈希值，并将对应位置的位设置为true
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

size_t BloomFilter::hash(const std::string &key, size_t idx) const { return 0; }

// 编码布隆过滤器为 std::vector<uint8_t>
std::vector<uint8_t> BloomFilter::encode() { return {}; }

// 从 std::vector<uint8_t> 解码布隆过滤器
BloomFilter BloomFilter::decode(const std::vector<uint8_t> &data) { return {}; }
} // namespace tiny_lsm