#pragma once

#include <string>
#include <vector>

// Include the necessary toml11 header
// Assuming a common setup where you include the main header:

namespace tiny_lsm {

class TomlConfig {
private:
  std::string config_file_path_; // 记录配置文件路径

  // --- LSM Core ---
  long long lsm_tol_mem_size_limit_;
  long long lsm_per_mem_size_limit_;
  int lsm_block_size_;
  int lsm_sst_level_ratio_;

  // --- LSM Cache ---
  int lsm_block_cache_capacity_;
  int lsm_block_cache_k_;

  // --- Bloom Filter ---
  int bloom_filter_expected_size_;
  double bloom_filter_expected_error_rate_;

  // --- WiscKey ---
  size_t wisckey_value_threshold_ = 0;

  // Private method to set default values
  void setDefaultValues();

  // Constructor
  TomlConfig(const std::string &filePath);

  // Method to load configuration from a TOML file
  // Returns true on success, false on failure (e.g., file not found, parse
  // error)
  bool loadFromFile(const std::string &filePath);

  bool saveToFile(const std::string &filePath);

public:
  ~TomlConfig();

  // --- Getter Methods ---
  // Declare all your getter methods here
  long long getLsmTolMemSizeLimit() const;
  long long getLsmPerMemSizeLimit() const;
  int getLsmBlockSize() const;
  int getLsmSstLevelRatio() const;

  int getLsmBlockCacheCapacity() const;
  int getLsmBlockCacheK() const;

  int getBloomFilterExpectedSize() const;
  double getBloomFilterExpectedErrorRate() const;

  size_t getWisckeyValueThreshold() const;

  static const TomlConfig &
  getInstance(const std::string &config_path = "config.toml");

  // for debug
  void modify_lsm_tol_mem_size_limit(long long one);
  void modify_lsm_per_mem_size_limit(long long one);
  void modify_lsm_block_size(int one);
};
} // namespace tiny_lsm