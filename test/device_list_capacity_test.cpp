#include "enums.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <initializer_list>
extern "C" {
void soloudTestSetPlaybackDeviceCount(int);
void listPlaybackDevices(char **, int **, int **, int *, int);
void freeListPlaybackDevices(char **, int **, int **, int);
}
int main() {
    soloudTestSetPlaybackDeviceCount(60);
    int failures = 0;
    for (int capacity : {50, 60}) {
        auto **names = static_cast<char **>(calloc(capacity, sizeof(char *)));
        auto **ids = static_cast<int **>(calloc(capacity, sizeof(int *)));
        auto **defaults = static_cast<int **>(calloc(capacity, sizeof(int *)));
        int count = 0;
        listPlaybackDevices(names, ids, defaults, &count, capacity);
        if (count != capacity) ++failures;
        for (int i = 0; i < count; ++i) {
            const auto expected = "Test Output " + std::to_string(i);
            if (strcmp(names[i], expected.c_str()) != 0 || *ids[i] != i ||
                *defaults[i] != (i == 0)) ++failures;
        }
        freeListPlaybackDevices(names, ids, defaults, count);
        free(names); free(ids); free(defaults);
    }
    soloudTestSetPlaybackDeviceCount(-1);
    printf("Device capacity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
