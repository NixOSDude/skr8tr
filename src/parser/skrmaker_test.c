/*
 * skrmaker_test.c — SkrtrMaker parser smoke test
 * SSoA LEVEL 3 — Manifest Shard (test / development tool)
 *
 * Usage: skrmaker_test <manifest.skr8tr> [manifest2.skr8tr ...]
 * Parses each file and dumps the result. Exit 0 on success.
 */

#include "skrmaker.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: skrmaker_test <file.skr8tr> ...\n");
        return 1;
    }

    int all_ok = 1;
    for (int i = 1; i < argc; i++) {
        char err[512] = {0};
        LambProc* procs = skrmaker_parse(argv[i], err, sizeof(err));
        if (!procs) {
            fprintf(stderr, "FAIL  %s\n  %s\n", argv[i], err);
            all_ok = 0;
            continue;
        }

        printf("OK    %s\n", argv[i]);
        printf("----------------------------------------------\n");
        skrmaker_dump(procs);
        printf("----------------------------------------------\n\n");
        skrmaker_free(procs);
    }

    return all_ok ? 0 : 1;
}
