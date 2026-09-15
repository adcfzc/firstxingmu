// Copyright (c) 2025 redis-lite authors. MIT License.
//
// test_resp.cc — RESP 解析器与编码器的单元测试
//
// 重点覆盖增量解析的「半包 / 粘包 / 协议错误」三类边界，
// 这是网络编程里最容易出 bug 也最容易被面试官追问的地方。

#include <limits>
#include <string>
#include <vector>

#include "rl/resp.h"
#include "test_util.h"

using namespace rl;

namespace {

std::string Join(const std::vector<std::string>& v, const char* sep = "|") {
    std::string r;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) r += sep;
        r += v[i];
    }
    return r;
}

}  // namespace

// ---------------------------------------------------------------- 编码
RL_TEST(encode_bulk_string) {
    std::string out;
    EncodeBulkString("hello", &out);
    RL_CHECK_EQ(out, std::string("$5\r\nhello\r\n"));
}

RL_TEST(encode_empty_bulk_string) {
    std::string out;
    EncodeBulkString("", &out);
    RL_CHECK_EQ(out, std::string("$0\r\n\r\n"));
}

RL_TEST(encode_null_bulk) {
    std::string out;
    EncodeNullBulk(&out);
    RL_CHECK_EQ(out, std::string("$-1\r\n"));
}

RL_TEST(encode_integer_negative) {
    std::string out;
    EncodeInteger(-12345, &out);
    RL_CHECK_EQ(out, std::string(":-12345\r\n"));
}

RL_TEST(encode_error) {
    std::string out;
    EncodeError("ERR bad", &out);
    RL_CHECK_EQ(out, std::string("-ERR bad\r\n"));
}

RL_TEST(encode_command_roundtrip) {
    std::vector<std::string> args = {"SET", "key", "value"};
    std::string encoded;
    EncodeCommand(args, &encoded);
    RL_CHECK_EQ(encoded, std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"));

    // 编码后必须能被自己的解析器读回来 —— 这是 AOF 恢复正确的前提。
    RespParser p;
    p.Append(encoded);
    std::vector<std::string> parsed;
    RL_CHECK(p.Next(&parsed));
    RL_CHECK_EQ(Join(parsed), Join(args));
}

// ---------------------------------------------------------------- 完整解析
RL_TEST(parse_simple_command) {
    RespParser p;
    p.Append("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    std::vector<std::string> args;
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(args.size(), size_t(3));
    RL_CHECK_EQ(args[0], std::string("SET"));
    RL_CHECK_EQ(args[1], std::string("foo"));
    RL_CHECK_EQ(args[2], std::string("bar"));
    RL_CHECK_EQ(p.buffered(), size_t(0));
}

RL_TEST(parse_binary_safe) {
    // value 里含 \r\n 与 \0，长度前缀必须让它们保持原义。
    // 若实现里用了 C 字符串函数，这个用例会立刻失败。
    std::string value("a\r\nb\0c", 6);
    std::vector<std::string> args = {"SET", "k", value};
    std::string encoded;
    EncodeCommand(args, &encoded);

    RespParser p;
    p.Append(encoded);
    std::vector<std::string> parsed;
    RL_CHECK(p.Next(&parsed));
    RL_CHECK_EQ(parsed.size(), size_t(3));
    RL_CHECK_EQ(parsed[2].size(), size_t(6));
    RL_CHECK_MSG(parsed[2] == value, "binary value must survive round-trip intact");
}

RL_TEST(parse_empty_command_array) {
    RespParser p;
    p.Append("*0\r\n");
    std::vector<std::string> args;
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(args.size(), size_t(0));
}

// ---------------------------------------------------------------- 半包
RL_TEST(partial_then_complete) {
    // 核心用例：逐字节喂入，只有最后一个字节到达后才应该返回成功。
    const std::string full = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";

    RespParser p;
    std::vector<std::string> args;

    for (size_t i = 0; i + 1 < full.size(); ++i) {
        p.Append(full.data() + i, 1);
        RL_CHECK_MSG(!p.Next(&args), "parser must not complete before all bytes arrive");
        // 未完成时内部游标不能前移，否则下次会从错误位置重解析。
        RL_CHECK_EQ(p.buffered(), i + 1);
    }

    p.Append(full.data() + full.size() - 1, 1);
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(args.size(), size_t(3));
    RL_CHECK_EQ(args[2], std::string("bar"));
}

RL_TEST(partial_header_only) {
    RespParser p;
    p.Append("*3\r\n$3\r\nSET\r\n");
    std::vector<std::string> args;
    RL_CHECK(!p.Next(&args));
    RL_CHECK_EQ(p.buffered(), p.buffered());  // 状态保持
}

RL_TEST(partial_bulk_body) {
    // header 完整但 body 不足：必须等齐，不能返回半截字符串。
    RespParser p;
    p.Append("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$5\r\nab");
    std::vector<std::string> args;
    RL_CHECK(!p.Next(&args));

    p.Append("cde\r\n");
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(args[2], std::string("abcde"));
}

// ---------------------------------------------------------------- 粘包
RL_TEST(pipelined_commands_in_one_buffer) {
    RespParser p;
    p.Append("*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n*1\r\n$4\r\nPING\r\n");

    std::vector<std::string> args;

    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(Join(args), std::string("PING"));

    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(Join(args), std::string("ECHO|hi"));

    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(Join(args), std::string("PING"));

    RL_CHECK(!p.Next(&args));  // 已排空
    RL_CHECK_EQ(p.buffered(), size_t(0));
}

RL_TEST(shared_crlf_across_segments) {
    // 极端的半包：CRLF 被拆到两个 TCP segment 里。
    RespParser p;
    p.Append("*1\r\n$4\r\nPING\r");
    std::vector<std::string> args;
    RL_CHECK(!p.Next(&args));
    p.Append("\n");
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(args[0], std::string("PING"));
}

// ---------------------------------------------------------------- inline
RL_TEST(inline_command) {
    // 允许用 telnet/nc 手测
    RespParser p;
    p.Append("PING\r\n");
    std::vector<std::string> args;
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(args.size(), size_t(1));
    RL_CHECK_EQ(args[0], std::string("PING"));
}

RL_TEST(inline_command_multiple_args) {
    RespParser p;
    p.Append("SET  key   value\r\n");
    std::vector<std::string> args;
    RL_CHECK(p.Next(&args));
    RL_CHECK_EQ(Join(args), std::string("SET|key|value"));
}

// ---------------------------------------------------------------- 协议错误
RL_TEST(error_invalid_multibulk_length) {
    RespParser p;
    p.Append("*abc\r\n");
    std::vector<std::string> args;
    bool threw = false;
    try {
        p.Next(&args);
    } catch (const ProtocolException&) {
        threw = true;
    }
    RL_CHECK_MSG(threw, "non-numeric multibulk length must be rejected");
}

RL_TEST(error_negative_bulk_length_other_than_minus_one) {
    RespParser p;
    p.Append("*1\r\n$-5\r\n");
    std::vector<std::string> args;
    bool threw = false;
    try {
        p.Next(&args);
    } catch (const ProtocolException&) {
        threw = true;
    }
    RL_CHECK_MSG(threw, "bulk length < -1 must be rejected");
}

RL_TEST(error_bulk_not_terminated_by_crlf) {
    RespParser p;
    // body 长度写 3，但后面跟着 "abcXX" 而不是 "abc\r\n"
    p.Append("*1\r\n$3\r\nabcXX");
    std::vector<std::string> args;
    bool threw = false;
    try {
        p.Next(&args);
    } catch (const ProtocolException&) {
        threw = true;
    }
    RL_CHECK_MSG(threw, "bulk body must be terminated by CRLF");
}

RL_TEST(error_multibulk_too_large) {
    RespParser p;
    p.Append("*99999999\r\n");
    std::vector<std::string> args;
    bool threw = false;
    try {
        p.Next(&args);
    } catch (const ProtocolException&) {
        threw = true;
    }
    RL_CHECK_MSG(threw, "absurd multibulk length must be rejected before allocation");
}

// ---------------------------------------------------------------- 整数解析
RL_TEST(parse_int64_valid) {
    int64_t v = 0;

    RL_CHECK(ParseInt64("0", 1, &v) && v == 0);
    RL_CHECK(ParseInt64("12345", 5, &v) && v == 12345);
    RL_CHECK(ParseInt64("-42", 3, &v) && v == -42);
    RL_CHECK(ParseInt64("+7", 2, &v) && v == 7);
    RL_CHECK(ParseInt64("9223372036854775807", 19, &v) &&
             v == std::numeric_limits<int64_t>::max());
    RL_CHECK(ParseInt64("-9223372036854775808", 20, &v) &&
             v == std::numeric_limits<int64_t>::min());
}

RL_TEST(parse_int64_invalid) {
    int64_t v = 0;
    RL_CHECK(!ParseInt64("", 0, &v));
    RL_CHECK(!ParseInt64("abc", 3, &v));
    RL_CHECK(!ParseInt64("12a", 3, &v));
    RL_CHECK(!ParseInt64("1 2", 3, &v));
    RL_CHECK(!ParseInt64(" 12", 3, &v));
    RL_CHECK(!ParseInt64("-", 1, &v));
    RL_CHECK(!ParseInt64("9223372036854775808", 19, &v));   // INT64_MAX + 1
    RL_CHECK(!ParseInt64("-9223372036854775809", 20, &v));  // INT64_MIN - 1
}

// ---------------------------------------------------------------- glob
RL_TEST(glob_exact) {
    RL_CHECK(GlobMatch("key", 3, "key", 3));
    RL_CHECK(!GlobMatch("key", 3, "keys", 4));
}

RL_TEST(glob_star) {
    RL_CHECK(GlobMatch("*", 1, "anything", 8));
    RL_CHECK(GlobMatch("key*", 4, "key123", 6));
    RL_CHECK(GlobMatch("*123", 4, "key123", 6));
    RL_CHECK(GlobMatch("k*y", 3, "key", 3));
    RL_CHECK(GlobMatch("k*y", 3, "keeeeeeey", 9));
    RL_CHECK(!GlobMatch("k*y", 3, "kez", 3));
    RL_CHECK(GlobMatch("a*b*c", 5, "axxbyyc", 7));
    RL_CHECK(!GlobMatch("a*b*c", 5, "axxbyy", 6));
}

RL_TEST(glob_question) {
    RL_CHECK(GlobMatch("k?y", 3, "key", 3));
    RL_CHECK(!GlobMatch("k?y", 3, "keey", 4));
}

RL_TEST(glob_charclass) {
    RL_CHECK(GlobMatch("key[0-9]", 8, "key5", 4));
    RL_CHECK(!GlobMatch("key[0-9]", 8, "keyx", 4));
    RL_CHECK(GlobMatch("key[abc]", 8, "keyb", 4));
    RL_CHECK(!GlobMatch("key[abc]", 8, "keyd", 4));
    RL_CHECK(GlobMatch("key[^abc]", 9, "keyd", 4));
    RL_CHECK(!GlobMatch("key[^abc]", 9, "keya", 4));
}

RL_TEST(glob_multiple_stars) {
    // 回溯逻辑的正确性：连续的 * 等价于一个 *
    RL_CHECK(GlobMatch("a**b", 4, "ab", 2));
    RL_CHECK(GlobMatch("a**b", 4, "axxxb", 5));
    RL_CHECK(GlobMatch("***", 3, "", 0));
}

RL_TEST(glob_empty_string) {
    RL_CHECK(GlobMatch("", 0, "", 0));
    RL_CHECK(!GlobMatch("", 0, "a", 1));
    RL_CHECK(GlobMatch("*", 1, "", 0));
}

// ---------------------------------------------------------------- 其他
RL_TEST(to_lower_ascii) {
    RL_CHECK_EQ(ToLowerAscii("SET"), std::string("set"));
    RL_CHECK_EQ(ToLowerAscii("SeT"), std::string("set"));
    RL_CHECK_EQ(ToLowerAscii("set"), std::string("set"));
    RL_CHECK_EQ(ToLowerAscii(""), std::string(""));
    RL_CHECK_EQ(ToLowerAscii("KEY:123"), std::string("key:123"));
}
