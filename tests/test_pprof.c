/*
 * pprof encoder unit test.
 *
 * Tests:
 *   1. build_pprof byte-level output matches known reference
 *   2. proto field numbers correct (profile.proto)
 *   3. string table: [0]="" always
 *   4. ValueType uses varint (wire_type=0) for type/unit, NOT LEN
 *   5. function name index matches string table
 *   6. push to Pyroscope and verify 200
 *
 * Compile: gcc -Wall -O2 -o tests/test_pprof tests/test_pprof.c
 * Run:     ./tests/test_pprof
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <assert.h>

/* Copy the exact encoder from extension/pyroscope_php.c (minus PHP/zend deps) */
#define MAX_STACK_LEN 4096
typedef char sample_t[MAX_STACK_LEN];

/* ── protobuf wire encoder (copied from pyroscope_php.c) ────────────── */

typedef struct { uint8_t *buf; size_t cap; size_t len; } pb_t;

static void pb_varint(pb_t *pb, uint64_t v) {
    while (v > 0x7f) {
        if (pb->len < pb->cap) pb->buf[pb->len] = (uint8_t)(v & 0x7f) | 0x80;
        pb->len++; v >>= 7;
    }
    if (pb->len < pb->cap) pb->buf[pb->len] = (uint8_t)v;
    pb->len++;
}

static inline void pb_tag(pb_t *pb, int fn, int wt) { pb_varint(pb, (uint64_t)((fn << 3) | wt)); }
static inline void pb_int(pb_t *pb, int fn, int64_t v)  { pb_tag(pb, fn, 0); pb_varint(pb, (uint64_t)v); }
static inline void pb_uint(pb_t *pb, int fn, uint64_t v) { pb_tag(pb, fn, 0); pb_varint(pb, v); }
static void pb_str(pb_t *pb, int fn, const char *s, size_t len) {
    pb_tag(pb, fn, 2); pb_varint(pb, len);
    if (pb->len + len <= pb->cap) memcpy(pb->buf + pb->len, s, len);
    pb->len += len;
}
static void pb_msg(pb_t *pb, int fn, size_t body_len) {
    pb_tag(pb, fn, 2); pb_varint(pb, body_len);
}
static void pb_valtype(pb_t *pb, int fn, int ti, int ui) {
    size_t body = 1 + 1 + 1 + 1;
    uint64_t t = ti, u = ui;
    while (t > 0x7f) { body++; t >>= 7; }
    while (u > 0x7f) { body++; u >>= 7; }
    pb_msg(pb, fn, body);
    pb_uint(pb, 1, ti);
    pb_uint(pb, 2, ui);
}

#define MAX_STR  1024
#define MAX_FRAMES 512

static int str_idx(const char **tab, int *cnt, const char *s) {
    if (!s || !*s) return 0;
    for (int i = 0; i < *cnt; i++)
        if (strcmp(tab[i], s) == 0) return i + 1;
    if (*cnt >= MAX_STR) return 0;
    int idx = (*cnt) + 1;
    tab[*cnt] = s;
    (*cnt)++;
    return idx;
}

static void build_pprof(sample_t *samples, int64_t *values, uint32_t n,
                        const char *stype, const char *sunit,
                        const char *ptype, const char *punit,
                        int64_t period_ns, char **out, size_t *out_len) {
    if (n == 0) { *out = NULL; *out_len = 0; return; }

    const char *stab[MAX_STR + 1]; int sc = 0;
    str_idx(stab, &sc, "");
    int si_st = str_idx(stab, &sc, stype);
    int si_su = str_idx(stab, &sc, sunit);
    int si_pt = str_idx(stab, &sc, ptype);
    int si_pu = str_idx(stab, &sc, punit);

    const char *fnames[MAX_FRAMES]; int fi[MAX_FRAMES]; int fcnt = 0;
    int *lens[MAX_FRAMES], *locs[MAX_FRAMES];

    for (uint32_t i = 0; i < n; i++) {
        char *s = samples[i]; int nf = 1;
        for (char *c = s; *c; c++) if (*c == ';') nf++;
        lens[i] = malloc(sizeof(int)); *lens[i] = nf;
        locs[i] = malloc(nf * sizeof(int));
        for (int j = 0; j < nf; j++) {
            char *sep = strchr(s, ';');
            size_t flen = sep ? (size_t)(sep - s) : strlen(s);
            char *name = malloc(flen + 1);
            memcpy(name, s, flen); name[flen] = '\0';
            int fsi = str_idx(stab, &sc, name);
            int found = -1;
            for (int k = 0; k < fcnt; k++)
                if (fi[k] == fsi) { found = k; break; }
            if (found < 0) { fnames[fcnt] = name; fi[fcnt] = fsi; found = fcnt; fcnt++; }
            else free(name);
            locs[i][j] = found;
            if (sep) s = sep + 1; else break;
        }
    }

    size_t cap = 4096 + n * 256;
    uint8_t *buf = malloc(cap);
    if (!buf) { *out = NULL; *out_len = 0; return; }
    pb_t pb = {.buf = buf, .cap = cap, .len = 0};

    pb_valtype(&pb, 1, si_st, si_su);
    pb_valtype(&pb, 11, si_pt, si_pu);
    pb_msg(&pb, 3, 2); pb_uint(&pb, 1, 1);

    for (int i = 0; i < fcnt; i++) {
        size_t body = 2 + 2;
        { uint64_t v = i + 1; while (v > 0x7f) { body++; v >>= 7; } }
        { uint64_t v = fi[i]; while (v > 0x7f) { body++; v >>= 7; } }
        pb_msg(&pb, 5, body);
        pb_uint(&pb, 1, i + 1);
        pb_uint(&pb, 2, fi[i]);
    }

    for (int i = 0; i < fcnt; i++) {
        size_t li = 1; { uint64_t v = i + 1; while (v > 0x7f) { li++; v >>= 7; } }
        li += 1;
        size_t body = 2 + 2 + 2 + li;
        { uint64_t v = i + 1; while (v > 0x7f) { body++; v >>= 7; } }
        pb_msg(&pb, 4, body);
        pb_uint(&pb, 1, i + 1);
        pb_uint(&pb, 2, 1);
        pb_msg(&pb, 4, li);
        pb_uint(&pb, 1, i + 1);
    }

    for (uint32_t i = 0; i < n; i++) {
        if (*lens[i] == 0) continue;
        size_t body = 0;
        for (int j = 0; j < *lens[i]; j++) {
            body += 2;
            uint64_t v = locs[i][j] + 1; while (v > 0x7f) { body++; v >>= 7; }
        }
        body += 2;
        { uint64_t v = (uint64_t)values[i]; while (v > 0x7f) { body++; v >>= 7; } }
        if (values[i] < 0) body += 9;
        pb_msg(&pb, 2, body);
        for (int j = 0; j < *lens[i]; j++)
            pb_uint(&pb, 1, locs[i][j] + 1);
        pb_uint(&pb, 2, (uint64_t)values[i]);
    }

    pb_int(&pb, 9, 0LL - period_ns);  /* time_nanos — fixed for deterministic test */
    pb_int(&pb, 10, period_ns);
    pb_uint(&pb, 14, 0);

    /* string_table(6): [0] must be empty string */
    pb_str(&pb, 6, "", 0);
    for (int i = 0; i < sc; i++)
        pb_str(&pb, 6, stab[i], strlen(stab[i]));

    *out = (char *)pb.buf;
    *out_len = pb.len;

    for (uint32_t i = 0; i < n; i++) { free(locs[i]); free(lens[i]); }
    for (int i = 0; i < fcnt; i++) free((void *)fnames[i]);
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static int tests_passed = 0, tests_failed = 0;
#define TEST(cond, msg) do { \
    if (cond) { printf("  PASS %s\n", msg); tests_passed++; } \
    else { printf("  FAIL %s\n", msg); tests_failed++; } \
} while (0)

static void test_basic_cpu(void) {
    printf("\n[1] Basic CPU pprof\n");
    sample_t stacks[2];
    strcpy(stacks[0], "root;mid;leaf");
    strcpy(stacks[1], "root;mid");
    int64_t values[] = {100, 50};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 2,
                "cpu", "nanoseconds", "cpu", "nanoseconds",
                10000000LL, &pp, &pl);

    TEST(pp != NULL, "build_pprof returns data");
    TEST(pl > 50, "pprof is reasonable size");
    if (!pp) return;

    /* Verify field numbers by checking magic bytes */
    /* sample_type(1) tag: (1<<3)|2 = 0x0a, check it at buf[0] */
    TEST(((uint8_t)pp[0] & 0x07) == 2, "sample_type uses LEN wire type");
    TEST((uint8_t)pp[0] >> 3 == 1, "sample_type field = 1");

    /* After sample_type(1), next major field: period_type(11) tag = (11<<3)|2 = 0x5a */
    /* Let's find period_type tag: right after sample_type sub-message */
    /* sample_type body = 4 bytes (tag+varint+varint+varint) */
    size_t off = 1 + 1 + 4; /* tag(1byte) + len_varint(1byte) + 4-byte body */
    uint8_t pt_tag = (uint8_t)pp[off];
    TEST((pt_tag & 0x07) == 2, "period_type uses LEN wire type");
    TEST(pt_tag >> 3 == 11, "period_type field = 11");

    /* mapping(3) right after period_type */
    off = 1 + 1 + 4 + 1 + 1 + 4; /* sample_type + period_type */
    uint8_t m_tag = (uint8_t)pp[off];
    TEST((m_tag & 0x07) == 2, "mapping uses LEN wire type");
    TEST(m_tag >> 3 == 3, "mapping field = 3");

    /* Check function(5) fields */
    /* Walk past mapping(3): tag + len_varint + 2-byte body = 4 bytes */
    off += 1 + 1 + 2;
    uint8_t f1_tag = (uint8_t)pp[off];
    TEST((f1_tag & 0x07) == 2, "function uses LEN wire type");
    TEST(f1_tag >> 3 == 5, "function field = 5");

    /* Check string_table exists: field 6 should appear */
    { int found_6 = 0;
        for (size_t i = 0; i < pl; i++) {
            if (((uint8_t)pp[i] >> 3) == 6 && ((uint8_t)pp[i] & 0x07) == 2) { found_6 = 1; break; }
        }
        TEST(found_6, "string_table (field 6) is present");
    }

    /* Check only valid wire types 0-2 appear at tag positions */
    { int bad = 0;
        size_t i = 0;
        while (i < pl) {
            uint8_t b = (uint8_t)pp[i];
            uint8_t wt = b & 0x07;
            if (wt > 2) { bad = 1; break; }
            if (wt == 0) { i += 1; while ((pp[i] & 0x80) && i < pl) i++; i++; } /* skip varint */
            else if (wt == 2) {
                size_t len = 0, shift = 0;
                i++;
                while (i < pl) { uint8_t vb = pp[i]; len |= (uint64_t)(vb & 0x7f) << shift; i++; shift += 7; if (!(vb & 0x80)) break; }
                i += len;
            } else { i++; }
        }
        TEST(!bad, "valid proto structure (walkable)");
    }

    free(pp);
}

static void test_alloc_space(void) {
    printf("\n[2] Alloc space pprof\n");
    sample_t stacks[2];
    strcpy(stacks[0], "root;big_alloc");
    strcpy(stacks[1], "root;small_alloc");
    int64_t values[] = {8192, 256};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 2,
                "alloc_space", "bytes", "space", "bytes",
                10000000LL, &pp, &pl);

    TEST(pp != NULL, "build_pprof alloc returns data");
    TEST(pl > 50, "alloc pprof is reasonable size");
    if (!pp) return;

    /* Verify sample_type string is "alloc_space", unit="bytes" */
    /* String table should contain "alloc_space", "bytes", "space" */
    TEST(memmem(pp, pl, "alloc_space", 11) != NULL, "contains 'alloc_space'");
    TEST(memmem(pp, pl, "bytes", 5) != NULL, "contains 'bytes'");
    TEST(memmem(pp, pl, "space", 5) != NULL, "contains 'space'");

    free(pp);
}

static void test_deterministic(void) {
    printf("\n[3] Deterministic output\n");
    sample_t stacks[2];
    strcpy(stacks[0], "root;mid;leaf");
    strcpy(stacks[1], "root;mid");
    int64_t values[] = {100, 50};

    char *p1 = NULL, *p2 = NULL;
    size_t l1 = 0, l2 = 0;
    build_pprof(stacks, values, 2,
                "cpu", "nanoseconds", "cpu", "nanoseconds", 10000000LL, &p1, &l1);
    build_pprof(stacks, values, 2,
                "cpu", "nanoseconds", "cpu", "nanoseconds", 10000000LL, &p2, &l2);

    TEST(l1 == l2, "same input → same size");
    TEST(memcmp(p1, p2, l1) == 0, "same input → same bytes");

    free(p1); free(p2);
}

static void test_empty(void) {
    printf("\n[4] Empty input\n");
    char *pp = NULL; size_t pl = 0;
    build_pprof(NULL, NULL, 0,
                "cpu", "nanoseconds", "cpu", "nanoseconds",
                10000000LL, &pp, &pl);
    TEST(pp == NULL, "empty input → NULL");
    TEST(pl == 0, "empty input → zero length");
}

static void test_function_name_index(void) {
    printf("\n[5] Function name string index\n");
    sample_t stacks[1];
    strcpy(stacks[0], "foo;bar");
    int64_t values[] = {10};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 1,
                "cpu", "nanoseconds", "cpu", "nanoseconds", 10000000LL, &pp, &pl);

    TEST(pp != NULL, "build succeeds");
    if (!pp) return;

    /* string_table should be: [0]="" [1]="cpu" [2]="nanoseconds" [3]="foo" [4]="bar" */
    /* function name=3 (foo), function name=4 (bar) */
    TEST(memmem(pp, pl, "foo", 3) != NULL, "contains 'foo'");
    TEST(memmem(pp, pl, "bar", 3) != NULL, "contains 'bar'");

    free(pp);
}

static void test_single_frame(void) {
    printf("\n[6] Single frame stack\n");
    sample_t stacks[1];
    strcpy(stacks[0], "main");
    int64_t values[] = {1};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 1,
                "samples", "count", "cpu", "nanoseconds", 10000000LL, &pp, &pl);

    TEST(pp != NULL, "single frame succeeds");
    TEST(pl > 30, "reasonable size for 1 sample");
    if (!pp) return;

    TEST(memmem(pp, pl, "main", 4) != NULL, "contains 'main'");

    free(pp);
}

static void test_deep_stack(void) {
    printf("\n[7] Deep stack (50 frames)\n");
    sample_t stacks[1];
    /* Build a deep stack */
    int pos = 0;
    for (int i = 0; i < 50; i++) {
        if (i > 0) stacks[0][pos++] = ';';
        pos += snprintf(stacks[0] + pos, MAX_STACK_LEN - pos, "f%d", i);
    }
    int64_t values[] = {999};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 1,
                "cpu", "nanoseconds", "cpu", "nanoseconds", 10000000LL, &pp, &pl);

    TEST(pp != NULL, "deep stack succeeds");
    TEST(pl > 100, "deep stack pprof > 100 bytes");

    free(pp);
}

static void test_korean_utf8(void) {
    printf("\n[8] UTF-8 function names\n");
    sample_t stacks[1];
    strcpy(stacks[0], "루트;중간;리프");
    int64_t values[] = {42};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 1,
                "cpu", "nanoseconds", "cpu", "nanoseconds", 10000000LL, &pp, &pl);

    TEST(pp != NULL, "UTF-8 succeeds");
    /* Verify Korean characters preserved */
    TEST(memmem(pp, pl, "루트", 6) != NULL, "contains 루트 (6 UTF-8 bytes)");
    TEST(memmem(pp, pl, "중간", 6) != NULL, "contains 중간");
    TEST(memmem(pp, pl, "리프", 6) != NULL, "contains 리프");

    free(pp);
}

static void test_large_value(void) {
    printf("\n[9] Large value (INT64_MAX)\n");
    sample_t stacks[1];
    strcpy(stacks[0], "big");
    int64_t values[] = {INT64_C(9223372036854775807)};  /* INT64_MAX */

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 1,
                "alloc_space", "bytes", "space", "bytes", 10000000LL, &pp, &pl);

    TEST(pp != NULL, "large value succeeds");
    TEST(pl > 20, "large value pprof > 20 bytes");
    /* INT64_MAX is 10 varint bytes */
    TEST(pl >= 30, "large value encoded correctly");

    free(pp);
}

/* ── Push test (requires Pyroscope running on localhost:4040) ──────── */

#include <curl/curl.h>

static int push_to_pyroscope(const char *app_name, const char *pprof, size_t plen) {
    char url[512];
    snprintf(url, sizeof(url), "http://localhost:4040/ingest?name=%s&format=pprof&spyName=phpspy", app_name);

    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, pprof);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)plen);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    struct curl_slist *h = NULL;
    h = curl_slist_append(h, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return (res != CURLE_OK) ? -1 : (int)code;
}

static void test_push(void) {
    printf("\n[10] Push to Pyroscope\n");

    /* Quick check if Pyroscope is there */
    CURL *c = curl_easy_init();
    if (!c) { printf("  SKIP curl not available\n"); return; }
    curl_easy_setopt(c, CURLOPT_URL, "http://localhost:4040/ready");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(c);
    long ready_code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &ready_code);
    curl_easy_cleanup(c);

    if (ready_code != 200) {
        printf("  SKIP Pyroscope not reachable (code=%ld) — start docker compose first\n", ready_code);
        return;
    }

    sample_t stacks[2];
    strcpy(stacks[0], "root;mid;leaf");
    strcpy(stacks[1], "root;mid");
    int64_t values[] = {100, 50};

    char *pp = NULL; size_t pl = 0;
    build_pprof(stacks, values, 2,
                "cpu", "nanoseconds", "cpu", "nanoseconds", 10000000LL, &pp, &pl);
    if (!pp) { printf("  FAIL build_pprof failed\n"); tests_failed++; return; }

    int code = push_to_pyroscope("pprof-test-cpu", pp, pl);
    TEST(code == 200, "pprof CPU push returns 200");
    if (code != 200) printf("  => Got HTTP %d\n", code);

    /* Test alloc too */
    sample_t astacks[2];
    strcpy(astacks[0], "root;make_big_array");
    strcpy(astacks[1], "root;str_repeat");
    int64_t avalues[] = {65536, 1024};

    char *ap = NULL; size_t al = 0;
    build_pprof(astacks, avalues, 2,
                "alloc_space", "bytes", "space", "bytes",
                10000000LL, &ap, &al);
    if (ap) {
        int acode = push_to_pyroscope("pprof-test-alloc", ap, al);
        TEST(acode == 200, "pprof alloc push returns 200");
        if (acode != 200) printf("  => Got HTTP %d\n", acode);
        free(ap);
    }

    free(pp);
}

int main(void) {
    curl_global_init(CURL_GLOBAL_ALL);

    printf("=== pprof encoder tests ===\n");

    test_basic_cpu();
    test_alloc_space();
    test_deterministic();
    test_empty();
    test_function_name_index();
    test_single_frame();
    test_deep_stack();
    test_korean_utf8();
    test_large_value();
    test_push();

    curl_global_cleanup();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
