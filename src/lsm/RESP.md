好的，Redis 支持多种操作命令，除了 `SET` 和 `GET` 之外，还有很多其他常见的命令。以下是几个常用的 Redis 命令及其对应的 RESP 编码方式示例：

### 常见 Redis 命令及其 RESP 编码

#### 1. **SET**
设置指定键的值。

- **命令格式**：
  ```
  *3
  $3
  SET
  $3
  key
  $5
  value
  ```

- **解释**：
  - `*3` 表示接下来有 3 个元素。
  - `$3` 表示第一个元素是长度为 3 的字符串 `"SET"`。
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key"`.
  - `$5` 表示第三个元素是长度为 5 的字符串 `"value"`。

#### 2. **GET**
获取指定键的值。

- **命令格式**：
  ```
  *2
  $3
  GET
  $3
  key
  ```

- **解释**：
  - `*2` 表示接下来有 2 个元素。
  - `$3` 表示第一个元素是长度为 3 的字符串 `"GET"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key"`.

#### 3. **DEL**
删除一个或多个键。

- **命令格式**：
  ```
  *3
  $3
  DEL
  $3
  key1
  $3
  key2
  ```

- **解释**：
  - `*3` 表示接下来有 3 个元素。
  - `$3` 表示第一个元素是长度为 3 的字符串 `"DEL"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key1"`.
  - `$3` 表示第三个元素是长度为 3 的字符串 `"key2"`.

#### 4. **INCR**
将键存储的数字值增加 1。

- **命令格式**：
  ```
  *2
  $4
  INCR
  $3
  key
  ```

- **解释**：
  - `*2` 表示接下来有 2 个元素。
  - `$4` 表示第一个元素是长度为 4 的字符串 `"INCR"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key"`.

#### 5. **DECR**
将键存储的数字值减少 1。

- **命令格式**：
  ```
  *2
  $4
  DECR
  $3
  key
  ```

- **解释**：
  - `*2` 表示接下来有 2 个元素。
  - `$4` 表示第一个元素是长度为 4 的字符串 `"DECR"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key"`.

#### 6. **EXPIRE**
设置键的过期时间（秒）。

- **命令格式**：
  ```
  *3
  $7
  EXPIRE
  $3
  key
  $2
  60
  ```

- **解释**：
  - `*3` 表示接下来有 3 个元素。
  - `$7` 表示第一个元素是长度为 7 的字符串 `"EXPIRE"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key"`.
  - `$2` 表示第三个元素是长度为 2 的字符串 `"60"`.

#### 7. **TTL**
获取键的剩余生存时间（秒）。

- **命令格式**：
  ```
  *2
  $3
  TTL
  $3
  key
  ```

- **解释**：
  - `*2` 表示接下来有 2 个元素。
  - `$3` 表示第一个元素是长度为 3 的字符串 `"TTL"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"key"`.

#### 8. **HSET**
设置哈希表中字段的值。

- **命令格式**：
  ```
  *4
  $4
  HSET
  $3
  hash
  $3
  field
  $5
  value
  ```

- **解释**：
  - `*4` 表示接下来有 4 个元素。
  - `$4` 表示第一个元素是长度为 4 的字符串 `"HSET"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"hash"`.
  - `$3` 表示第三个元素是长度为 3 的字符串 `"field"`.
  - `$5` 表示第四个元素是长度为 5 的字符串 `"value"`.

#### 9. **HGET**
获取哈希表中字段的值。

- **命令格式**：
  ```
  *3
  $4
  HGET
  $3
  hash
  $5
  field
  ```

- **解释**：
  - `*3` 表示接下来有 3 个元素。
  - `$4` 表示第一个元素是长度为 4 的字符串 `"HGET"`.
  - `$3` 表示第二个元素是长度为 3 的字符串 `"hash"`.
  - `$5` 表示第三个元素是长度为 5 的字符串 `"field"`.

#### 10. **LPUSH**
将一个或多个值插入列表头部。

- **命令格式**：
  ```
  *3
  $5
  LPUSH
  $5
  mylist
  $4
  item
  ```

- **解释**：
  - `*3` 表示接下来有 3 个元素。
  - `$5` 表示第一个元素是长度为 5 的字符串 `"LPUSH"`.
  - `$5` 表示第二个元素是长度为 5 的字符串 `"mylist"`.
  - `$4` 表示第三个元素是长度为 4 的字符串 `"item"`.

#### 11. **LRANGE**
获取列表中指定范围的元素。

- **命令格式**：
  ```
  *4
  $6
  LRANGE
  $5
  mylist
  $1
  0
  $1
  1
  ```

- **解释**：
  - `*4` 表示接下来有 4 个元素。
  - `$6` 表示第一个元素是长度为 6 的字符串 `"LRANGE"`.
  - `$5` 表示第二个元素是长度为 5 的字符串 `"mylist"`.
  - `$1` 表示第三个元素是长度为 1 的字符串 `"0"`.
  - `$1` 表示第四个元素是长度为 1 的字符串 `"1"`.

### 实现更多命令

基于上述命令及其 RESP 编码方式，我们可以在你的服务器代码中实现更多的命令处理逻辑。以下是一个扩展后的 `handleRequest` 函数示例，增加了对 `DEL`, `INCR`, `DECR`, `EXPIRE`, `TTL`, `HSET`, `HGET`, `LPUSH`, 和 `LRANGE` 命令的支持：

```cpp
std::string handleRequest(const std::string &request) {
    size_t pos = 0;

    // 检查是否以 '*' 开头
    if (request.empty() || request[pos] != '*') {
        LOG_ERROR << "Protocol error: expected '*'";
        return "-ERR Protocol error: expected '*'\r\n";
    }

    // 解析数组元素数量
    int numElements = 0;
    try {
        numElements = std::stoi(request.substr(pos + 1)); // 跳过 '*'
        LOG_INFO << "Number of elements: " << numElements;
    } catch (const std::exception &) {
        LOG_ERROR << "Protocol error: invalid number of elements";
        return "-ERR Protocol error: invalid number of elements\r\n";
    }
    pos += request.find('\r', pos) + 2; // 跳过 '\r\n'

    std::vector<std::string> args;
    for (int i = 0; i < numElements; ++i) {
        // 检查是否以 '$' 开头
        if (pos >= request.size() || request[pos] != '$') {
            LOG_ERROR << "Protocol error: expected '$'";
            return "-ERR Protocol error: expected '$'\r\n";
        }

        // 解析批量字符串长度
        int len = 0;
        try {
            len = std::stoi(request.substr(pos + 1)); // 跳过 '$'
            LOG_INFO << "Bulk string length: " << len;
        } catch (const std::exception &) {
            LOG_ERROR << "Protocol error: invalid bulk string length";
            return "-ERR Protocol error: invalid bulk string length\r\n";
        }
        pos += request.find('\r', pos) + 2; // 跳过 '\r\n'

        // 检查是否有足够的字符来读取指定长度的数据
        if (pos + len > request.size()) {
            LOG_ERROR << "Protocol error: bulk string length exceeds request size";
            return "-ERR Protocol error: bulk string length exceeds request size\r\n";
        }

        // 提取批量字符串内容
        std::string bulkString = request.substr(pos, len);
        LOG_INFO << "Bulk string content: " << bulkString;
        args.push_back(bulkString);
        pos += len + 2; // 跳过数据和 '\r\n'
    }

    LOG_INFO << "Request: ";
    for (const auto &arg : args) {
        LOG_INFO << arg << " ";
    }
    LOG_INFO << '\n';

    // 处理命令
    if (args[0] == "SET") {
        if (args.size() != 3)
            return "-ERR wrong number of arguments for 'SET' command\r\n";
        kvStore_[args[1]] = args[2];
        return "+OK\r\n";
    } else if (args[0] == "GET") {
        if (args.size() != 2)
            return "-ERR wrong number of arguments for 'GET' command\r\n";
        auto it = kvStore_.find(args[1]);
        if (it != kvStore_.end()) {
            return "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
        } else {
            return "$-1\r\n"; // 表示键不存在
        }
    } else if (args[0] == "DEL") {
        int deleted = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            if (kvStore_.erase(args[i])) {
                ++deleted;
            }
        }
        return ":" + std::to_string(deleted) + "\r\n";
    } else if (args[0] == "INCR") {
        if (args.size() != 2)
            return "-ERR wrong number of arguments for 'INCR' command\r\n";
        try {
            int value = std::stoi(kvStore_[args[1]]);
            kvStore_[args[1]] = std::to_string(++value);
            return ":" + std::to_string(value) + "\r\n";
        } catch (const std::exception &) {
            return "-ERR value is not an integer or out of range\r\n";
        }
    } else if (args[0] == "DECR") {
        if (args.size() != 2)
            return "-ERR wrong number of arguments for 'DECR' command\r\n";
        try {
            int value = std::stoi(kvStore_[args[1]]);
            kvStore_[args[1]] = std::to_string(--value);
            return ":" + std::to_string(value) + "\r\n";
        } catch (const std::exception &) {
            return "-ERR value is not an integer or out of range\r\n";
        }
    } else if (args[0] == "EXPIRE") {
        if (args.size() != 3)
            return "-ERR wrong number of arguments for 'EXPIRE' command\r\n";
        try {
            int ttl = std::stoi(args[2]);
            if (ttl <= 0) {
                return ":0\r\n"; // 设置失败
            }
            // 这里可以添加实际的过期时间处理逻辑
            return ":1\r\n"; // 设置成功
        } catch (const std::exception &) {
            return "-ERR value is not an integer or out of range\r\n";
        }
    } else if (args[0] == "TTL") {
        if (args.size() != 2)
            return "-ERR wrong number of arguments for 'TTL' command\r\n";
        // 这里可以添加实际的 TTL 查询逻辑
        return ":0\r\n"; // 返回 0 表示没有设置过期时间
    } else if (args[0] == "HSET") {
        if (args.size() != 4)
            return "-ERR wrong number of arguments for 'HSET' command\r\n";
        kvStore_[args[1] + ":" + args[2]] = args[3];
        return ":1\r\n"; // 表示成功设置了 1 个字段
    } else if (args[0] == "HGET") {
        if (args.size() != 3)
            return "-ERR wrong number of arguments for 'HGET' command\r\n";
        auto it = kvStore_.find(args[1] + ":" + args[2]);
        if (it != kvStore_.end()) {
            return "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
        } else {
            return "$-1\r\n"; // 表示字段不存在
        }
    } else if (args[0] == "LPUSH") {
        if (args.size() < 2)
            return "-ERR wrong number of arguments for 'LPUSH' command\r\n";
        std::string listKey = args[1];
        int count = args.size() - 2;
        for (int i = 2; i < args.size(); ++i) {
            kvStore_[listKey + ":" + std::to_string(i - 2)] = args[i];
        }
        return ":" + std::to_string(count) + "\r\n"; // 返回插入的元素数量
    } else if (args[0] == "LRANGE") {
        if (args.size() != 4)
            return "-ERR wrong number of arguments for 'LRANGE' command\r\n";
        std::string listKey = args[1];
        int start = std::stoi(args[2]);
        int end = std::stoi(args[3]);
        std::string response = "*";
        std::vector<std::string> result;
        for (int i = start; i <= end; ++i) {
            std::string listItem = kvStore_[listKey + ":" + std::to_string(i)];
            if (!listItem.empty()) {
                result.push_back(listItem);
            }
        }
        response += std::to_string(result.size()) + "\r\n";
        for (const auto &item : result) {
            response += "$" + std::to_string(item.size()) + "\r\n" + item + "\r\n";
        }
        return response;
    } else {
        return "-ERR unknown command '" + args[0] + "'\r\n";
    }
}
```

### 测试与验证

你可以使用 `redis-cli` 来测试这些新实现的命令：

```bash
# 测试 DEL
redis-cli -p 6379 SET key1 "value1"
redis-cli -p 6379 SET key2 "value2"
redis-cli -p 6379 DEL key1 key2

# 测试 INCR 和 DECR
redis-cli -p 6379 SET counter 10
redis-cli -p 6379 INCR counter
redis-cli -p 6379 DECR counter

# 测试 EXPIRE 和 TTL
redis-cli -p 6379 SET tempKey "tempValue"
redis-cli -p 6379 EXPIRE tempKey 60
redis-cli -p 6379 TTL tempKey

# 测试 HSET 和 HGET
redis-cli -p 6379 HSET myhash field1 "Hello"
redis-cli -p 6379 HGET myhash field1

# 测试 LPUSH 和 LRANGE
redis-cli -p 6379 LPUSH mylist item1 item2 item3
redis-cli -p 6379 LRANGE mylist 0 2
```

通过这些测试命令，你可以验证你的服务器是否正确实现了这些 Redis 命令。如果有任何问题，请检查日志输出并进行相应的调试。