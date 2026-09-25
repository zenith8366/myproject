/* vibepet_proto.h 的离线测试 —— 不需要硬件，在电脑上跑。
 *
 * 为什么要有这个文件：设备端的协议解析（行重组 + 取 JSON 字段）是整个固件里
 * 最容易出错、又最难在真机上调试的一段 —— 它出错的表现往往是「屏幕上什么都
 * 没有」，看不出是解析错了、还是根本没收到、还是设备死了。
 *
 * 好在解析内核是纯计算、不碰硬件的，所以可以用一个假的 Arduino.h
 * （tools/proto_test/Arduino.h）把它搬到电脑上编译，跑一张表驱动测试。
 *
 * 跑法（在仓库根目录）：
 *     g++ -I tools/proto_test -I firmware/VibePet_UNO tools/test_proto.cpp -o proto_test \
 *         && ./proto_test
 * 退出码 0 = 全通过；非 0 = 有用例失败（失败的那条会打印期望与实际）。
 */

#include <stdio.h>
#include <string.h>

#include "vibepet_proto.h"

static int checks = 0;
static int failures = 0;

static void expect(bool ok, const char *name, const char *detail) {
    checks++;
    if (ok) {
        printf("  [\xe9\x80\x9a\xe8\xbf\x87] %s\n", name);   /* 通过 */
        return;
    }
    failures++;
    printf("  [\xe5\xa4\xb1\xe8\xb4\xa5] %s    %s\n", name, detail ? detail : "");
}

static void expect_str(bool found, const char *got, const char *want, const char *name) {
    if (!found) {
        expect(false, name, "\xe6\xb2\xa1\xe6\x89\xbe\xe5\x88\xb0\xe5\xad\x97\xe6\xae\xb5");
        return;
    }
    if (strcmp(got, want) != 0) {
        char detail[256];
        snprintf(detail, sizeof(detail), "\xe6\x9c\x9f\xe6\x9c\x9b %s\xef\xbc\x8c\xe5\xae\x9e\xe9\x99\x85 %s",
                 want, got);
        expect(false, name, detail);
        return;
    }
    expect(true, name, NULL);
}

/* 取一个字符串字段到 buf；返回是否找到 */
static bool scan(const char *line, const char *key, char *buf, size_t cap) {
    VpSink sink;
    vpSinkInit(&sink, buf, (uint16_t)cap);
    return vpJsonScan(line, key, &sink);
}

/* ─────────────── 一、取字段 ─────────────── */

static void test_basic_fields(void) {
    char buf[64];
    const char *line = "{\"type\":\"state\",\"status\":\"working\",\"msg\":\"hi\"}";

    expect(vpJsonKeyIs(line, "type", "state"), "type 等于 state", NULL);
    expect(!vpJsonKeyIs(line, "type", "button"), "type 不等于 button", NULL);
    expect(!vpJsonKeyIs(line, "type", "stat"), "前缀不算相等（stat ≠ state）", NULL);
    expect(vpJsonKeyIs(line, "status", "working"), "status 等于 working", NULL);
    expect(vpJsonKeyIs(line, "status", "workingx") == false, "长的也不算相等", NULL);

    expect_str(scan(line, "msg", buf, sizeof(buf)), buf, "hi", "取到 msg");
    expect_str(scan(line, "status", buf, sizeof(buf)), buf, "working", "取到 status");

    /* 字段顺序颠倒也要能取到（协议不保证顺序） */
    expect_str(scan("{\"msg\":\"first\",\"type\":\"state\"}", "msg", buf, sizeof(buf)),
               buf, "first", "字段顺序无关");

    /* 不存在的字段：返回 false，且落点被清空 */
    buf[0] = 'X';
    expect(!scan(line, "summary", buf, sizeof(buf)), "不存在的字段返回 false", NULL);
    expect(buf[0] == '\0', "取不到字段时落点被清空", NULL);

    /* 值不是字符串 → 不认 */
    expect(!scan("{\"msg\":123}", "msg", buf, sizeof(buf)), "非字符串的值不认", NULL);

    /* 重复的键：取第一个（协议里不会出现，写下来免得以后改错） */
    expect_str(scan("{\"msg\":\"one\",\"msg\":\"two\"}", "msg", buf, sizeof(buf)),
               buf, "one", "重复的键取第一个");
}

/* ─────────────── 二、转义 ─────────────── */

static void test_escapes(void) {
    char buf[128];

    /* Windows 路径：反斜杠写成两个 —— 这是最常见的一种 */
    expect_str(scan("{\"summary\":\"C:\\\\Users\\\\lyh35\"}", "summary", buf, sizeof(buf)),
               buf, "C:\\Users\\lyh35", "反斜杠还原成单个");

    /* 值里带引号 */
    expect_str(scan("{\"msg\":\"say \\\"hi\\\"\"}", "msg", buf, sizeof(buf)),
               buf, "say \"hi\"", "转义引号还原");

    /* 斜杠与制表符 */
    expect_str(scan("{\"msg\":\"a\\/b\"}", "msg", buf, sizeof(buf)),
               buf, "a/b", "转义斜杠还原");
    expect_str(scan("{\"msg\":\"a\\tb\"}", "msg", buf, sizeof(buf)),
               buf, "a b", "制表符换成空格（屏幕画不出制表符）");

    /* \uXXXX：Python 的 json.dumps 遇到控制字符会写成这样 */
    expect_str(scan("{\"msg\":\"\\u4e2d\\u6587\"}", "msg", buf, sizeof(buf)),
               buf, "\xe4\xb8\xad\xe6\x96\x87", "\\uXXXX 解码成 UTF-8（中文）");
    expect_str(scan("{\"msg\":\"\\u0009\"}", "msg", buf, sizeof(buf)),
               buf, " ", "\\u0009（制表符）变成空格");

    /* 代理对（emoji）：设备显示不了，统一变问号，绝不能崩 */
    expect_str(scan("{\"msg\":\"\\ud83d\\ude00\"}", "msg", buf, sizeof(buf)),
               buf, "?", "代理对（emoji）变问号");

    /* 没见过的转义：丢掉反斜杠、保留字符 —— 关键是别输出孤立的反斜杠 */
    expect_str(scan("{\"msg\":\"a\\qb\"}", "msg", buf, sizeof(buf)),
               buf, "aqb", "未知转义：丢反斜杠保留字符");

    /* 值里出现花括号和冒号（命令里带 JSON）不能被当成报文结尾 */
    expect_str(scan("{\"msg\":\"echo {\\\"a\\\":1}\",\"type\":\"state\"}", "msg", buf, sizeof(buf)),
               buf, "echo {\"a\":1}", "值里的花括号不误判为结尾");
}

/* ─────────────── 三、跳过嵌套结构 ─────────────── */

static void test_nested(void) {
    char buf[64];
    const char *line = "{\"tool_input\":{\"a\":{\"b\":1},\"c\":[1,2,3]},"
                       "\"type\":\"state\",\"msg\":\"after\"}";
    expect(vpJsonKeyIs(line, "type", "state"), "跳过嵌套对象后仍能取到后面的字段", NULL);
    expect_str(scan(line, "msg", buf, sizeof(buf)), buf, "after", "嵌套之后的字段");

    /* 数组里带字符串和括号 */
    const char *tricky = "{\"arr\":[\"}\",{\"x\":\"[\"}],\"msg\":\"ok\"}";
    expect_str(scan(tricky, "msg", buf, sizeof(buf)), buf, "ok",
               "数组里带引号括号也不迷路");
}

/* ─────────────── 四、UTF-8 边界安全的截断 ─────────────── */

static void test_truncation(void) {
    char small[6];
    /* "中中中" 是 9 字节，落点只有 6 字节 → 只能放 1 个汉字（3 字节）+ 结尾 */
    expect_str(scan("{\"msg\":\"\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\"}",
                    "msg", small, sizeof(small)),
               small, "\xe4\xb8\xad", "放不下时只留完整的汉字（不切出半个）");
    expect(strlen(small) == 3, "留下的确实是完整的 3 字节汉字", NULL);

    char exact[5];
    /* 5 字节的落点：3 字节汉字 + 结尾正好用满 */
    expect_str(scan("{\"msg\":\"\xe4\xb8\xad\xe4\xb8\xad\"}", "msg", exact, sizeof(exact)),
               exact, "\xe4\xb8\xad", "容量正好够一个字时不越界");
}

/* ─────────────── 五、行重组 ─────────────── */

static void feed_str(VpLine *line, const char *text) {
    for (const char *p = text; *p; p++) vpLineFeed(line, *p);
}

static void test_line_assembly(void) {
    static VpLine line;   /* 320 字节，别放栈上 */

    /* 一条行分三次喂进去（模拟串口分片） */
    vpLineInit(&line);
    feed_str(&line, "{\"type\":\"state\",\"msg\":\"\xe4\xb8\xad");
    expect(!line.ready, "半截行不算一条消息", NULL);
    feed_str(&line, "\xe6\x96\x87\"}");
    expect(!line.ready, "还是半截（差换行）", NULL);
    vpLineFeed(&line, '\n');
    expect(line.ready, "收到换行才算一条", NULL);
    expect_str(true, line.buf, "{\"type\":\"state\",\"msg\":\"\xe4\xb8\xad\xe6\x96\x87\"}",
               "分片重组后内容完整、不乱码");

    /* 派发完之后只摘旗，缓冲里的内容要留着（下一条消息的开头可能已经进来了） */
    vpLineConsume(&line);
    expect(!line.ready, "消费后旗子被摘掉", NULL);
    feed_str(&line, "{\"a\":1}");
    expect(!line.ready, "下一行还没攒够", NULL);
    expect(line.len == 7, "换行前攒到的字节数正确", NULL);
    vpLineFeed(&line, '\n');
    expect(line.ready && line.len == 0, "攒够后长度归零、旗子立起", NULL);

    /* 超长的一行：整条丢弃，而且必须只丢到行尾为止 */
    vpLineInit(&line);
    for (int i = 0; i < VP_LINE_MAX + 50; i++) vpLineFeed(&line, 'x');
    expect(!line.ready && line.dropping, "超长行进入丢弃状态", NULL);
    vpLineFeed(&line, '\n');
    expect(!line.ready && !line.dropping, "丢弃在行尾复位、不产生消息", NULL);
    feed_str(&line, "{\"type\":\"heartbeat\"}");
    vpLineFeed(&line, '\n');
    expect(line.ready, "丢弃之后的下一行照常工作", NULL);
    expect_str(true, line.buf, "{\"type\":\"heartbeat\"}", "下一行内容正确");

    /* 空行：算一条空消息，解析时自然取不到字段（不该崩） */
    vpLineInit(&line);
    vpLineFeed(&line, '\n');
    expect(line.ready && line.len == 0, "空行也算一条（内容为空）", NULL);
    char buf[8];
    expect(!scan(line.buf, "type", buf, sizeof(buf)), "空行里取不到字段，安全返回", NULL);
}

/* ─────────────── 六、接近真实的报文 ─────────────── */

static void test_real_messages(void) {
    char buf[200];
    /* 一条最长的审批请求（工具名 24 字节 + 摘要 160 字节） */
    static char long_line[512];
    char summary[400];
    memset(summary, 'a', sizeof(summary));
    summary[159] = '\0';    /* 159 个 ASCII 字符 = 159 字节 */
    snprintf(long_line, sizeof(long_line),
             "{\"type\":\"approval_request\",\"request_id\":\"ab12cd34\","
             "\"tool\":\"mcp__github__create_issue\",\"summary\":\"%s\"}", summary);
    expect(vpJsonKeyIs(long_line, "type", "approval_request"), "最长报文：取到 type", NULL);
    expect_str(scan(long_line, "request_id", buf, sizeof(buf)), buf, "ab12cd34",
               "最长报文：取到 request_id");
    expect_str(scan(long_line, "tool", buf, sizeof(buf)), buf, "mcp__github__create_issue",
               "最长报文：取到 tool");
    expect(scan(long_line, "summary", buf, sizeof(buf)) && strlen(buf) == 159,
           "最长报文：取到完整摘要", NULL);

    /* 心跳 */
    expect(vpJsonKeyIs("{\"type\":\"heartbeat\",\"seq\":12345}", "type", "heartbeat"),
           "心跳：取到 type", NULL);

    /* 带中文摘要的审批请求 */
    const char *cn = "{\"type\":\"approval_request\",\"request_id\":\"ff00ff00\","
                     "\"tool\":\"Bash\",\"summary\":\"rm -rf /tmp/\xe7\xbc\x93\xe5\xad\x98\"}";
    expect_str(scan(cn, "summary", buf, sizeof(buf)), buf,
               "rm -rf /tmp/\xe7\xbc\x93\xe5\xad\x98", "中文摘要完整取出");

    /* 坏报文：不是对象 / 键没引号 / 缺冒号 → 都必须安全返回 false */
    expect(!vpJsonKeyIs("not json at all", "type", "state"), "非 JSON 安全返回", NULL);
    expect(!vpJsonKeyIs("{type:\"state\"}", "type", "state"), "键没引号 → 不认", NULL);
    expect(!vpJsonKeyIs("{\"type\" \"state\"}", "type", "state"), "缺冒号 → 不认", NULL);
    /* 少个右花括号：要的字段本身是完整的（键、冒号、成对引号都在），所以仍然认
       —— 这是**故意**的宽容：解析只看它要的那一个字段，不为整份报文做完整校验。
       真机上这种行根本到不了这里（行重组要求先收到换行才算一条消息）。 */
    expect(vpJsonKeyIs("{\"type\":\"state\"", "type", "state"),
           "缺右花括号但字段完整 → 宽容接受", NULL);
    /* 但值本身残缺（少了结尾引号）必须拒绝 */
    expect(!vpJsonKeyIs("{\"type\":\"sta", "type", "state"), "值残缺 → 拒绝", NULL);
}

int main(void) {
    printf("vibepet_proto 离线测试\n");
    printf("\n一、取字段\n");
    test_basic_fields();
    printf("\n二、转义\n");
    test_escapes();
    printf("\n三、嵌套\n");
    test_nested();
    printf("\n四、截断\n");
    test_truncation();
    printf("\n五、行重组\n");
    test_line_assembly();
    printf("\n六、接近真实的报文\n");
    test_real_messages();

    printf("\n==================================================\n");
    if (failures == 0) {
        printf("%d/%d \xe5\x85\xa8\xe9\x83\xa8\xe9\x80\x9a\xe8\xbf\x87\n", checks, checks);
        return 0;
    }
    printf("%d/%d \xe9\x80\x9a\xe8\xbf\x87\xef\xbc\x8c%d \xe6\x9d\xa1\xe5\xa4\xb1\xe8\xb4\xa5\n",
           checks - failures, checks, failures);
    return 1;
}
