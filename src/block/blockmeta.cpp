#include "block/blockmeta.h"
#include <cstring>
#include <functional>
#include <iterator>
#include <stdexcept>

namespace tiny_lsm {
BlockMeta::BlockMeta() : offset(0), first_key(""), last_key("") {}

BlockMeta::BlockMeta(size_t offset, const std::string &first_key,
                     const std::string &last_key)
    : offset(offset), first_key(first_key), last_key(last_key) {}

// TODO: Lab 3.4 将内存中所有`Blcok`的元数据编码为二进制字节数组
void BlockMeta::encode_meta_to_slice(std::vector<BlockMeta> &meta_entries,
                                     std::vector<uint8_t> &metadata) {
  // ? 输入输出都由参数中的引用给定, 你不需要自己创建`vector`
  // 布局 blockmeta.h 已经钉死：
  // [num_entries:32][MetaEntry]...[Hash:32]
  // MetaEntry = offset(32) | first_key_len(16) | first_key | last_key_len(16) | last_key

  // 1. 预计算总大小，一次 resize 到位（覆盖式，不是append）
  //    头尾各一个 uint32: num_entries 和 hash 
  size_t total = sizeof(uint32_t)*2;
  for (const auto &m : meta_entries)
    total += sizeof(uint32_t) + sizeof(uint16_t)*2 + m.first_key.size() + m.last_key.size(); 
  metadata.resize(total);  
  
  // 写指针，写完一段往前面走
  uint8_t *ptr = metadata.data(); 

  //2. 条目数 (uint32 memcpy 本机字节序)
  uint32_t num = static_cast<uint32_t>(meta_entries.size()); 
  memcpy(ptr, &num, sizeof(uint32_t)); 
  ptr += sizeof(uint32_t); 

  //3. 逐条写 MetaEntry: offset(32) | fk_len(16) | fk | lk_len(16) | lk 
  for (const auto &m : meta_entries){
    // size_t 收窄为 uint32
    uint32_t off = static_cast<uint32_t>(m.offset); 
    memcpy(ptr, &off, sizeof(uint32_t)); 
    ptr += sizeof(uint32_t); 

    uint16_t fk_len = static_cast<uint16_t>(m.first_key.size()); 
    memcpy(ptr, &fk_len, sizeof(uint16_t)); 
    ptr += sizeof(uint16_t); 
    memcpy(ptr, m.first_key.data(), fk_len); 
    ptr += fk_len;


    uint16_t lk_len = static_cast<uint16_t>(m.last_key.size()); 
    memcpy(ptr, &lk_len, sizeof(uint16_t)); 
    ptr += sizeof(uint16_t); 
    memcpy(ptr, m.last_key.data(), lk_len); 
    ptr += lk_len; 
  }

  //4. ! hash 只盖 entries 段 [data+4, ptr), 不含 num_entries
  //    std::hash 返回 size_t，截断保留低 32 位
  const uint8_t *entries_begin = metadata.data() + sizeof(uint32_t); // 段起点
  size_t entries_len = ptr - entries_begin;   
  auto entries_view = std::string_view(
    reinterpret_cast<const char*>(entries_begin), entries_len
  ); 

  uint32_t hash = static_cast<uint32_t>(std::hash<std::string_view>{}(entries_view)); 
  memcpy(ptr, &hash, sizeof(uint32_t));

}

 // TODO: Lab 3.4 将二进制字节数组解码为内存中的`Blcok`元数据
std::vector<BlockMeta>
BlockMeta::decode_meta_from_slice(const std::vector<uint8_t> &metadata) {
  return {};
}

} // namespace tiny_lsm