// include/utils/bloom_filter.h

#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tiny_lsm {

class BloomFilter {
public:
  // 构造函数，初始化布隆过滤器
  // expected_elements: 预期插入的元素数量
  // false_positive_rate: 允许的假阳性率
  BloomFilter();
  BloomFilter(size_t expected_elements, double false_positive_rate);

  BloomFilter(size_t expected_elements, double false_positive_rate,
              size_t num_bits);

  void add(const std::string &key);

  // 如果key可能存在于布隆过滤器中，返回true；否则返回false
  bool possibly_contains(const std::string &key) const;

  // 清空布隆过滤器
  void clear();

  std::vector<uint8_t> encode();
  static BloomFilter decode(const std::vector<uint8_t> &data);

private:
  // 布隆过滤器的位数组大小
  size_t expected_elements_;
  // 允许的假阳性率
  double false_positive_rate_;
  size_t num_bits_;
  // 哈希函数的数量
  size_t num_hashes_;
  // 布隆过滤器的位数组
  std::vector<bool> bits_;

private:
  // 第一个哈希函数
  // 返回值: 哈希值
  size_t hash1(const std::string &key) const;

  // 第二个哈希函数
  // 返回值: 哈希值
  size_t hash2(const std::string &key) const;

  size_t hash(const std::string &key, size_t idx) const;
};
} // namespace tiny_lsm