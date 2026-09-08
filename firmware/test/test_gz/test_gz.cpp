// The gzip stream on fixed arrays, checked against the host's zlib.
#include <unity.h>
#include <zlib.h>
#include <string>
#include <vector>
#include "gzstream.h"

void setUp() {}
void tearDown() {}

static GzStream<4096, 1100, 4096, 10> gz;   // the dongle's sizes
static std::vector<uint8_t> file;

static void commit() { gz.flush(); file.insert(file.end(), gz.out, gz.out + gz.pending()); gz.taken(); }

// Inflate a gzip stream; returns the bytes and whether the trailer was reached.
static std::string inflateAll(const std::vector<uint8_t>& in, bool& complete) {
    z_stream z = {};
    inflateInit2(&z, 31);
    std::string out;
    uint8_t buf[4096];
    z.next_in = (Bytef*)in.data();
    z.avail_in = in.size();
    int r;
    do {
        z.next_out = buf; z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.append((char*)buf, sizeof buf - z.avail_out);
    } while (r == Z_OK && z.avail_in);
    complete = r == Z_STREAM_END;
    inflateEnd(&z);
    return out;
}

void test_lines_round_trip_through_commits_and_the_trailer() {
    file.clear();
    gz.begin();
    std::string expect;
    for (int i = 0; i < 300; i++) {
        char line[120];
        int n = snprintf(line, sizeof line, "2026-09-07T21:%02d:%02d.%03d Disch limits: curr 819 cap 819 act 2147483647 pow %d", i / 60, i % 60, i * 7 % 1000, 83500 + i % 3);
        TEST_ASSERT_TRUE(gz.add(line, n));
        expect += std::string(line, n) + "\n";
        if (i % 40 == 39) commit();
    }
    commit();
    gz.finish();
    file.insert(file.end(), gz.out, gz.out + gz.pending());
    bool complete = false;
    std::string got = inflateAll(file, complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL(expect.size(), got.size());
    TEST_ASSERT_TRUE(expect == got);
    TEST_ASSERT_TRUE(file.size() * 4 < expect.size());   // repetitive lines compress well across commits
    TEST_ASSERT_FALSE(gz.overflowed());
}

void test_a_stream_cut_before_the_trailer_decodes_to_the_last_commit() {
    file.clear();
    gz.begin();
    gz.add("first", 5); gz.add("second", 6);
    commit();
    gz.add("never committed", 15);   // in the buffer when the power went
    bool complete = true;
    std::string got = inflateAll(file, complete);
    TEST_ASSERT_FALSE(complete);
    TEST_ASSERT_EQUAL_STRING("first\nsecond\n", got.c_str());
}

void test_the_window_slides_and_history_still_matches() {
    file.clear();
    gz.begin();
    std::string expect;
    std::string line(900, 'x');
    for (int i = 0; i < 40; i++) {   // 36 KB through a 5 KB window
        line[0] = 'a' + i % 26;
        TEST_ASSERT_TRUE(gz.add(line.data(), line.size()));
        expect += line + "\n";
        commit();
    }
    gz.finish();
    file.insert(file.end(), gz.out, gz.out + gz.pending());
    bool complete = false;
    std::string got = inflateAll(file, complete);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(expect == got);
    TEST_ASSERT_TRUE(file.size() < expect.size() / 20);   // 900 x's match the previous line's x's
}

void test_add_refuses_rather_than_overflowing() {
    gz.begin();
    uint32_t x = 12345;
    int added = 0;
    for (;;) {   // a different incompressible line each time, so nothing matches
        std::string noise;
        for (int i = 0; i < 1000; i++) { x = x * 1103515245u + 12345u; noise += (char)((x >> 16) % 255 + 1); }
        if (!gz.add(noise.data(), noise.size())) break;
        added++;
    }
    TEST_ASSERT_TRUE(added >= 3 && added < 5);   // a 4 KB buffer holds three or four such lines plus the flush
    TEST_ASSERT_FALSE(gz.overflowed());
    gz.flush();
    TEST_ASSERT_TRUE(gz.pending() <= 4096);
    TEST_ASSERT_FALSE(gz.overflowed());
    TEST_ASSERT_FALSE(gz.add(std::string(1200, 'y').data(), 1200));   // longer than a line can be
}

void test_empty_session_is_a_valid_gzip() {
    file.clear();
    gz.begin();
    gz.finish();
    file.insert(file.end(), gz.out, gz.out + gz.pending());
    bool complete = false;
    TEST_ASSERT_EQUAL_STRING("", inflateAll(file, complete).c_str());
    TEST_ASSERT_TRUE(complete);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_lines_round_trip_through_commits_and_the_trailer);
    RUN_TEST(test_a_stream_cut_before_the_trailer_decodes_to_the_last_commit);
    RUN_TEST(test_the_window_slides_and_history_still_matches);
    RUN_TEST(test_add_refuses_rather_than_overflowing);
    RUN_TEST(test_empty_session_is_a_valid_gzip);
    return UNITY_END();
}
