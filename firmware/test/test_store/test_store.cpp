// The store's reclaim and dictionary collection, on the host, against a
// temporary directory: the directory walk is the real readdir and stat, and
// free space is counted the way LittleFS counts it, in blocks with small files
// inline. Everything the module calls outward is defined here; the module
// itself is compiled in, so its statics are in scope and nothing is stubbed
// inside it.
#include <unity.h>
#include <string>
#include <vector>
#include <algorithm>
#include "config.h"
#include "clock.h"
#include "sys.h"
#include <LittleFS.h>
#include <Preferences.h>

uint32_t testMillis = 1000;
LittleFSClass LittleFS;

String clockStamp() { return String("2026-09-10T12:00:00.000"); }
bool clockValid() { return true; }
const char* clockSourceName() { return "test"; }
void sysFeedWatchdog() {}
const char* sysNodeName() { return "zero-dongle-test"; }

#include "store.cpp"

static const uint32_t DICT_A = 0x0000aaaa, DICT_B = 0x0000bbbb, DICT_C = 0x0000cccc;
static const size_t BIG = 20480;   // five blocks, a session's worth
static const size_t TINY = 100;    // inline in the directory: frees no block

// The module keeps its state in file statics for the run; each test starts
// from a formatted flash, an empty NVS and a fresh boot.
static void resetModule() {
    activeName = ""; active = File(); activeBytes = 0; seq = 0; sessionId = ""; sessionPart = 0;
    lastRotateMs = 0; lastReclaimMs = (uint32_t)0 - RECLAIM_GAP_MS - 1; lastOpenFailMs = (uint32_t)0 - RECLAIM_GAP_MS - 1; lastSpaceCheckMs = 0;
    dictId = 0; lastDictRefreshMs = 0; streamOpen = named = endWanted = pendingHasMbb = false;
    pendingLines = droppedLines = rawBytes = storedBytes = 0; awakeCount = 0;
    for (auto& n : openForRead) n = "";
    keeper.load((const uint8_t*)"", 0);
}

void setUp() {
    testMillis = 1000;
    LittleFS.clear(); LittleFS.full = false;
    Preferences::wipe();
    resetModule();
    TEST_ASSERT_TRUE(storeBegin("test"));
}
void tearDown() {}

static String logName(int s, uint32_t dict) { char b[48]; ::sessionName(b, sizeof b, 1, s, "20260910-120000", dict); return String(b); }
static void session(int s, size_t bytes, uint32_t dict = DICT_A) { LittleFS.put(pathOf(logName(s, dict)), bytes); }
static bool have(const String& name) { return LittleFS.exists(pathOf(name)); }
static std::vector<std::string> listing() {
    std::vector<std::string> v;
    forEachFile([&](const char* n, size_t) { v.push_back(n); }, false);
    std::sort(v.begin(), v.end());
    return v;
}
static String dictFile(uint32_t id) { char b[24]; snprintf(b, sizeof b, "dict-%08lx.txt", (unsigned long)id); return String(b); }

// Forty-one sessions of five blocks each, plus two directories' metadata,
// leave 61440 bytes free against a 98304 reserve: two files short.
static void fillPastTheReserve(int from = 1) { for (int s = from; s < from + 41; s++) session(s, BIG); }

void test_the_flash_is_counted_in_blocks() {
    session(1, BIG);
    session(2, TINY);
    TEST_ASSERT_EQUAL(2 * 8192 + 20480, LittleFS.usedBytes());   // the root and the log directory, one file's blocks, and nothing for the inline one
    TEST_ASSERT_EQUAL(0, blockBytes(TINY));
    TEST_ASSERT_EQUAL(BIG, blockBytes(BIG));
    TEST_ASSERT_EQUAL(4096, blockBytes(513));
}

void test_reclaim_deletes_the_oldest_until_the_reserve_is_back() {
    fillPastTheReserve();
    TEST_ASSERT_TRUE(freeBytes() < FS_MIN_FREE);
    TEST_ASSERT_TRUE(ensureSpace());
    TEST_ASSERT_TRUE(freeBytes() >= FS_MIN_FREE);
    TEST_ASSERT_FALSE(have(logName(1, DICT_A)));
    TEST_ASSERT_FALSE(have(logName(2, DICT_A)));
    TEST_ASSERT_TRUE(have(logName(3, DICT_A)));   // the minimum: two files, not the eight the walk picked
    TEST_ASSERT_EQUAL(39, listing().size());
    TEST_ASSERT_FALSE(ensureSpace());   // above the reserve: nothing to do
}

void test_reclaim_spares_the_active_file_and_one_being_read() {
    fillPastTheReserve();
    activeName = logName(1, DICT_A);
    File f = storeOpenRead(logName(2, DICT_A));
    TEST_ASSERT_TRUE((bool)f);
    TEST_ASSERT_TRUE(ensureSpace());
    TEST_ASSERT_TRUE(have(logName(1, DICT_A)));
    TEST_ASSERT_TRUE(have(logName(2, DICT_A)));
    TEST_ASSERT_FALSE(have(logName(3, DICT_A)));
    TEST_ASSERT_FALSE(have(logName(4, DICT_A)));
    TEST_ASSERT_TRUE(have(logName(5, DICT_A)));
    f.close();
    storeReadDone(logName(2, DICT_A));
}

// The bench's directory: twenty tiny sessions in front of the big ones. An
// inline file frees no block, so the tally must not count it as one, or the
// four rounds go on confirming a shortfall that a tiny delete never covered.
void test_tiny_inline_files_do_not_count_as_space_freed() {
    for (int s = 1; s <= 20; s++) session(s, TINY);
    fillPastTheReserve(21);
    TEST_ASSERT_TRUE(ensureSpace());
    for (int s = 1; s <= 22; s++) TEST_ASSERT_FALSE(have(logName(s, DICT_A)));   // every tiny one, then the two big ones that mattered
    TEST_ASSERT_TRUE(have(logName(23, DICT_A)));
    TEST_ASSERT_TRUE(freeBytes() >= FS_MIN_FREE);
}

void test_stale_dictionaries_go_and_named_ones_stay() {
    dictId = DICT_A;
    LittleFS.put(pathOf(dictFile(DICT_A)), 6000);
    LittleFS.put(pathOf(dictFile(DICT_B)), 6000);
    LittleFS.put(pathOf(dictFile(DICT_C)), 6000);
    LittleFS.put(pathOf("dict-new.tmp"), 6000);   // a rebuild that died before its rename
    session(1, BIG, DICT_B);
    TEST_ASSERT_EQUAL(2 * 8192, dictCollect());   // C and the temporary: two files of two blocks each
    TEST_ASSERT_TRUE(have(dictFile(DICT_A)));      // in use
    TEST_ASSERT_TRUE(have(dictFile(DICT_B)));      // named by a session on the flash
    TEST_ASSERT_FALSE(have(dictFile(DICT_C)));
    TEST_ASSERT_FALSE(have("dict-new.tmp"));
    TEST_ASSERT_TRUE(have(logName(1, DICT_B)));    // never a session file
}

void test_a_dictionary_being_read_is_kept() {
    LittleFS.put(pathOf(dictFile(DICT_C)), 6000);
    File f = storeOpenRead(dictFile(DICT_C));
    TEST_ASSERT_TRUE((bool)f);
    TEST_ASSERT_EQUAL(0, dictCollect());
    TEST_ASSERT_TRUE(have(dictFile(DICT_C)));
    f.close();
    storeReadDone(dictFile(DICT_C));
    TEST_ASSERT_EQUAL(8192, dictCollect());
    TEST_ASSERT_FALSE(have(dictFile(DICT_C)));
}

void test_reclaim_takes_stale_dictionaries_before_sessions() {
    dictId = DICT_A;
    fillPastTheReserve();
    LittleFS.put(pathOf(dictFile(DICT_C)), 40000);   // ten blocks nothing names: the shortfall is now 77824
    TEST_ASSERT_TRUE(ensureSpace());
    TEST_ASSERT_FALSE(have(dictFile(DICT_C)));
    TEST_ASSERT_FALSE(have(logName(1, DICT_A)));   // the dictionary's 40960 left 36864 to find: two sessions
    TEST_ASSERT_FALSE(have(logName(2, DICT_A)));
    TEST_ASSERT_TRUE(have(logName(3, DICT_A)));
}

void test_boot_loads_the_dictionary_nvs_names_and_starts_without_a_damaged_one() {
    std::string text = "Saving Stats, Hibernating for 3600 sec\nMBB RTC treated valid.\n";
    uint32_t id = uzlib_adler32((const uint8_t*)text.data(), text.size(), 1);
    LittleFS.put(pathOf(dictFile(id)), 0);
    { File f = LittleFS.open(pathOf(dictFile(id)), FILE_WRITE); f.write((const uint8_t*)text.data(), text.size()); f.close(); }
    Preferences p; p.begin("dongle", false); p.putUInt("dict", id); p.end();
    resetModule();
    TEST_ASSERT_TRUE(storeBegin("test"));
    TEST_ASSERT_EQUAL_UINT32(id, dictId);
    TEST_ASSERT_EQUAL(text.size(), keeper.dlen);
    TEST_ASSERT_EQUAL(2, keeper.nlines);
    LittleFS.put(pathOf(dictFile(id)), 10);   // the file on flash no longer matches its id
    resetModule();
    TEST_ASSERT_TRUE(storeBegin("test"));
    TEST_ASSERT_EQUAL_UINT32(0, dictId);
    TEST_ASSERT_EQUAL(0, keeper.dlen);
}

// A session end to end: the first MBB line names it, the quiet commit creates
// the file, the name carries the dictionary in use, and the close leaves a
// file that is not empty.
void test_a_session_file_is_named_for_the_dictionary_in_use() {
    dictId = 0x1234abcd;
    storeAppend("2026-09-10T12:00:00.000 09/10/2026 12:00:00.000 - State change from STRT to PWSU");
    TEST_ASSERT_EQUAL(0, listing().size());   // nothing on the flash while the MBB may still be talking
    storeTick(true);
    std::vector<std::string> files = listing();
    TEST_ASSERT_EQUAL(1, files.size());
    TEST_ASSERT_EQUAL_UINT32(0x1234abcd, logDictId(files[0].c_str(), files[0].size()));
    TEST_ASSERT_EQUAL_STRING(files[0].c_str(), storeActiveName().c_str());
    storeSessionClose();
    TEST_ASSERT_EQUAL_STRING("", storeActiveName().c_str());
    TEST_ASSERT_TRUE(fileSizeOf(files[0].c_str()) > 8);   // header, a line, a flush and the trailer
    TEST_ASSERT_EQUAL(0, storeDroppedLines());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_the_flash_is_counted_in_blocks);
    RUN_TEST(test_reclaim_deletes_the_oldest_until_the_reserve_is_back);
    RUN_TEST(test_reclaim_spares_the_active_file_and_one_being_read);
    RUN_TEST(test_tiny_inline_files_do_not_count_as_space_freed);
    RUN_TEST(test_stale_dictionaries_go_and_named_ones_stay);
    RUN_TEST(test_a_dictionary_being_read_is_kept);
    RUN_TEST(test_reclaim_takes_stale_dictionaries_before_sessions);
    RUN_TEST(test_boot_loads_the_dictionary_nvs_names_and_starts_without_a_damaged_one);
    RUN_TEST(test_a_session_file_is_named_for_the_dictionary_in_use);
    return UNITY_END();
}
