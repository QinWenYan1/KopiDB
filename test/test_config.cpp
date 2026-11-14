#include "config/config.h"
#include "logger/logger.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <toml.hpp>

using namespace ::tiny_lsm;

namespace {

// 全部使用非默认值：即使加载中途失败，退回默认值也不能让测试误通过。
constexpr const char *storage_config = R"toml(
[lsm.core]
LSM_TOL_MEM_SIZE_LIMIT = 8388608
LSM_PER_MEM_SIZE_LIMIT = 1048576
LSM_BLOCK_SIZE = 8192
LSM_SST_LEVEL_RATIO = 3
[lsm.cache]
LSM_BLOCK_CACHE_CAPACITY = 37
LSM_BLOCK_CACHE_K = 3
[bloom_filter]
BLOOM_FILTER_EXPECTED_SIZE = 12345
BLOOM_FILTER_EXPECTED_ERROR_RATE = 0.025
[lsm.wisckey]
WISCKEY_VALUE_THRESHOLD = 1536
)toml";

class TemporaryConfig {
public:
  std::filesystem::path directory;
  std::filesystem::path path;

  TemporaryConfig() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    directory = std::filesystem::temp_directory_path() /
                ("kopidb_config_test_" + std::to_string(stamp));
    if (!std::filesystem::create_directory(directory)) {
      throw std::runtime_error("Cannot create isolated config test directory");
    }
    path = directory / "config.toml";
    std::ofstream file(path);
    file << storage_config;
    file.close();
    if (!file) {
      throw std::runtime_error("Cannot write config test fixture");
    }
  }

  ~TemporaryConfig() {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
};

void check_storage_config_load() {
  TemporaryConfig fixture;
  const auto &config = TomlConfig::getInstance(fixture.path.string());
  EXPECT_EQ(config.getLsmTolMemSizeLimit(), 8388608);
  EXPECT_EQ(config.getLsmPerMemSizeLimit(), 1048576);
  EXPECT_EQ(config.getLsmBlockSize(), 8192);
  EXPECT_EQ(config.getLsmSstLevelRatio(), 3);
  EXPECT_EQ(config.getLsmBlockCacheCapacity(), 37);
  EXPECT_EQ(config.getLsmBlockCacheK(), 3);
  EXPECT_EQ(config.getBloomFilterExpectedSize(), 12345);
  EXPECT_DOUBLE_EQ(config.getBloomFilterExpectedErrorRate(), 0.025);
  EXPECT_EQ(config.getWisckeyValueThreshold(), 1536u);
}

void check_storage_config_save() {
  TemporaryConfig fixture;
  {
    // 复制公开单例后移走配置文件，作用域结束时由现有析构逻辑重新保存。
    // 不公开私有 saveToFile，也不修改正常运行时的配置文件。
    TomlConfig snapshot = TomlConfig::getInstance(fixture.path.string());
    ASSERT_TRUE(std::filesystem::remove(fixture.path));
  }
  ASSERT_TRUE(std::filesystem::exists(fixture.path));
  const auto saved = toml::parse(fixture.path.string());
  // 保存结果只应包含存储配置所属的两个顶层表，不能带回已移除组件的配置。
  ASSERT_EQ(saved.as_table().size(), 2u);
  ASSERT_TRUE(saved.contains("lsm"));
  ASSERT_TRUE(saved.contains("bloom_filter"));
  const auto &core = saved.at("lsm").at("core");
  EXPECT_EQ(core.at("LSM_TOL_MEM_SIZE_LIMIT").as_integer(), 8388608);
  EXPECT_EQ(core.at("LSM_PER_MEM_SIZE_LIMIT").as_integer(), 1048576);
  EXPECT_EQ(core.at("LSM_BLOCK_SIZE").as_integer(), 8192);
  EXPECT_EQ(core.at("LSM_SST_LEVEL_RATIO").as_integer(), 3);
  const auto &cache = saved.at("lsm").at("cache");
  EXPECT_EQ(cache.at("LSM_BLOCK_CACHE_CAPACITY").as_integer(), 37);
  EXPECT_EQ(cache.at("LSM_BLOCK_CACHE_K").as_integer(), 3);
  const auto &bloom = saved.at("bloom_filter");
  EXPECT_EQ(bloom.at("BLOOM_FILTER_EXPECTED_SIZE").as_integer(), 12345);
  EXPECT_DOUBLE_EQ(bloom.at("BLOOM_FILTER_EXPECTED_ERROR_RATE").as_floating(),
                   0.025);
}

} // namespace

// 场景：用户只提供存储配置。预期：LSM、缓存、Bloom 与 WiscKey 均读到配置值。
TEST(ConfigDeathTest, LoadStorageSettingsWithoutExtraSections) {
  // 单例每进程只加载一次；重新启动子进程，避免其他用例先初始化单例的影响。
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        check_storage_config_load();
        // helper 已释放临时目录；跳过单例的退出析构，避免重新生成已清理的文件。
        std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

// 场景：配置文件丢失，由析构重新保存。预期：原有存储参数保留，额外配置不再输出。
TEST(ConfigDeathTest, PersistOnlyStorageSettings) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        check_storage_config_save();
        std::_Exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(ConfigTest, BasicTest) {
  // 编译后的单元测试位置为: build/linux/x86_64/debug/test_config
  // 而配置文件放在项目根目录
  TomlConfig gConfig = TomlConfig::getInstance("../../../../config.toml");

  EXPECT_EQ(gConfig.getLsmBlockSize(), 32768);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  init_spdlog_file();
  return RUN_ALL_TESTS();
}
