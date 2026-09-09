// Keep the N smallest of a stream of names, sorted, without holding them all.
// The shortcut belongs to a full set: on the name that fills the last slot
// there is nothing yet in it to compare against.
#pragma once
#include <cstddef>
#include <cstring>

template <size_t N, size_t LEN>
struct KeepSmallest {
    char item[N][LEN];
    size_t n = 0;

    void offer(const char* name) {
        if (n == N && strcmp(name, item[N - 1]) >= 0) return;
        size_t i = n < N ? n++ : N - 1;
        while (i > 0 && strcmp(name, item[i - 1]) < 0) {
            memcpy(item[i], item[i - 1], LEN);
            i--;
        }
        strncpy(item[i], name, LEN - 1);
        item[i][LEN - 1] = 0;
    }
};
