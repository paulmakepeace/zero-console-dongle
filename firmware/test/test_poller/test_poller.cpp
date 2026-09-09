// The poller's line state machine and batch rules, on the host. The MBB's
// side is a script of lines; time is a variable the test steps. Everything
// the module calls outward is defined here, so the module itself is under
// test and nothing inside it is stubbed.
#include <unity.h>
#include <string>
#include <vector>
#include "config.h"
#include "mbb_uart.h"
#include "store.h"
#include "clock.h"
#include "readings.h"
#include <LittleFS.h>

uint32_t testMillis = 1000;
LittleFSClass LittleFS;

// What the poller calls outward, defined before the module so the linker has
// them: this is the seam, and nothing inside the module is replaced.
static std::vector<std::string> sent;
static std::vector<std::string> logged;
static bool txHeld = false;
static uint32_t lastByteMs = 0;
static bool clockOk = true;

size_t mbbWrite(const uint8_t* d, size_t n) {
    std::string s((const char*)d, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    sent.push_back(s);
    return n;
}
void mbbTxHold(bool on) { txHeld = on; }
uint32_t mbbLastByteMs() { return lastByteMs; }
void storeTick(bool) {}
void storeAppend(const String& line, bool) { logged.push_back(line.s); }
String clockStamp() { return String("2026-09-09T00:00:00.000"); }
bool clockValid() { return clockOk; }
void readingsFeed(const char*, size_t, long) {}

// The module itself, compiled into the test: its statics are its own.
#include "poller.cpp"

// The module keeps its state in file statics, which live for the whole run,
// and pollerBegin does not clear them. Compiling it into the test puts them
// in scope, so each test starts from a known board rather than the last
// test's leftovers.
static void resetModule() {
    running = false; finishAfterThis = false; requested = false; requestSkipsSettle = false;
    cur = -1; watch = -1; expectEcho = false; consoleOn = false;
    buf = String(); quietWaitMs = 0; lastPollMs = 0; cmdStartedMs = 0; watchStartedMs = 0;
    wasAwake = false; awakeSinceMs = 0; mbbStopping = false; stoppingSinceMs = 0;
    saveDue = false; soc = -1; havePack = false; bikeState[0] = 0;
    for (int i = 0; i < NCMD; i++) {
        outputs[i] = String(); outputAtMs[i] = 0; outputEpoch[i] = 0; outputOk[i] = false; failedAtMs[i] = 0;
    }
}

void setUp() {
    testMillis = 1000; clockOk = true;
    sent.clear(); logged.clear(); txHeld = false; lastByteMs = 0;
    LittleFS.files.clear(); LittleFS.full = false;
    resetModule();
    // The schedule off by default: a test that wants a batch asks for one, so
    // nothing starts behind its back. The board's own schedule is the subject
    // of its own test.
    pollerBegin(0);
}
void tearDown() {}

static void advance(uint32_t ms) { testMillis += ms; }
static void feed(const char* line) { pollerConsumeLine(line, strlen(line)); }
static void tick(bool awake = true, bool console = false) { pollerTick(awake, console); }
static void quiet() { lastByteMs = testMillis > 3000 ? testMillis - 3000 : 0; }
static void answer(const char* body) {
    feed(sent.back().c_str());
    feed(body ? body : "some output");
    feed("ZERO MBB> ");
}
static void runWholeBatch(const char* body = nullptr) {
    for (int i = 0; i < 40 && pollerActive(); i++) {
        if (sent.empty()) break;
        answer(body);
        advance(10);
        tick();
    }
}
// A batch may start only once the MBB has been awake past the settle, and
// only on a line that has been quiet: the same preconditions the board has.
static void wakeAndSettle() {
    quiet(); tick();                       // the awake edge
    advance(POLL_SETTLE_MS + 1000);
    quiet(); tick();
}
static void startBatch() {
    wakeAndSettle();
    quiet(); pollerRequest();
    advance(10); quiet(); tick();
}

void test_a_batch_runs_every_command_in_order_and_keeps_each_answer() {
    startBatch();
    TEST_ASSERT_TRUE(pollerActive());
    TEST_ASSERT_TRUE(txHeld);
    runWholeBatch("Bike State: STOP");
    TEST_ASSERT_FALSE(pollerActive());
    TEST_ASSERT_FALSE(txHeld);
    TEST_ASSERT_EQUAL(13, (int)sent.size());
    TEST_ASSERT_EQUAL_STRING("status", sent[0].c_str());
    TEST_ASSERT_EQUAL_STRING("performance", sent[12].c_str());
    const String* out = pollerOutput("status");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(out->s.find("Bike State: STOP") != std::string::npos);
    TEST_ASSERT_TRUE(out->s.find("status") == std::string::npos);
}

void test_a_stale_prompt_does_not_close_the_first_command() {
    startBatch();
    TEST_ASSERT_EQUAL(1, (int)sent.size());
    feed("ZERO MBB> status");
    feed("Bike State: STOP");
    feed("ZERO MBB> ");
    advance(10); tick();
    const String* out = pollerOutput("status");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(out->s.find("Bike State: STOP") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("charging", sent[1].c_str());
}

void test_a_late_prompt_after_a_timeout_does_not_shift_every_slot() {
    startBatch();
    feed(sent.back().c_str());
    advance(POLL_TIMEOUT_MS + 100); tick();
    TEST_ASSERT_EQUAL(2, (int)sent.size());
    feed("tail of the old answer");
    feed("ZERO MBB> charging");
    feed("Charging State: 1");
    feed("ZERO MBB> ");
    advance(10); tick();
    const String* charging = pollerOutput("charging");
    TEST_ASSERT_NOT_NULL(charging);
    TEST_ASSERT_TRUE(charging->s.find("Charging State: 1") != std::string::npos);
    TEST_ASSERT_TRUE(charging->s.find("tail of the old answer") == std::string::npos);
}

void test_a_console_client_typing_a_poll_command_has_its_answer_kept() {
    wakeAndSettle();
    quiet(); tick(true, true);
    TEST_ASSERT_FALSE(pollerActive());
    feed("bms");
    feed("soc 76");
    feed("ZERO MBB> ");
    const String* out = pollerOutput("bms");
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE(out->s.find("soc 76") != std::string::npos);
    TEST_ASSERT_EQUAL(0, (int)sent.size());
}

void test_two_commands_typed_back_to_back_are_both_kept() {
    wakeAndSettle();
    quiet(); tick(true, true);
    feed("pdu");
    feed("Total_Current, 1");
    feed("ZERO MBB> in");
    feed("12V_Battery, 13127");
    feed("ZERO MBB> ");
    const String* pdu = pollerOutput("pdu");
    const String* in = pollerOutput("in");
    TEST_ASSERT_NOT_NULL(pdu);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_TRUE(pdu->s.find("Total_Current") != std::string::npos);
    TEST_ASSERT_TRUE(in->s.find("12V_Battery") != std::string::npos);
    TEST_ASSERT_TRUE(in->s.find("Total_Current") == std::string::npos);
}

void test_a_batch_waits_while_a_typed_command_is_being_answered() {
    wakeAndSettle();
    quiet(); tick(true, true);
    feed("bms");                       // a watch opens
    quiet(); pollerRequest();
    advance(1000); quiet(); tick(true, false);   // the client left, a poll is due
    TEST_ASSERT_FALSE(pollerActive());           // but the answer is still coming
    feed("soc 76");
    feed("ZERO MBB> ");
    advance(10); quiet(); tick();
    TEST_ASSERT_TRUE(pollerActive());            // now it may start
}

void test_a_typed_command_the_mbb_never_answers_does_not_hold_the_batch_off() {
    wakeAndSettle();
    quiet(); tick(true, true);
    feed("bms");                       // a watch opens and no answer ever comes
    quiet(); pollerRequest();
    advance(POLL_TIMEOUT_MS + 500); quiet(); tick(true, false);
    TEST_ASSERT_TRUE(pollerActive());  // the watch went stale and the batch ran
    TEST_ASSERT_NULL(pollerOutput("bms"));   // and kept nothing from it
}

void test_an_output_line_equal_to_a_command_name_does_not_open_a_watch() {
    wakeAndSettle();
    quiet(); tick(true, true);
    feed("  in");
    feed("something");
    feed("ZERO MBB> ");
    TEST_ASSERT_NULL(pollerOutput("in"));
}

void test_the_batch_ends_when_the_mbb_sleeps_and_keeps_what_it_had() {
    startBatch();
    answer("Bike State: STOP");
    advance(10); tick();
    TEST_ASSERT_TRUE(pollerActive());
    tick(false);
    TEST_ASSERT_FALSE(pollerActive());
    TEST_ASSERT_FALSE(txHeld);
    TEST_ASSERT_NOT_NULL(pollerOutput("status"));
}

void test_the_schedule_starts_a_batch_once_the_settle_has_passed() {
    pollerSetInterval(60);
    quiet(); tick();                 // the awake edge
    advance(POLL_SETTLE_MS - 1000);
    quiet(); tick();
    TEST_ASSERT_FALSE(pollerActive());   // still inside the settle
    advance(2000);
    quiet(); tick();
    TEST_ASSERT_TRUE(pollerActive());
}

void test_a_request_repeated_while_a_batch_runs_does_not_queue_another() {
    startBatch();
    TEST_ASSERT_TRUE(pollerActive());
    pollerRequest(); pollerRequest();
    runWholeBatch();
    int after = (int)sent.size();
    advance(1000); quiet(); tick();
    TEST_ASSERT_EQUAL(after, (int)sent.size());
}

void test_the_saved_batch_round_trips_including_names_with_a_space() {
    startBatch();
    runWholeBatch("a line of output");
    tick(false);
    TEST_ASSERT_TRUE(LittleFS.exists(String("/") + POLL_SAVE_NAME));
    pollerBegin(60);
    TEST_ASSERT_NOT_NULL(pollerOutput("status"));
    TEST_ASSERT_NOT_NULL(pollerOutput("bms interface"));
    TEST_ASSERT_NOT_NULL(pollerOutput("dash info"));
    TEST_ASSERT_NOT_NULL(pollerOutput("performance"));
}

void test_a_full_flash_keeps_the_previous_save() {
    startBatch();
    runWholeBatch("first");
    tick(false);
    std::string good = LittleFS.files[std::string("/") + POLL_SAVE_NAME];
    TEST_ASSERT_TRUE(good.size() > 0);
    LittleFS.full = true;
    startBatch();
    runWholeBatch("second");
    tick(false);
    TEST_ASSERT_EQUAL_STRING(good.c_str(), LittleFS.files[std::string("/") + POLL_SAVE_NAME].c_str());
}

void test_an_unsolicited_line_inside_an_answer_is_not_kept_as_output() {
    startBatch();
    feed(sent.back().c_str());
    feed("DEBUG:   09/09/2026 00:00:00.000  something the MBB said on its own");
    feed("Bike State: STOP");
    feed("ZERO MBB> ");
    advance(10); tick();
    TEST_ASSERT_TRUE(pollerOutput("status")->s.find("Bike State: STOP") != std::string::npos);
    TEST_ASSERT_TRUE(pollerOutput("status")->s.find("something the MBB said") == std::string::npos);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_batch_runs_every_command_in_order_and_keeps_each_answer);
    RUN_TEST(test_a_stale_prompt_does_not_close_the_first_command);
    RUN_TEST(test_a_late_prompt_after_a_timeout_does_not_shift_every_slot);
    RUN_TEST(test_a_console_client_typing_a_poll_command_has_its_answer_kept);
    RUN_TEST(test_two_commands_typed_back_to_back_are_both_kept);
    RUN_TEST(test_a_batch_waits_while_a_typed_command_is_being_answered);
    RUN_TEST(test_a_typed_command_the_mbb_never_answers_does_not_hold_the_batch_off);
    RUN_TEST(test_an_output_line_equal_to_a_command_name_does_not_open_a_watch);
    RUN_TEST(test_the_batch_ends_when_the_mbb_sleeps_and_keeps_what_it_had);
    RUN_TEST(test_the_schedule_starts_a_batch_once_the_settle_has_passed);
    RUN_TEST(test_a_request_repeated_while_a_batch_runs_does_not_queue_another);
    RUN_TEST(test_the_saved_batch_round_trips_including_names_with_a_space);
    RUN_TEST(test_a_full_flash_keeps_the_previous_save);
    RUN_TEST(test_an_unsolicited_line_inside_an_answer_is_not_kept_as_output);
    return UNITY_END();
}
