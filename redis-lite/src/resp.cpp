// Copyright (c) 2025 redis-lite authors. MIT License.
//
// resp.cpp — RESP 编解码实现。

// include 顺序：log.h 必须最先，理由见 platform.h 顶部注释。
#include "rl/log.h"

#include "rl/resp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace rl {

// ================================================================ 编码
void EncodeSimpleString(const std::string& s, std::string* out) {
    out->push_back('+');
    out->append(s);
    out->append(kCRLF);
}

void EncodeError(const std::string& msg, std::string* out) {
    out->push_back('-');
    out->append(msg);
    out->append(kCRLF);
}

void EncodeInteger(int64_t v, std::string* out) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    out->push_back(':');
    out->append(buf, static_cast<size_t>(n));
    out->append(kCRLF);
}

void EncodeBulkString(const std::string& s, std::string* out) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "$%llu", static_cast<unsigned long long>(s.size()));
    out->append(buf, static_cast<size_t>(n));
    out->append(kCRLF);
    out->append(s);
    out->append(kCRLF);
}

void EncodeNullBulk(std::string* out) { out->append("$-1\r\n"); }

void EncodeArrayHeader(size_t n, std::string* out) {
    char buf[32];
    int len = std::snprintf(buf, sizeof(buf), "*%llu", static_cast<unsigned long long>(n));
    out->append(buf, static_cast<size_t>(len));
    out->append(kCRLF);
}

void EncodeCommand(const std::vector<std::string>& args, std::string* out) {
    EncodeArrayHeader(args.size(), out);
    for (const std::string& a : args) {
        EncodeBulkString(a, out);
    }
}

// ================================================================ 解析工具
bool ParseLine(const char* data, size_t len, std::string* out) {
    out->assign(data, len);
    return true;
}

bool ParseInt64(const char* data, size_t len, int64_t* out) {
    if (len == 0 || len > 20) return false;

    size_t i = 0;
    bool neg = false;
    if (data[0] == '-') {
        neg = true;
        i = 1;
        if (len == 1) return false;
    } else if (data[0] == '+') {
        i = 1;
        if (len == 1) return false;
    }

    uint64_t acc = 0;
    for (; i < len; ++i) {
        char c = data[i];
        if (c < '0' || c > '9') return false;
        acc = acc * 10 + static_cast<uint64_t>(c - '0');
        if (acc > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            // 允许 INT64_MIN 的绝对值多 1。
            if (neg && acc == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1) {
                continue;
            }
            return false;
        }
    }

    if (neg) {
        if (acc == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1) {
            *out = std::numeric_limits<int64_t>::min();
            return true;
        }
        *out = -static_cast<int64_t>(acc);
        return true;
    }
    *out = static_cast<int64_t>(acc);
    return true;
}

std::string ToLowerAscii(const std::string& s) {
    std::string r = s;
    for (char& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

// ================================================================ GlobMatch
// 经典回溯实现。O(n*m) 最坏，但 KEYS 只用于调试，不在热路径上。
// 若要上生产应改成 DP 或限制 pattern 长度。
namespace {

bool GlobMatchImpl(const char* p, size_t plen, const char* s, size_t slen) {
    size_t pi = 0, si = 0;
    size_t star_p = std::string::npos, star_s = 0;

    while (si < slen) {
        if (pi < plen) {
            char pc = p[pi];
            switch (pc) {
                case '*': {
                    // 记录回溯点：* 先匹配空串，失败后再多吃一个字符。
                    star_p = pi++;
                    star_s = si;
                    continue;
                }
                case '?': {
                    ++pi;
                    ++si;
                    continue;
                }
                case '[': {
                    // 字符类 [abc] / [a-z] / [^abc]
                    size_t j = pi + 1;
                    bool negate = false;
                    if (j < plen && (p[j] == '^' || p[j] == '!')) {
                        negate = true;
                        ++j;
                    }
                    bool matched = false;
                    bool closed = false;
                    size_t k = j;
                    while (k < plen) {
                        if (p[k] == ']' && k > j) {
                            closed = true;
                            break;
                        }
                        if (k + 2 < plen && p[k + 1] == '-' && p[k + 2] != ']') {
                            char lo = p[k], hi = p[k + 2];
                            if (s[si] >= lo && s[si] <= hi) matched = true;
                            k += 3;
                        } else {
                            if (p[k] == s[si]) matched = true;
                            ++k;
                        }
                    }
                    if (!closed) {
                        // 未闭合的 '[' 按字面量处理，与 Redis 行为一致。
                        if (pc == s[si]) {
                            ++pi;
                            ++si;
                            continue;
                        }
                        break;
                    }
                    if (matched == negate) break;
                    pi = k + 1;
                    ++si;
                    continue;
                }
                case '\\': {
                    if (pi + 1 < plen) {
                        if (p[pi + 1] == s[si]) {
                            pi += 2;
                            ++si;
                            continue;
                        }
                        break;
                    }
                    if (pc == s[si]) {
                        ++pi;
                        ++si;
                        continue;
                    }
                    break;
                }
                default: {
                    if (pc == s[si]) {
                        ++pi;
                        ++si;
                        continue;
                    }
                    break;
                }
            }
        }

        // 本字符未匹配：若之前出现过 '*'，回退到那个 '*' 并让它多吃一个字符。
        if (star_p != std::string::npos) {
            pi = star_p + 1;
            si = ++star_s;
            continue;
        }
        return false;
    }

    while (pi < plen && p[pi] == '*') ++pi;
    return pi == plen;
}

}  // namespace

bool GlobMatch(const char* pattern, size_t plen, const char* str, size_t slen) {
    return GlobMatchImpl(pattern, plen, str, slen);
}

// ================================================================ RespParser
namespace {
// 在 [start, end) 中找第一个 CRLF，返回其偏移，找不到返回 npos。
size_t FindCRLF(const char* data, size_t len, size_t start) {
    if (len < kCRLFLen) return std::string::npos;
    for (size_t i = start; i + 1 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n') return i;
    }
    return std::string::npos;
}
}  // namespace

void RespParser::Append(const char* data, size_t len) {
    if (len == 0) return;
    buf_.append(data, len);
}

bool RespParser::Ensure(size_t n) const { return buf_.size() - pos_ >= n; }

bool RespParser::ParseValue(std::string* out, int depth) {
    if (depth > 8) {
        throw ProtocolException("nested array too deep");
    }
    if (!Ensure(1)) return false;

    switch (cursor()[0]) {
        case '+': {
            // 简单字符串
            size_t off = FindCRLF(buf_.data(), buf_.size(), pos_ + 1);
            if (off == std::string::npos) return false;
            out->assign(buf_.data() + pos_ + 1, off - pos_ - 1);
            pos_ = off + kCRLFLen;
            return true;
        }
        case '$': {
            // 批量字符串
            size_t off = FindCRLF(buf_.data(), buf_.size(), pos_ + 1);
            if (off == std::string::npos) return false;

            int64_t len = 0;
            if (!ParseInt64(buf_.data() + pos_ + 1, off - pos_ - 1, &len)) {
                throw ProtocolException("invalid bulk length");
            }
            if (len < -1 || len > static_cast<int64_t>(kMaxBulkSize)) {
                throw ProtocolException("bulk length out of range");
            }

            const size_t header = off + kCRLFLen;
            if (len == -1) {
                // RESP2 的 null bulk
                out->clear();
                pos_ = header;
                return true;
            }

            const size_t total = static_cast<size_t>(len) + kCRLFLen;
            if (buf_.size() - header < total) return false;  // 数据未到齐

            // 校验尾部 CRLF，脏数据要尽早发现而不是静默错位。
            if (buf_[header + static_cast<size_t>(len)] != '\r' ||
                buf_[header + static_cast<size_t>(len) + 1] != '\n') {
                throw ProtocolException("bulk not terminated by CRLF");
            }

            out->assign(buf_.data() + header, static_cast<size_t>(len));
            pos_ = header + total;
            return true;
        }
        case ':': {
            size_t off = FindCRLF(buf_.data(), buf_.size(), pos_ + 1);
            if (off == std::string::npos) return false;
            int64_t v = 0;
            if (!ParseInt64(buf_.data() + pos_ + 1, off - pos_ - 1, &v)) {
                throw ProtocolException("invalid integer");
            }
            *out = std::to_string(v);
            pos_ = off + kCRLFLen;
            return true;
        }
        default:
            throw ProtocolException("unsupported RESP type for inline parse");
    }
}

bool RespParser::SkipValue(int depth) {
    if (depth > 8) throw ProtocolException("nested array too deep");
    if (!Ensure(1)) return false;

    if (cursor()[0] == '*') {
        size_t off = FindCRLF(buf_.data(), buf_.size(), pos_ + 1);
        if (off == std::string::npos) return false;
        int64_t n = 0;
        if (!ParseInt64(buf_.data() + pos_ + 1, off - pos_ - 1, &n) || n < 0) {
            throw ProtocolException("invalid multibulk length");
        }
        pos_ = off + kCRLFLen;
        std::string tmp;
        for (int64_t i = 0; i < n; ++i) {
            if (!SkipValue(depth + 1)) return false;
        }
        return true;
    }
    std::string tmp;
    return ParseValue(&tmp, depth);
}

bool RespParser::Next(std::vector<std::string>* args) {
    args->clear();
    if (!Ensure(1)) return false;

    const char first = cursor()[0];

    // 兼容 inline command（方便用 telnet / nc 手测）：非 '*' 开头时按空格切分。
    if (first != '*') {
        size_t off = FindCRLF(buf_.data(), buf_.size(), pos_);
        if (off == std::string::npos) {
            // 防御：单行过长且始终没有 CRLF，视为协议错误。
            if (buffered() > kMaxInlineSize) {
                throw ProtocolException("inline command too long");
            }
            return false;
        }
        // 未消费任何字节前先记下起点，出错时状态不变。
        std::string line(buf_.data() + pos_, off - pos_);
        pos_ = off + kCRLFLen;

        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i >= line.size()) break;
            size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
            args->push_back(line.substr(start, i - start));
        }
        if (args->empty()) return false;  // 空行，忽略
        return true;
    }

    // 标准 RESP 数组请求
    const size_t saved = pos_;
    size_t off = FindCRLF(buf_.data(), buf_.size(), pos_ + 1);
    if (off == std::string::npos) return false;

    int64_t argc = 0;
    if (!ParseInt64(buf_.data() + pos_ + 1, off - pos_ - 1, &argc)) {
        throw ProtocolException("invalid multibulk length");
    }
    if (argc <= 0) {
        // *0 或 *-1：空请求，直接消费掉，调用方用空 args 表示"无命令"。
        pos_ = off + kCRLFLen;
        return true;
    }
    if (argc > 1024 * 1024) {
        throw ProtocolException("multibulk length out of range");
    }

    // 先只把 header 游标前移，再逐个解析元素。
    pos_ = off + kCRLFLen;

    // 逐个解析。注意：这里必须写进局部 vector，成功后才整体交换给 args。
    // 若中途数据不足就回滚 pos_，此时 args 必须保持「未被修改」的状态，
    // 否则调用方会以为自己拿到了半条命令 —— 这是增量解析最容易踩的坑。
    std::vector<std::string> parsed;
    parsed.reserve(static_cast<size_t>(argc));
    for (int64_t i = 0; i < argc; ++i) {
        std::string arg;
        if (!ParseValue(&arg, 0)) {
            pos_ = saved;  // 整体回滚，等下次可读重新解析
            return false;
        }
        parsed.push_back(std::move(arg));
    }

    *args = std::move(parsed);
    return true;
}

}  // namespace rl
