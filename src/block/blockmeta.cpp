#include "block/blockmeta.h"
#include <cstring>
#include <functional>
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
  // MetaEntry = offset(32) | first_key_len(16) | last_key_len(16) | last_key

  // 1. 预计算总大小，一次 resize 到位（覆盖式，不是append）
  //    头尾各一个 uint32: num_entries 和 hash 
  size_t total = sizeof(uint32_t)*2;
  for (const auto &m : meta_entries)
    total += sizeof(uint32_t) + sizeof(uint16_t)*2 + m.first_key.size() + m.last_key.size(); 
  metadata.resize(total);  
  
  // 写指针，写完一段往前面走
  uint8_t *ptr = metadata.data(); 

  //2. 条目数 (uint32 memcpy 本机字节序)
  
}

 // TODO: Lab 3.4 将二进制字节数组解码为内存中的`Blcok`元数据
std::vector<BlockMeta>
BlockMeta::decode_meta_from_slice(const std::vector<uint8_t> &metadata) {
  return {};
}

} // namespace tiny_lsm