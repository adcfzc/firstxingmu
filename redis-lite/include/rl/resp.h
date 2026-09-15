// Copyright (c) 2025 redis-lite authors. MIT License.
//
// resp.h — RESP2 协议的增量解析与序列化
//
// 设计要点（面试高频）：
//   * 增量解析：TCP 是字节流，一条命令可能被拆成多个 segment 到达（半包），
//     也可能多条命令挤在一个 segment 里（粘包）。Parser 内部持有 std::string
//     游标，数据不完整时返回 nullopt 并「不消费任何字节」，等下次可读继续。
//   * 零解析分支：Next() 只做一次长度计算，不构建中间对象；
//     真正的字符串拷贝发生在需要时。
//   * 兼容 redis-cli / redis-benchmark，因此压测工具可以直接复用官方工具，
//     也可以用自己的 tools/bench.cc。
//
// 支持的入站类型：RESP 数组（客户端标准请求）
//   *2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n
// 支持的出站类型：+simple -error :integer $bulk $-1(null) *array

#ifndef RL_RESP_H
#define RL_RESP_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rl {

// RESP 换行符，抽出来避免到处硬编码 "\r\n"。
constexpr char kCRLF[] = "\r\n";
constexpr size_t kCRLFLen = 2;

// --------------------------------------------------------------- 编码工具
// 直接把编码结果追加到 out，避免临时 string。
void EncodeSimpleString(const std::string& s, std::string* out);
void EncodeError(const std::string& msg, std::string* out);
void EncodeInteger(int64_t v, std::string* out);
void EncodeBulkString(const std::string& s, std::string* out);
void EncodeNullBulk(std::string* out);
void EncodeArrayHeader(size_t n, std::string* out);

// 把一条命令编码成 RESP 数组，AOF 落盘复用这里的逻辑。
// 保证 AOF 内容与客户端请求格式完全一致 → 恢复时可以复用同一个解析器。
void EncodeCommand(const std::vector<std::string>& args, std::string* out);

// 解析一行 CRLF 结尾的文本（不含 CRLF）。
bool ParseLine(const char* data, size_t len, std::string* out);
// 严格解析十进制整数：允许前导 '-'，不接受空格/多余字符/溢出。
bool ParseInt64(const char* data, size_t len, int64_t* out);

// 把字符串转小写（只处理 ASCII），用于命令名大小写不敏感匹配。
std::string ToLowerAscii(const std::string& s);

// glob 风格匹配，支持 * ? [abc] [a-z] 与转义 \。KEYS 命令使用。
bool GlobMatch(const char* pattern, size_t plen, const char* str, size_t slen);

// ------------------------------------------------------------- RespParser
class RespParser {
public:
    RespParser() = default;

    // 追加原始字节。
    void Append(const char* data, size_t len);
    void Append(const std::string& s) { Append(s.data(), s.size()); }

    // 尝试解析下一条完整命令。
    //   返回 true  : 成功，args 已填充，内部游标前移
    //   返回 false : 数据不足，内部状态不变（重要：绝不能部分消费）
    //   抛 ProtocolException : 协议错误，调用方应回复错误并关闭连接
    bool Next(std::vector<std::string>* args);

    size_t buffered() const { return buf_.size() - pos_; }

    // 已缓存但未消费的字节数超过阈值时由上层断开，防止内存被慢速攻击打爆。
    static constexpr size_t kMaxInlineSize = 64 * 1024;
    static constexpr size_t kMaxBulkSize = 512 * 1024 * 1024;  // 与 Redis 一致

private:
    // 确保从 pos_ 起至少有 n 字节；不足返回 false（不消费）。
    bool Ensure(size_t n) const;
    const char* cursor() const { return buf_.data() + pos_; }

    // 解析一条 RESP 值。depth 用于防御嵌套数组导致的栈溢出。
    bool ParseValue(std::string* out, int depth);
    // 跳过（而不是解析）一条值，用于忽略数组中的嵌套。
    bool SkipValue(int depth);

    std::string buf_;
    size_t pos_ = 0;
};

// 协议错误。上层捕获后回复 -ERR Protocol error 并关闭连接。
class ProtocolException : public std::exception {
public:
    explicit ProtocolException(std::string msg) : msg_(std::move(msg)) {}
    const char* what() const noexcept override { return msg_.c_str(); }

private:
    std::string msg_;
};

}  // namespace rl

#endif  // RL_RESP_H
