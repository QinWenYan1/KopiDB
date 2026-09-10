#include "sst/sst.h"
#include "config/config.h"
#include "consts.h"
#include "sst/sst_iterator.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>

namespace tiny_lsm {

// Magic byte identifying a WiscKey SST footer
static constexpr uint8_t WISCKEY_MAGIC = 0x4B;
// Old footer size (24 bytes)
static constexpr size_t OLD_FOOTER_SIZE =
    sizeof(uint32_t) * 2 + sizeof(uint64_t) * 2;
// New WiscKey footer size (26 bytes)
static constexpr size_t WISCKEY_FOOTER_SIZE = OLD_FOOTER_SIZE + 2;

// **************************************************
// SST
// **************************************************

std::shared_ptr<SST> SST::open(size_t sst_id, FileObj file,
                               std::shared_ptr<BlockCache> block_cache,
                               std::shared_ptr<VLog> vlog) {
  // TODO: Lab 3.6 打开一个SST文件, 返回一个描述类
  // ? 步骤:
  // ?   0. 检测文件末尾 magic byte 判断是否为 WiscKey 格式 (WISCKEY_MAGIC =
  // 0x4B) ?      footer 共 24 字节 (老格式) 或 26 字节 (WiscKey, 末尾多
  // storage_mode + magic) ?   1. 从文件末尾读取 footer: meta_block_offset,
  // bloom_offset, min_tranc_id, max_tranc_id ?      如为 WiscKey 格式, 还需读取
  // storage_mode_ ?   2. 读取并解码 Bloom Filter (bloom_offset ~
  // meta_block_offset 之间) ?   3. 读取并解码元数据块 (meta_block_offset ~
  // bloom_offset 之间) ?      调用 BlockMeta::decode_meta_from_slice ?   4.
  // 设置 first_key 和 last_key ?   注: vlog 用于 WiscKey 模式下的 value 读取,
  // 直接赋值给 sst->vlog_
  return nullptr;
}

void SST::del_sst() { file.del_file(); }

std::shared_ptr<Block> SST::read_block(int64_t block_idx) {
  // TODO: Lab 3.6 根据 block 的 id 读取一个 Block
  // ? 先从 block_cache 查找; 未命中则计算该 block 的偏移和大小
  // ? 读取数据后调用 Block::decode(data, true) 解码
  // ? 解码后存入 block_cache 并返回
  // ? block 大小: 相邻 meta_entries 的 offset 差值; 最后一个 block 到
  // meta_block_offset
  return nullptr;
}

int64_t SST::find_block_idx(const std::string &key) {
  // TODO: Lab 3.6 二分查找
  // ? 先用布隆过滤器快速排除 (bloom_filter->possibly_contains(key))
  // ? 再在 meta_entries 上二分查找: first_key <= key <= last_key
  // ? 若未找到合适 block 返回 -1
  return 0;
}

SstIterator SST::get(const std::string &key, uint64_t tranc_id) {
  // TODO: Lab 3.6 根据查询 key 返回一个迭代器
  // ? 先检查 key 是否在 [first_key, last_key] 范围内, 否则返回 end()
  // ? 再用 bloom_filter 快速排除
  // ? 返回 SstIterator(shared_from_this(), key, tranc_id)
  throw std::runtime_error("Not implemented");
}

size_t SST::num_blocks() const { return meta_entries.size(); }

std::string SST::get_first_key() const { return first_key; }

std::string SST::get_last_key() const { return last_key; }

size_t SST::sst_size() const { return file.size(); }

size_t SST::get_sst_id() const { return sst_id; }

std::string SST::resolve_value(const std::string &raw_value) const {
  // WiscKey 模式下: raw_value 是 12 字节的 vlog 引用 [offset:8][size:4]
  // 普通模式下直接返回 raw_value
  if (storage_mode_ == 0 || raw_value.empty()) {
    return raw_value;
  }
  if (raw_value.size() < 12) {
    return raw_value;
  }
  uint64_t off = 0;
  uint32_t sz = 0;
  memcpy(&off, raw_value.data(), sizeof(uint64_t));
  memcpy(&sz, raw_value.data() + sizeof(uint64_t), sizeof(uint32_t));
  if (!vlog_) {
    throw std::runtime_error(
        "SST::resolve_value: vlog is null for WiscKey SST");
  }
  return vlog_->read_value(off, sz);
}

bool SST::is_wisckey() const { return storage_mode_ == 1; }

SstIterator SST::begin(uint64_t tranc_id, bool keep_all_versions) {
  // TODO: Lab 3.6 返回起始位置迭代器
  // ? 返回 SstIterator(shared_from_this(), tranc_id, keep_all_versions)
  throw std::runtime_error("Not implemented");
}

SstIterator SST::end() {
  // TODO: Lab 3.6 返回终止位置迭代器
  // ? 构造一个 SstIterator 并将 m_block_idx 设为 meta_entries.size(),
  // m_block_it 设为 nullptr
  throw std::runtime_error("Not implemented");
}

std::pair<uint64_t, uint64_t> SST::get_tranc_id_range() const {
  return std::make_pair(min_tranc_id_, max_tranc_id_);
}

// **************************************************
// SSTBuilder
// **************************************************

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom) : block(block_size) {
  // 初始化第一个block
  if (has_bloom) {
    bloom_filter = std::make_shared<BloomFilter>(
        TomlConfig::getInstance().getBloomFilterExpectedSize(),
        TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
  }
  meta_entries.clear();
  data.clear();
  first_key.clear();
  last_key.clear();
}

SSTBuilder::SSTBuilder(size_t block_size, bool has_bloom,
                       std::shared_ptr<VLog> vlog, size_t wisckey_threshold)
    : block(block_size), vlog_(std::move(vlog)),
      wisckey_threshold_(wisckey_threshold), storage_mode_(1) {
  // WiscKey 模式构造函数: vlog 用于大 value 分离存储
  if (has_bloom) {
    bloom_filter = std::make_shared<BloomFilter>(
        TomlConfig::getInstance().getBloomFilterExpectedSize(),
        TomlConfig::getInstance().getBloomFilterExpectedErrorRate());
  }
  meta_entries.clear();
  data.clear();
  first_key.clear();
  last_key.clear();
}

// Lab 3.5 添加键值对
// 当前在建 block 的首尾 key (草稿)
// 每收下一条 entry, last_key 就刷新一次
void SSTBuilder::add(const std::string &key, const std::string &value,
                     uint64_t tranc_id) {
  // ? 尝试向 block 添加 entry; 若返回 false (block满) 先调用 finish_block()
  // 再添加 ? 注意: 相同 key 必须在同一个 block 中 (force_write = key ==
  // last_key) ? 更新 last_key

  // 1.  首个 block 的 first key 只在第一次调用时记录（构造时已 clear）
  //     后续每个新 block 的 first_key 在步骤 6 里更新
  if (first_key.empty())
    first_key = key;

  // 2. bloom filter 收录 key （将来 SST::get 先问 bloom 再读 block）
  if (bloom_filter)
    bloom_filter->add(key);

  // 3.  维护事务 id 区间，build 时写进 footer 供上层按 tranc 过滤整个 SST
  //     更新 max_tranc_id_ / min_tranc_id_
  max_tranc_id_ = std::max(max_tranc_id_, tranc_id);
  min_tranc_id_ = std::min(min_tranc_id_, tranc_id);

  // 4.  WiscKey 模式下: 若 value 非空且超过 wisckey_threshold_, 将 value 写入
  // vlog
  //     并将 vlog 引用 [offset:8][size:4] 作为 actual_value
  //
  // WiscKey 是什么: 一种"值分离"设计
  //  默认模式下 value 跟着 key 一起住进 block（inline）
  //  但 LSM 的 compaction 会反复重写 SST，大 value
  //  每次都被原样重搬一遍（写放大） WiscKey 的做法：大 value 不写进
  //  block，追加到一个专门的日志文件（vlog） block 里只存一张 12 字节的"提货单"
  //  [offset:8][size:4]，读的时候拿单子去 vlog 取货
  //  就像搬家：钥匙串随身带，大家具走物流
  const std::string *actual_value = &value;
  std::string vlog_ref;
  if (storage_mode_ == 1 && vlog_ && !value.empty() && wisckey_threshold_ > 0 &&
      value.size() > wisckey_threshold_) {
    // 空value (tombstone 删除标记) 永不分离: 没体积还可省白送一次 IO
    // 把 value 本体追加到 vlog
    // 文件末尾，返回写入位置的偏移量——相当于物流揽件后给你的单号
    uint64_t offset = vlog_->append(key, value);

    // vlog 引用格式: [offset:8][size:4], memcpy 本机序 (同 block 的取舍)
    vlog_ref.resize(sizeof(uint64_t) + sizeof(uint32_t));
    memcpy(vlog_ref.data(), &offset, sizeof(uint64_t));
    uint32_t vlen = static_cast<uint32_t>(value.size());
    memcpy(vlog_ref.data() + sizeof(uint64_t), &vlen, sizeof(uint32_t));

    // 指针改道，大 value 不发生拷贝
    actual_value = &vlog_ref;
  }

  // 5. 核心不变式：连续相同 key 的所有版本必须挤在同一个 block
  //   (Block 的二分/迭代器都假设)
  //   同 key → force_write=true → 即使 block 已经满了也硬塞。为什么？
  //   你写的 adjust_idx_by_tranc_id 假设"同 key 的所有版本连续存放在同一个
  //   block 内" 先退到组首、再往后找可见版本。 要是两个版本被切到不同
  //   block，SST 层的 find_block_idx
  //   二分只会命中其中一个块，另一个块里的版本就永远找不到了。
  //   版本团聚是正确性问题，不是优化。
  //   不同 key → force_write=false → 遵守容量纪律，满了就被拒（返回 false)
  bool force_write = (key == last_key);
  if (block.add_entry(key, *actual_value, tranc_id, force_write)) {
    last_key = key;
    return;
  }

  // 6. block满了: 封盘开新块。空 block 有 “必收第一条” key-value 对，这次必成功
  finish_block();
  block.add_entry(key, *actual_value, tranc_id, false);
  // finish_block 把旧的 first_key 快照进 meta, 这里开新章
  first_key = key;
  last_key = key;
}

size_t SSTBuilder::real_size() const { return data.size() + block.cur_size(); }

size_t SSTBuilder::estimated_size() const { return data.size(); }

// TODO: Lab 3.5 构建块
// 草稿定格 → BlockMeta(offset, first_key, last_key)
// 压进 meta_entries, 一个 block 一张
void SSTBuilder::finish_block() {
  // ? 将当前 block 编码并追加到 data, 同时向 meta_entries 添加元数据
  // ? 然后重置 block 为新的空 Block
  // ? meta_entries 记录: (当前data起始偏移, first_key, last_key)

  // 1. 把当前 block 挪出来编码（默认带CRC32）
  auto old_block = std::move(block);
  auto encoded_block = old_block.encode();

  // 2. 草稿定格为 BlockMeta: (块起始偏移)
  //    偏移 = 此刻 data 的末尾：之前所有块的字节都在 data 里,
  //    本块将要写在这个位置，所以 data.size() 就是它在文件中的偏移
  meta_entries.emplace_back(data.size(), first_key, last_key);

  // 3. 编码字节追加进入到 data [存放该SST的多block位置]
  data.insert(data.end(), encoded_block.begin(), encoded_block.end());

  // 4. 然后重建 block std::move 之后的对象是"有效但未指定"状态
  //    标准不保证它是空的——显式重建一个同容量新块, 不靠实现细节
  block = Block(block_size);
}

// TODO: Lab 3.5 构建一个SST，并落盘
// SST.first_key = meta_entries.front().first_key
// SST.last_key  = meta_entries.back().last_key
//               = 整个文件的首尾 key
std::shared_ptr<SST>
SSTBuilder::build(size_t sst_id, const std::string &path,
                  std::shared_ptr<BlockCache> block_cache) {
  // ? 1. 若 block 非空则调用 finish_block()
  // ? 2. 若 meta_entries 为空则抛出异常
  // ? 3. 编码元数据块并追加到 data (BlockMeta::encode_meta_to_slice)
  // ? 4. 追加 Bloom Filter 编码
  // ? 5. 写入 footer (老格式 24B 或 WiscKey 26B):
  // ?
  // [meta_offset:uint32][bloom_offset:uint32][min_tranc_id:uint64][max_tranc_id:uint64]
  // ?    WiscKey 额外: [storage_mode_:uint8][WISCKEY_MAGIC:uint8]
  // ? 6. 调用 FileObj::create_and_write 写文件
  // ? 7. 构造并返回 SST 对象

  //1. 收尾，将当前的 block 里面没有定格的 entry， 先封盘
  if (!block.is_empty())
    finish_block();

  //2. 一个块都没有 = 空 SST，拒绝 build，直接throw error 
  if (meta_entries.empty())
    throw std::runtime_error("SSTBuilder::build: Cannot build empty SST"); 

  //3. 元数据段：必须编码到临时 vector 再追加
  //   你写的 encode_meta_to_slice 是 resize 覆盖式，直接传 data 会把所有 block 字节冲掉
  //   覆盖式语意的代价：调用方负责给空容器
  uint32_t meta_offset = static_cast<uint32_t>(data.size()); 
  std::vector<uint8_t> meta_section; 
  BlockMeta::encode_meta_to_slice(meta_entries, meta_section); 
  data.insert(data.end(), meta_section.begin(), meta_section.end()); 
  
  //4. bloom filter，记下其偏移量再追加
  uint32_t bloom_off = static_cast<uint32_t>(data.size()); 
  if(bloom_filter){
    auto bloom_bytes = bloom_filter->encode(); 
    data.insert(data.end(), bloom_bytes.begin(), bloom_bytes.end()); 
  }

  //5. extra information 段: 老格式 24B; WiscKey 模式 26B (多一个storage_mode + 魔数)
  //   [meta_offset:u32][bloom_offset:u32][min_tranc:u64][max_tranc:u64]
  size_t footer_size = (storage_mode_ == 1) ? 26 : 24; 
  size_t footer_base = data.size(); 
  data.resize(footer_base + footer_size); 
  uint8_t* p = data.data() + data.size(); 
  memcpy(p, &meta_offset, sizeof(uint32_t)); 

}
} // namespace tiny_lsm
 