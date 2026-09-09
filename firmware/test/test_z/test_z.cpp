// The zlib stream with a preset dictionary, checked against the host's zlib.
#include <unity.h>
#include <zlib.h>
#include <string>
#include <vector>
#include "zstream.h"

void setUp() {}
void tearDown() {}

static ZStream<6144, 8192, 1100, 4096, 10> zs;   // the dongle's sizes, hash included
static std::vector<uint8_t> file;

static void commit() { zs.flush(); file.insert(file.end(), zs.out, zs.out + zs.pending()); zs.taken(); }

// Inflate a zlib stream with an optional dictionary; returns the bytes and whether the trailer was reached.
static std::string inflateAll(const std::vector<uint8_t>& in, const std::string& dict, bool& complete) {
    z_stream z = {};
    inflateInit2(&z, 15);
    std::string out;
    uint8_t buf[4096];
    z.next_in = (Bytef*)in.data();
    z.avail_in = in.size();
    int r;
    do {
        z.next_out = buf; z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        if (r == Z_NEED_DICT) {
            TEST_ASSERT_EQUAL_UINT32(adler32(1, (const Bytef*)dict.data(), dict.size()), z.adler);   // the header names our dictionary
            inflateSetDictionary(&z, (const Bytef*)dict.data(), dict.size());
            r = Z_OK;
            continue;
        }
        out.append((char*)buf, sizeof buf - z.avail_out);
    } while (r == Z_OK && z.avail_in);
    complete = r == Z_STREAM_END;
    inflateEnd(&z);
    return out;
}

static const std::string BANNER =
    "******************************************************************\n"
    "*              Zero Motorcycles MBB                         *\n"
    "*             Board Name : MBB PDU POTTED GEN 3 STEP D      *\n"
    "Reset Source: Hib Wake RTC, Power-On, Supply WD, Power Val\n"
    "ZERO MBB> waiting for startup...\n - Checking Measure\n - Checking Eeprom\n\t- Okay\n - Checking Flash\n\t- Okay\n"
    "DEBUG: ../src/Application/zero_mbb_test.c : line 1413 - RTC test\nVerify RTC\nMBB RTC treated valid.\n"
    "Startup complete, status: SUCCESS\nSaving Stats, Hibernating for 3600 sec\n";

void test_round_trip_with_a_dictionary_and_the_header_names_it() {
    file.clear();
    zs.begin((const uint8_t*)BANNER.data(), BANNER.size());
    TEST_ASSERT_EQUAL(0x78, zs.out[0]); TEST_ASSERT_EQUAL(0x20, zs.out[1]);
    TEST_ASSERT_EQUAL(0, (0x78 * 256 + 0x20) % 31);
    std::string expect;
    for (int i = 0; i < 4; i++) {   // a session that is the banner with stamps, four times over
        for (size_t p = 0, q; p < BANNER.size(); p = q + 1) {
            q = BANNER.find('\n', p);
            std::string line = "2026-09-08T04:08:2" + std::to_string(i) + ".1" + std::to_string(p % 100) + " " + BANNER.substr(p, q - p);
            TEST_ASSERT_TRUE(zs.add(line.data(), line.size()));
            expect += line + "\n";
        }
        commit();
    }
    zs.finish();
    file.insert(file.end(), zs.out, zs.out + zs.pending());
    bool complete = false;
    std::string got = inflateAll(file, BANNER, complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(expect == got);
    TEST_ASSERT_TRUE(file.size() * 2 < expect.size());   // the stamps are literals; the text is matches
    TEST_ASSERT_FALSE(zs.overflowed());
    // The dictionary's worth: the banner alone, first copy, with and without it.
    zs.begin((const uint8_t*)BANNER.data(), BANNER.size());
    size_t withDict = 0;
    for (size_t p = 0, q; p < BANNER.size(); p = q + 1) { q = BANNER.find('\n', p); zs.add(BANNER.data() + p, q - p); }
    zs.flush(); withDict = zs.pending() - 6;
    zs.begin(nullptr, 0);
    for (size_t p = 0, q; p < BANNER.size(); p = q + 1) { q = BANNER.find('\n', p); zs.add(BANNER.data() + p, q - p); }
    zs.flush();
    size_t without = zs.pending() - 2;
    TEST_ASSERT_TRUE(withDict * 2 < without);   // measured 74 against 220 bytes
}

void test_no_dictionary_is_plain_zlib() {
    file.clear();
    zs.begin(nullptr, 0);
    TEST_ASSERT_EQUAL(0x78, zs.out[0]); TEST_ASSERT_EQUAL(0x01, zs.out[1]);
    zs.add("first", 5); zs.add("second", 6);
    commit();
    zs.add("never committed", 15);
    bool complete = true;
    std::string got = inflateAll(file, "", complete);
    TEST_ASSERT_FALSE(complete);   // cut before the trailer: readable to the last commit
    TEST_ASSERT_EQUAL_STRING("first\nsecond\n", got.c_str());
    zs.finish();
    file.insert(file.end(), zs.out, zs.out + zs.pending());
    got = inflateAll(file, "", complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_STRING("first\nsecond\nnever committed\n", got.c_str());
}

void test_dictionary_survives_the_window_sliding() {
    file.clear();
    zs.begin((const uint8_t*)BANNER.data(), BANNER.size());
    std::string expect;
    std::string filler(900, 'x');
    for (int i = 0; i < 20; i++) {   // 18 KB of filler through an 8 KB history, then the banner again
        filler[0] = 'a' + i;
        zs.add(filler.data(), filler.size()); expect += filler + "\n";
        commit();
    }
    size_t before = file.size();
    for (size_t p = 0, q; p < BANNER.size(); p = q + 1) {
        q = BANNER.find('\n', p);
        zs.add(BANNER.data() + p, q - p); expect += BANNER.substr(p, q - p) + "\n";
    }
    commit();
    TEST_ASSERT_TRUE(file.size() - before < BANNER.size() / 3);   // still matched against the dictionary after the slide (74 of 700 bytes measured)
    zs.finish();
    file.insert(file.end(), zs.out, zs.out + zs.pending());
    bool complete = false;
    TEST_ASSERT_TRUE(expect == inflateAll(file, BANNER, complete));
    TEST_ASSERT_TRUE(complete);
}

void test_add_refuses_rather_than_overflowing() {
    zs.begin(nullptr, 0);
    uint32_t x = 12345;
    int added = 0;
    for (;;) {
        std::string noise;
        for (int i = 0; i < 1000; i++) { x = x * 1103515245u + 12345u; noise += (char)((x >> 16) % 255 + 1); }
        if (!zs.add(noise.data(), noise.size())) break;
        added++;
    }
    TEST_ASSERT_TRUE(added >= 3 && added < 5);
    zs.flush();
    TEST_ASSERT_TRUE(zs.pending() <= 4096);
    TEST_ASSERT_FALSE(zs.overflowed());
}

void test_dictionary_distance_is_true_after_many_slides() {
    file.clear();
    zs.begin((const uint8_t*)BANNER.data(), BANNER.size());
    std::string expect;
    std::string filler(1000, 'q');
    for (int i = 0; i < 40; i++) {   // 40 KB of filler: the dictionary passes out of the decoder's 32 KB reach part-way
        filler[0] = 'a' + i % 26; filler[1] = 'A' + i % 26;
        zs.add(filler.data(), filler.size()); expect += filler + "\n";
        if (i % 7 == 6) {   // the banner again, at several distances from the dictionary
            for (size_t p = 0, q; p < BANNER.size(); p = q + 1) {
                q = BANNER.find('\n', p);
                zs.add(BANNER.data() + p, q - p); expect += BANNER.substr(p, q - p) + "\n";
            }
        }
        commit();
    }
    zs.finish();
    file.insert(file.end(), zs.out, zs.out + zs.pending());
    bool complete = false;
    std::string got = inflateAll(file, BANNER, complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL(expect.size(), got.size());
    TEST_ASSERT_TRUE(expect == got);
}

void test_a_match_never_runs_from_the_dictionary_into_the_data() {
    // A dictionary that does not end in a newline, and lines that equal its
    // tail followed by what happens to sit first in the history after a slide.
    std::string dict = "abcdefghij";   // no trailing newline
    file.clear();
    zs.begin((const uint8_t*)dict.data(), dict.size());
    std::string expect;
    for (int i = 0; i < 20; i++) {   // through a slide: 20 x 909 bytes against 8 KB of history
        std::string l = "ghijKLMN" + std::string(900, 'z') + std::to_string(i);
        zs.add(l.data(), l.size()); expect += l + "\n";
        commit();
    }
    TEST_ASSERT_TRUE(zs.c.dict_extra > 0);   // the slide happened
    std::string tricky = "abcdefghijzzzzzzzz";   // dictionary tail then what the history now begins with
    zs.add(tricky.data(), tricky.size()); expect += tricky + "\n";
    commit();
    zs.finish();
    file.insert(file.end(), zs.out, zs.out + zs.pending());
    bool complete = false;
    std::string got = inflateAll(file, dict, complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(expect == got);
}

// The poller commits between every command of a batch, thirteen times a
// minute rather than twice, so the stream has to survive a flush cadence
// far tighter than a session's. And a trailer written straight after a
// flush, with nothing drained between them, must fit the reserve.
void test_many_flushes_and_a_trailer_that_follows_one() {
    for (int cadence : {1, 3, 13}) {
        zs = {};
        file.clear();
        std::string dict = BANNER;
        zs.begin((const uint8_t*)dict.data(), dict.size());
        std::string want;
        char line[120];
        for (int i = 0; i < 2000; i++) {
            int n = snprintf(line, sizeof line, "2026-09-09T00:%02d:%02d.%03d  Disch limits: curr %d cap %d act 2147483647 pow %d",
                             i / 60 % 60, i % 60, i % 1000, 1251 + i % 7, 1251 + i % 5, 138861 + i);
            TEST_ASSERT_TRUE(zs.add(line, n));
            want.append(line, n);
            want += '\n';   // the stream ends every line itself
            if (i % cadence == cadence - 1) commit();
        }
        zs.finish();   // straight after the last flush on the cadence-1 pass, nothing drained
        file.insert(file.end(), zs.out, zs.out + zs.pending());
        zs.taken();
        TEST_ASSERT_FALSE(zs.overflowed());
        bool complete = false;
        std::string got = inflateAll(file, dict, complete);
        TEST_ASSERT_TRUE(complete);
        TEST_ASSERT_EQUAL_size_t(want.size(), got.size());
        TEST_ASSERT_TRUE(want == got);
    }
}


int main() {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_with_a_dictionary_and_the_header_names_it);
    RUN_TEST(test_no_dictionary_is_plain_zlib);
    RUN_TEST(test_dictionary_survives_the_window_sliding);
    RUN_TEST(test_add_refuses_rather_than_overflowing);
    RUN_TEST(test_dictionary_distance_is_true_after_many_slides);
    RUN_TEST(test_a_match_never_runs_from_the_dictionary_into_the_data);
    RUN_TEST(test_many_flushes_and_a_trailer_that_follows_one);
    return UNITY_END();
}
