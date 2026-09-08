// Host-side tests for the pure layer: pio test -e native
#include <unity.h>
#include <string>
#include <vector>
#include "mbb_time.h"
#include "framer.h"
#include "names.h"
#include "json_escape.h"
#include "commit_account.h"
#include "hibernate.h"
#include "mbb_parse.h"

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

// --- hibernate ------------------------------------------------------------
static long hib(const char* s) { return parseHibernateSeconds(s, strlen(s)); }

void test_hibernate_line() {
    TEST_ASSERT_EQUAL(3600, hib("Saving Stats, Hibernating for 3600 sec"));
    TEST_ASSERT_EQUAL(60, hib("Hibernating for 60 sec"));
    TEST_ASSERT_EQUAL(-1, hib("INFO: MBB will hibernate in under 30 seconds"));
    TEST_ASSERT_EQUAL(-1, hib("Hibernating for  sec"));
    TEST_ASSERT_EQUAL(-1, hib("Hibernating for 3600 min"));
}

void test_seconds_until_wake() {
    TEST_ASSERT_EQUAL(3600, secondsUntilMbbWake(false, 0, 0, 999999, 0, 6, 3600));   // nothing known: the fallback
    TEST_ASSERT_EQUAL(3600, secondsUntilMbbWake(true, 1000, 3600, 1000, 0, 6, 3600));   // announced just now
    TEST_ASSERT_EQUAL(3475, secondsUntilMbbWake(true, 1000, 3600, 1125, 0, 6, 3600));   // 125 s later, all awake
    TEST_ASSERT_EQUAL(-100, secondsUntilMbbWake(true, 0, 3600, 3700, 0, 6, 3600));      // overdue
    // 2000 s of that elapsed time were RC-clock sleep with no NTP since: assume 6% more really passed.
    TEST_ASSERT_EQUAL(3600 - 2500 - 120, secondsUntilMbbWake(true, 0, 3600, 2500, 2000, 6, 3600));
}

void test_sleep_chunks_land_before_the_mbb() {
    // 6% drift, 10 s lead, 600 s chunks, 30 s minimum.
    TEST_ASSERT_EQUAL(554, sleepChunk(3600, 10, 600, 6, 30));   // a long wait: one full chunk, shortened
    TEST_ASSERT_EQUAL(554, sleepChunk(700, 10, 600, 6, 30));
    TEST_ASSERT_EQUAL(147, sleepChunk(166, 10, 600, 6, 30));    // the bench case: up about 19 s early at worst
    TEST_ASSERT_EQUAL(0, sleepChunk(40, 10, 600, 6, 30));       // too short to be worth it
    TEST_ASSERT_EQUAL(0, sleepChunk(10, 10, 600, 6, 30));       // inside the lead
    TEST_ASSERT_EQUAL(0, sleepChunk(-5, 10, 600, 6, 30));       // overdue
    // Whatever the drift up to 6%, a chunk's real length stays under the time left.
    for (long until = 31; until < 5000; until += 7) {
        long s = sleepChunk(until, 10, 600, 6, 30);
        if (s) TEST_ASSERT_TRUE(s + s * 6 / 100 + 1 <= until);
    }
}

// --- mbb_parse ------------------------------------------------------------
void test_prompt_and_unsolicited() {
    TEST_ASSERT_TRUE(isPrompt("ZERO MBB> ", 10));
    TEST_ASSERT_TRUE(isPrompt("ZERO MBB>", 9));
    TEST_ASSERT_FALSE(isPrompt("ZERO MBB", 8));
    TEST_ASSERT_FALSE(isPrompt(" ZERO MBB>", 10));
    const char* a = "DEBUG:   09/07/2026 21:58:20.935  x.c : line 650 - Control flags changed";
    TEST_ASSERT_TRUE(isUnsolicited(a, strlen(a)));
    const char* b = "09/07/2026 21:58:20.937 - State change from CHRG to STOP";
    TEST_ASSERT_TRUE(isUnsolicited(b, strlen(b)));
    TEST_ASSERT_TRUE(isUnsolicited("Disch limits: curr 819", 22));
    TEST_ASSERT_FALSE(isUnsolicited(" Bike State: CHRG", 17));
    TEST_ASSERT_FALSE(isUnsolicited("     - soc               86", 27));
}

void test_soc_and_bike_state() {
    const char* bms = "BMS 2 status data:\n - CONTACTOR_CLOSED\n     - pack voltage      108558\n     - soc               86\n     - precharge command 0\n";
    TEST_ASSERT_EQUAL(86, parseSoc(bms, strlen(bms)));
    const char* nosoc = " - pack voltage 1\n - society 5\n";
    TEST_ASSERT_EQUAL(-1, parseSoc(nosoc, strlen(nosoc)));   // "society" is not "soc" followed by a number
    char st[16];
    const char* state = "state\n\n Bike State: CHRG\n\n***** Inputs *****\n";
    TEST_ASSERT_EQUAL(4, parseBikeState(state, strlen(state), st, sizeof st));
    TEST_ASSERT_EQUAL_STRING("CHRG", st);
    TEST_ASSERT_EQUAL(0, parseBikeState("nothing here", 12, st, sizeof st));
    TEST_ASSERT_EQUAL_STRING("", st);
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
    RUN_TEST(test_hibernate_line);
    RUN_TEST(test_seconds_until_wake);
    RUN_TEST(test_sleep_chunks_land_before_the_mbb);
    RUN_TEST(test_prompt_and_unsolicited);
    RUN_TEST(test_soc_and_bike_state);
    return UNITY_END();
}
