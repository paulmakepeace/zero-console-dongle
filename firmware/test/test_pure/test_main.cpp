// Host-side tests for the pure layer: pio test -e native
#include <unity.h>
#include <string>
#include <vector>
#include "mbb_time.h"
#include "framer.h"
#include "names.h"
#include "json_escape.h"
#include "commit_account.h"

void setUp() {}
void tearDown() {}

// --- mbb_time -------------------------------------------------------------
static bool parse(const char* s, MbbStamp& t) { return parseMbbStamp(s, strlen(s), t); }

void test_stamp_plain_and_debug_prefixed() {
    MbbStamp t;
    TEST_ASSERT_TRUE(parse("09/07/2026 19:55:00.123 hello", t));
    TEST_ASSERT_EQUAL(2026, t.year); TEST_ASSERT_EQUAL(9, t.month); TEST_ASSERT_EQUAL(7, t.day);
    TEST_ASSERT_EQUAL(19, t.hour); TEST_ASSERT_EQUAL(55, t.minute); TEST_ASSERT_EQUAL(0, t.second); TEST_ASSERT_EQUAL(123, t.ms);
    TEST_ASSERT_TRUE(parse("DEBUG:   09/07/2026 21:06:37.425  ../src/x.c : line 1499", t));
    TEST_ASSERT_EQUAL(21, t.hour);
}

void test_stamp_only_at_line_start() {
    MbbStamp t;
    TEST_ASSERT_FALSE(parse("Torque: 0 09/07/2026 19:55:00.123", t));
    TEST_ASSERT_FALSE(parse("dongle: 09/07/2026 19:55:00.123", t));
    TEST_ASSERT_FALSE(parse("", t));
    TEST_ASSERT_FALSE(parse("09/07/2026 19:55:00.12", t));   // one digit short
}

void test_stamp_rejects_impossible_fields() {
    MbbStamp t;
    TEST_ASSERT_FALSE(parse("13/07/2026 19:55:00.123", t));
    TEST_ASSERT_FALSE(parse("02/30/2026 19:55:00.123", t));   // no rollover into March
    TEST_ASSERT_TRUE(parse("02/29/2028 00:00:00.000", t));    // leap day
    TEST_ASSERT_FALSE(parse("02/29/2026 00:00:00.000", t));
    TEST_ASSERT_FALSE(parse("09/07/2026 24:00:00.000", t));
    TEST_ASSERT_FALSE(parse("09/07/2019 19:55:00.123", t));   // before the range: a log dump of old dates
    TEST_ASSERT_FALSE(parse("09/07/2041 19:55:00.123", t));
    TEST_ASSERT_FALSE(parse("09/0x/2026 19:55:00.123", t));
}

void test_consensus_needs_two_agreeing_stamps() {
    StampConsensus c;
    TEST_ASSERT_FALSE(c.offer(1000, 0));
    TEST_ASSERT_TRUE(c.offer(1002, 2000));      // 2 s later, 2 s further on
    TEST_ASSERT_FALSE(c.offer(5000, 3000));     // a fresh candidate after a confirmation
    TEST_ASSERT_FALSE(c.offer(9000, 4000));     // disagrees: replaces the candidate
    TEST_ASSERT_TRUE(c.offer(9010, 14000));     // agrees with the replacement
}

void test_consensus_tolerance_and_reset() {
    StampConsensus c;
    c.offer(1000, 0);
    TEST_ASSERT_FALSE(c.offer(1005, 0));        // 5 s off is outside +-5
    c.offer(1000, 0);
    TEST_ASSERT_TRUE(c.offer(1004, 0));
    c.offer(1000, 0);
    c.reset();
    TEST_ASSERT_FALSE(c.offer(1000, 0));
}

// --- framer ---------------------------------------------------------------
static std::vector<std::string> lines;
static void collect(const char* l, size_t n) { lines.push_back(std::string(l, n)); }

void test_framer_splits_and_strips() {
    lines.clear();
    LineFramer<32> f;
    const char* in = "one\r\ntwo\n\0three";
    f.feed((const uint8_t*)in, 15, collect);
    TEST_ASSERT_EQUAL(2, lines.size());
    TEST_ASSERT_EQUAL_STRING("one", lines[0].c_str());
    TEST_ASSERT_EQUAL_STRING("two", lines[1].c_str());
    TEST_ASSERT_TRUE(f.flush(collect));
    TEST_ASSERT_EQUAL_STRING("three", lines[2].c_str());
    TEST_ASSERT_FALSE(f.flush(collect));
}

void test_framer_survives_split_across_reads() {
    lines.clear();
    LineFramer<32> f;
    f.feed((const uint8_t*)"ab", 2, collect);
    f.feed((const uint8_t*)"c\nd", 3, collect);
    TEST_ASSERT_EQUAL(1, lines.size());
    TEST_ASSERT_EQUAL_STRING("abc", lines[0].c_str());
    TEST_ASSERT_EQUAL(1, f.len);
}

void test_framer_marks_an_overlong_line() {
    lines.clear();
    LineFramer<40> f;
    std::string in(60, 'x');
    in += "\n";
    f.feed((const uint8_t*)in.data(), in.size(), collect);
    TEST_ASSERT_EQUAL(2, lines.size());
    TEST_ASSERT_EQUAL(39, lines[0].size());
    TEST_ASSERT_EQUAL_STRING(" [dongle: line continues]", lines[0].substr(39 - 25).c_str());
    TEST_ASSERT_EQUAL(21, lines[1].size());   // 60 - 39 carried on
}

// --- names ----------------------------------------------------------------
static bool ok(const char* n) { return logNameOk(n, strlen(n)); }

void test_log_names() {
    TEST_ASSERT_TRUE(ok("b0102-001-20260907-202357.log"));
    TEST_ASSERT_TRUE(ok("b0011-02-nosync.log"));
    TEST_ASSERT_FALSE(ok(""));
    TEST_ASSERT_FALSE(ok(".hidden"));
    TEST_ASSERT_FALSE(ok("-flag"));
    TEST_ASSERT_FALSE(ok("a/b.log"));
    TEST_ASSERT_FALSE(ok("a..b.log"));
    TEST_ASSERT_FALSE(ok("a b.log"));
    std::string longName(65, 'a');
    TEST_ASSERT_FALSE(ok(longName.c_str()));
    TEST_ASSERT_TRUE(ok(std::string(64, 'a').c_str()));
}

void test_session_name_sorts_by_creation() {
    char a[48], b[48], c[48];
    sessionName(a, sizeof a, 99, 12, "20260907-191951");
    sessionName(b, sizeof b, 100, 1, "nosync");
    sessionName(c, sizeof c, 100, 2, "20260907-194117");
    TEST_ASSERT_EQUAL_STRING("b0099-012-20260907-191951.log", a);
    TEST_ASSERT_TRUE(strcmp(a, b) < 0);
    TEST_ASSERT_TRUE(strcmp(b, c) < 0);
    TEST_ASSERT_TRUE(ok(a) && ok(b) && ok(c));
}

// --- json_escape ----------------------------------------------------------
static std::string esc(const std::string& s) { return jsonEscape(s.data(), s.size()); }

void test_json_escape() {
    TEST_ASSERT_EQUAL_STRING("plain", esc("plain").c_str());
    TEST_ASSERT_EQUAL_STRING("P\\\"W\\\\", esc("P\"W\\").c_str());
    TEST_ASSERT_EQUAL_STRING("a\\u000ab", esc("a\nb").c_str());
    TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", esc("caf\xc3\xa9").c_str());          // valid UTF-8 passes
    TEST_ASSERT_EQUAL_STRING("x\\u00ffy", esc("x\xffy").c_str());                // a lone byte is escaped
    TEST_ASSERT_EQUAL_STRING("\\u00c3", esc("\xc3").c_str());                    // truncated sequence
}

// --- commit_account -------------------------------------------------------
void test_commit_account_counts_loss_once() {
    CommitAccount a = commitAccount(100, 1100, 1100);
    TEST_ASSERT_EQUAL(100, a.kept); TEST_ASSERT_EQUAL(0, a.lostAtFlush);
    a = commitAccount(100, 1100, 1060);          // 40 bytes of the tail did not reach the flash
    TEST_ASSERT_EQUAL(60, a.kept); TEST_ASSERT_EQUAL(40, a.lostAtFlush);
    a = commitAccount(30, 1030, 900);            // more lost than written this time: nothing kept
    TEST_ASSERT_EQUAL(0, a.kept);
    const char* pending = "l1\nl2\nl3\n";
    TEST_ASSERT_EQUAL(3, countLines(pending, 9));
    TEST_ASSERT_EQUAL(2, countLines(pending + 3, 6));   // the lines after the kept prefix
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_stamp_plain_and_debug_prefixed);
    RUN_TEST(test_stamp_only_at_line_start);
    RUN_TEST(test_stamp_rejects_impossible_fields);
    RUN_TEST(test_consensus_needs_two_agreeing_stamps);
    RUN_TEST(test_consensus_tolerance_and_reset);
    RUN_TEST(test_framer_splits_and_strips);
    RUN_TEST(test_framer_survives_split_across_reads);
    RUN_TEST(test_framer_marks_an_overlong_line);
    RUN_TEST(test_log_names);
    RUN_TEST(test_session_name_sorts_by_creation);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_commit_account_counts_loss_once);
    return UNITY_END();
}
