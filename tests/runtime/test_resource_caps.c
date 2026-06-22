/* tests/runtime/test_resource_caps.c — issue #343 unit tests.
 *
 * Verifies the cap counter tracks current usage (not high-water-
 * mark), refuses past-cap allocations, saturates on under-account
 * free, and flips the deadline tripwire on schedule.
 */

#include "test_harness.h"
#include "../../runtime/aether_resource_caps.h"
#include "../../std/bytes/aether_bytes.h"
#include "../../std/os/aether_os.h"
#include "../../std/fs/aether_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Reset to a known baseline between tests by lifting the cap and
 * draining whatever the prior test accumulated. The counter doesn't
 * expose a "set_used" API on purpose (tests manage their own
 * accounting), so we read + drain. */
static void caps_reset(void) {
    aether_caps_set_memory_cap(0);
    aether_caps_set_deadline_ms(0);
    uint64_t leftover = aether_caps_used_bytes();
    if (leftover > 0) aether_caps_account_free((size_t)leftover);
}

TEST_CATEGORY(caps_cap_disabled_by_default, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* No cap set → every check passes. */
    ASSERT_EQ(1, aether_caps_check_alloc(1));
    ASSERT_EQ(1, aether_caps_check_alloc(1024 * 1024));
    aether_caps_account_free(1);
    aether_caps_account_free(1024 * 1024);
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
}

TEST_CATEGORY(caps_refuses_oversize, TEST_CATEGORY_STDLIB) {
    caps_reset();
    aether_caps_set_memory_cap(1024 * 1024);  /* 1 MiB */
    /* 2 MiB single alloc → refused. */
    ASSERT_EQ(0, aether_caps_check_alloc(2 * 1024 * 1024));
    /* Counter unchanged on refuse. */
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
    aether_caps_set_memory_cap(0);
}

TEST_CATEGORY(caps_current_usage_not_highwater, TEST_CATEGORY_STDLIB) {
    /* The whole point of #343: long-running guests that allocate +
     * free in a loop must not eventually trip the cap on cumulative
     * churn. With current-usage tracking, alloc-then-free cancels
     * exactly. */
    caps_reset();
    aether_caps_set_memory_cap(1024 * 1024);  /* 1 MiB */
    /* 100 × 8 KiB allocs interleaved with frees — total churn ~800
     * KiB but in-flight high-water ~16 KiB. Must succeed. */
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ(1, aether_caps_check_alloc(8 * 1024));
        if (i & 1) aether_caps_account_free(8 * 1024);
    }
    /* 50 still outstanding × 8 KiB = 400 KiB; within the cap. */
    ASSERT_TRUE(aether_caps_used_bytes() == 50 * 8 * 1024);
    /* Drain. */
    for (int i = 0; i < 50; i++) aether_caps_account_free(8 * 1024);
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
    aether_caps_set_memory_cap(0);
}

TEST_CATEGORY(caps_saturating_decrement, TEST_CATEGORY_STDLIB) {
    /* Free more than we allocated → counter saturates at 0 in
     * release. (Debug builds would assert; this test only runs the
     * release-shape path via NDEBUG=defined builds.) */
    caps_reset();
#ifdef NDEBUG
    aether_caps_account_free(100);
    ASSERT_EQ((uint64_t)0, aether_caps_used_bytes());
#endif
}

TEST_CATEGORY(caps_deadline_disabled_by_default, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* No deadline → tripwire returns 0. */
    ASSERT_EQ(0, aether_caps_deadline_tripped());
}

TEST_CATEGORY(caps_deadline_trips_on_schedule, TEST_CATEGORY_STDLIB) {
    caps_reset();
    aether_caps_set_deadline_ms(20);  /* 20 ms window */
    /* Immediately after arming, tripwire should not have flipped. */
    ASSERT_EQ(0, aether_caps_deadline_tripped());
    /* Sleep 50 ms — well past the deadline. */
    struct timespec ts = { 0, 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    /* Now tripped. */
    ASSERT_EQ(1, aether_caps_deadline_tripped());
    /* Sticky: still tripped on a follow-up read. */
    ASSERT_EQ(1, aether_caps_deadline_tripped());
    /* Reset clears the flag. */
    aether_caps_set_deadline_ms(0);
    ASSERT_EQ(0, aether_caps_deadline_tripped());
}

TEST_CATEGORY(caps_explicit_abort_sticky, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* No deadline set, but explicit abort flips the sticky flag. */
    __aether_abort_call();
    ASSERT_EQ(1, aether_caps_deadline_tripped());
    /* Cleared by re-arming the deadline. */
    aether_caps_set_deadline_ms(0);
    ASSERT_EQ(0, aether_caps_deadline_tripped());
}

/* #462 caps-audit accounting balance: std.bytes routes its growable
 * buffer + struct through aether_caps_malloc/realloc/free. The defining
 * correctness property of a caps conversion (distinct from a leak: a
 * size-mismatched free corrupts the *accounting*, not the heap) is that
 * the counter returns to baseline after create + grow + free. Repeatedly
 * build a bytes object, force several reallocations by writing at a high
 * index, and free it; used_bytes must land back where it started — no
 * drift from an over- or under-credited free. */
TEST_CATEGORY(caps_bytes_accounting_balances, TEST_CATEGORY_STDLIB) {
    caps_reset();
    uint64_t base = aether_caps_used_bytes();
    for (int rep = 0; rep < 4; rep++) {
        AetherBytes* b = aether_bytes_new(0);
        ASSERT_TRUE(b != NULL);
        for (int i = 0; i < 4096; i++) {
            ASSERT_EQ(1, aether_bytes_set(b, i, i & 0xff));  /* forces grows */
        }
        /* Mid-life the counter must have grown above baseline. */
        ASSERT_TRUE(aether_caps_used_bytes() > base);
        aether_bytes_free(b);
        /* ...and return exactly to baseline after the matched frees. */
        ASSERT_EQ(base, aether_caps_used_bytes());
    }
}

/* #462 caps-audit, std.os: os_getenv now allocates its returned value
 * through aether_caps_malloc, so a sandbox memory cap below the value
 * length refuses the read (a plugin can't exfiltrate an arbitrarily
 * large environment value past the limit). This exercises the exact
 * POSIX path converted in this change: uncapped it returns the value;
 * capped below the value size it returns NULL with the counter
 * unchanged. (os_getenv's POSIX body is the converted site; setenv is
 * POSIX, so the test is POSIX-guarded.) */
#ifndef _WIN32
TEST_CATEGORY(caps_os_getenv_denied_past_cap, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* 64-char value, comfortably larger than the tiny cap below. */
    setenv("AETHER_CAP_TEST_VAR",
           "0123456789012345678901234567890123456789012345678901234567890123", 1);

    /* Uncapped: the value is returned (and is cap-allocated). The
     * caller-owned contract is libc free / fail-safe upward drift,
     * exactly as in production. */
    char* ok = os_getenv("AETHER_CAP_TEST_VAR");
    ASSERT_TRUE(ok != NULL);
    free(ok);

    /* Cap below the value length: the os_getenv allocation is refused. */
    caps_reset();
    uint64_t base = aether_caps_used_bytes();
    aether_caps_set_memory_cap(8);
    char* denied = os_getenv("AETHER_CAP_TEST_VAR");
    ASSERT_TRUE(denied == NULL);
    /* Refusal must not perturb the counter. */
    ASSERT_EQ(base, aether_caps_used_bytes());

    aether_caps_set_memory_cap(0);
    unsetenv("AETHER_CAP_TEST_VAR");
}
#endif
/* #462 caps-audit, std.fs directory listing: dir_list_raw routes the
 * DirList struct, the doubling entries array, and every strdup'd entry
 * name through aether_caps_malloc/realloc; dir_list_free releases them
 * via the new `capacity` field (array) and strlen-at-free (names). The
 * correctness property — distinct from a leak, since a size-mismatched
 * free corrupts the accounting not the heap — is that the counter
 * returns exactly to baseline after list + free. List the current
 * directory (which always has entries), confirm the counter grew, then
 * free and confirm it lands back on baseline with no drift. */
TEST_CATEGORY(caps_fs_dir_list_accounting_balances, TEST_CATEGORY_STDLIB) {
    caps_reset();
    uint64_t base = aether_caps_used_bytes();
    for (int rep = 0; rep < 4; rep++) {
        DirList* dl = dir_list_raw(".");
        ASSERT_TRUE(dl != NULL);
        /* The source tree's CWD always has entries → counter grew. */
        ASSERT_TRUE(dir_list_count(dl) > 0);
        ASSERT_TRUE(aether_caps_used_bytes() > base);
        dir_list_free(dl);
        /* Struct + array + every entry name freed with exact sizes. */
        ASSERT_EQ(base, aether_caps_used_bytes());
    }
}

/* #462 caps-audit, std.fs file handle: file_open_raw now routes the File
 * struct AND its retained path copy (a sandboxed caller can craft an
 * enormous filename) through aether_caps_malloc; file_close releases both
 * with the matching sizes. The accounting must return exactly to baseline
 * after open + close — a size-mismatched free would drift the counter. */
TEST_CATEGORY(caps_fs_file_open_close_balances, TEST_CATEGORY_STDLIB) {
    caps_reset();
    /* CWD-relative so it works on Windows too (no /tmp). The test runner
     * runs from a writable directory — the dir-list test reads "." here. */
    const char* tp = "aether_caps_fopen_test.tmp";
    FILE* w = fopen(tp, "wb");
    ASSERT_TRUE(w != NULL);
    fputs("hello", w);
    fclose(w);

    uint64_t base = aether_caps_used_bytes();
    for (int rep = 0; rep < 4; rep++) {
        File* f = file_open_raw(tp, "rb");
        ASSERT_TRUE(f != NULL);
        /* The File struct + the path copy sit above baseline. */
        ASSERT_TRUE(aether_caps_used_bytes() > base);
        file_close(f);
        /* ...and both are released with their exact sizes. */
        ASSERT_EQ(base, aether_caps_used_bytes());
    }
    remove(tp);
}

/* #462 caps-audit acceptance: reading a file whose size exceeds the
 * sandbox's remaining memory headroom must be refused, not OOM the host.
 * file_read_all_raw's buffer is cap-allocated (#343); with a cap set
 * below the file size the alloc is denied (returns NULL) and the counter
 * does not drift from the refused allocation. The cap surface is what a
 * plugin host installs to bound filesystem-driven memory. */
#ifndef _WIN32
TEST_CATEGORY(caps_fs_read_denied_past_cap, TEST_CATEGORY_STDLIB) {
    caps_reset();
    const char* tp = "aether_caps_read_test.tmp";  /* CWD-relative; see above */
    FILE* w = fopen(tp, "wb");
    ASSERT_TRUE(w != NULL);
    char chunk[4096];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 32; i++) fwrite(chunk, 1, sizeof(chunk), w);  /* 128 KiB */
    fclose(w);

    File* f = file_open_raw(tp, "rb");
    ASSERT_TRUE(f != NULL);

    /* Cap at current usage + 64 KiB — below the 128 KiB read buffer. */
    uint64_t headroom = aether_caps_used_bytes();
    aether_caps_set_memory_cap(headroom + 64 * 1024);
    char* data = file_read_all_raw(f);
    ASSERT_TRUE(data == NULL);                     /* read refused */
    ASSERT_EQ(headroom, aether_caps_used_bytes()); /* counter not drifted */

    aether_caps_set_memory_cap(0);
    file_close(f);
    remove(tp);
}
#endif
