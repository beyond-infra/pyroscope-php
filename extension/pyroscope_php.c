#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_compile.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

static void (*orig_execute_ex)(zend_execute_data *execute_data);

#define BUF_SIZE 65536
#define MAX_DEPTH 128
#define MAX_STACK_LEN 4096

typedef char sample_t[MAX_STACK_LEN];

static sample_t      *active_buf = NULL;
static sample_t      *drain_buf  = NULL;
static pthread_mutex_t buf_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t        active_count;

static int g_enabled = 1;

static char  *g_endpoint = NULL;
static char  *g_app_name = NULL;
static int    g_interval = 10;

static pthread_t g_push_thread;
static int       g_push_stop = 0;

static void walk_stack(zend_execute_data *ex, char *out, size_t out_sz) {
    /* Per frame we may emit up to 3 segments: [class][sep][funcname].
     * PHP stores a method's class separately in f->common.scope, so using only
     * function_name drops the class/namespace for methods (functions and closures
     * are already self-qualified in function_name). */
    const char *seg[MAX_DEPTH][3];
    size_t      len[MAX_DEPTH][3];
    int         ns[MAX_DEPTH];
    int d = 0;
    while (ex && d < MAX_DEPTH) {
        zend_function *f = ex->func;
        if (f && f->common.function_name) {
            int s = 0;
            zend_class_entry *scope = f->common.scope;
            if (scope && !(f->common.fn_flags & ZEND_ACC_CLOSURE)) {
                seg[d][s] = ZSTR_VAL(scope->name);
                len[d][s] = ZSTR_LEN(scope->name);
                s++;
                /* ponytail: :: for static, -> for instance — phpspy convention,
                 * keeps the call-flavor distinction visible in the flamegraph. */
                if (f->common.fn_flags & ZEND_ACC_STATIC) {
                    seg[d][s] = "::"; len[d][s] = 2;
                } else {
                    seg[d][s] = "->"; len[d][s] = 2;
                }
                s++;
            }
            seg[d][s] = ZSTR_VAL(f->common.function_name);
            len[d][s] = ZSTR_LEN(f->common.function_name);
            s++;
            ns[d] = s;
            d++;
        }
        ex = ex->prev_execute_data;
    }
    char *p = out, *end = out + out_sz - 1;
    for (int i = d - 1; i >= 0 && p < end; i--) {
        char *frame_start = p;  /* drop whole frame on truncation, not half */
        if (i < d - 1) { *p++ = ';'; }
        int s;
        for (s = 0; s < ns[i]; s++) {
            if (p + len[i][s] >= end) break;
            memcpy(p, seg[i][s], len[i][s]); p += len[i][s];
        }
        if (s < ns[i]) { p = frame_start; break; }  /* frame didn't fit whole */
    }
    *p = '\0';
}

static void cp_execute_ex(zend_execute_data *execute_data) {
    zend_function *f = EX(func);
    /* ponytail: always run the original executor — frames without a function
     * name (main script, include/eval) must still execute, only profiling is
     * skipped. Returning early here used to swallow all top-level code. */
    if (f && f->common.function_name && g_enabled) {
        pthread_mutex_lock(&buf_mutex);
        if (active_count < BUF_SIZE) {
            walk_stack(execute_data, active_buf[active_count], MAX_STACK_LEN);
            active_count++;
        }
        pthread_mutex_unlock(&buf_mutex);
    }
    orig_execute_ex(execute_data);
}

/* ── protobuf wire encoder ─────────────────────────────────────────── */

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

/* ── pprof build — heap-based, no large stack arrays ───────────────── */

static int str_idx(const char **tab, int *cnt, int max, const char *s) {
    if (!s || !*s) return 0;
    for (int i = 0; i < *cnt; i++)
        if (strcmp(tab[i], s) == 0) return i + 1;
    if (*cnt >= max) return 0;
    int idx = (*cnt) + 1;
    tab[*cnt] = s;
    (*cnt)++;
    return idx;
}

static void build_pprof(sample_t *samples, int64_t *values, uint32_t n,
                        const char *stype, const char *sunit,
                        const char *ptype, const char *punit,
                        char **out, size_t *out_len) {
    if (n == 0) { *out = NULL; *out_len = 0; return; }

    /* Estimate frame count upper bound: sum of frames across all samples */
    int max_frames_total = 0;
    for (uint32_t i = 0; i < n; i++) {
        int nf = 1;
        for (char *c = samples[i]; *c; c++) if (*c == ';') nf++;
        max_frames_total += nf;
    }
    int max_str = max_frames_total + 8;
    const char **stab = calloc(max_str, sizeof(char *));
    if (!stab) { *out = NULL; *out_len = 0; return; }
    int sc = 0;
    str_idx(stab, &sc, max_str, "");
    int si_st  = str_idx(stab, &sc, max_str, stype);
    int si_su  = str_idx(stab, &sc, max_str, sunit);
    int si_pt  = str_idx(stab, &sc, max_str, ptype);
    int si_pu  = str_idx(stab, &sc, max_str, punit);

    const char **fnames = calloc(max_frames_total, sizeof(char *));
    int *fi = calloc(max_frames_total, sizeof(int));
    int fcnt = 0;

    int **locs = calloc(n, sizeof(int *));
    int *lens = calloc(n, sizeof(int));

    if (!fnames || !fi || !locs || !lens) {
        free(stab); free(fnames); free(fi); free(locs); free(lens);
        *out = NULL; *out_len = 0; return;
    }

    for (uint32_t i = 0; i < n; i++) {
        char *s = samples[i];
        int nf = 1;
        for (char *c = s; *c; c++) if (*c == ';') nf++;
        lens[i] = nf;
        locs[i] = calloc(nf, sizeof(int));
        if (!locs[i]) continue;

        for (int j = 0; j < nf; j++) {
            char *sep = strchr(s, ';');
            size_t flen = sep ? (size_t)(sep - s) : strlen(s);
            char *name = malloc(flen + 1);
            if (!name) continue;
            memcpy(name, s, flen); name[flen] = '\0';
            int fsi = str_idx(stab, &sc, max_str, name);
            int found = -1;
            for (int k = 0; k < fcnt; k++) {
                if (fi[k] == fsi) { found = k; break; }
            }
            if (found < 0) {
                fnames[fcnt] = name;
                fi[fcnt] = fsi;
                found = fcnt++;
            } else {
                free(name);
            }
            locs[i][j] = found;
            if (sep) s = sep + 1; else break;
        }
    }

    size_t cap = 4096 + n * 256;
    uint8_t *buf = malloc(cap);
    if (!buf) { goto cleanup; }
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
        if (lens[i] == 0) continue;
        size_t body = 0;
        for (int j = 0; j < lens[i]; j++) {
            body += 2;
            uint64_t v = locs[i][j] + 1; while (v > 0x7f) { body++; v >>= 7; }
        }
        body += 2;
        { uint64_t v = (uint64_t)values[i]; while (v > 0x7f) { body++; v >>= 7; } }
        pb_msg(&pb, 2, body);
        for (int j = 0; j < lens[i]; j++)
            pb_uint(&pb, 1, locs[i][j] + 1);
        pb_int(&pb, 2, values[i]);
    }

    pb_int(&pb, 9, (int64_t)time(NULL) * 1000000000LL - INT64_C(10000000000));
    pb_int(&pb, 10, INT64_C(10000000000));
    pb_uint(&pb, 14, 0);
    pb_str(&pb, 6, "", 0);
    for (int i = 0; i < sc; i++)
        pb_str(&pb, 6, stab[i], strlen(stab[i]));

    *out = (char *)pb.buf;
    *out_len = pb.len;

cleanup:
    for (uint32_t i = 0; i < n; i++) free(locs[i]);
    free(locs); free(lens);
    for (int i = 0; i < fcnt; i++) free((void *)fnames[i]);
    free(fnames); free(fi);
    free(stab);
}

static int sample_cmp(const void *a, const void *b) {
    return strcmp(*(const sample_t *)a, *(const sample_t *)b);
}

static uint32_t merge_cpu(sample_t *slice, uint32_t n, int64_t *out_values) {
    if (n == 0) return 0;
    qsort(slice, n, sizeof(sample_t), sample_cmp);
    uint32_t uniq = 0, run_start = 0;
    for (uint32_t i = 1; i <= n; i++) {
        int same = (i < n) && (strcmp(slice[i], slice[run_start]) == 0);
        if (!same) {
            memcpy(slice[uniq], slice[run_start], sizeof(sample_t));
            out_values[uniq] = (int64_t)(i - run_start);
            uniq++; run_start = i;
        }
    }
    return uniq;
}

/* ── HTTP push (raw pprof, not multipart) ──────────────────────────── */

static int push_pprof(const char *endpoint, const char *app,
                       const char *pprof, size_t plen) {
    char url[512];
    int url_len = snprintf(url, sizeof(url), "%s/ingest?name=%s&format=pprof&spyName=phpspy", endpoint, app);
    if (url_len < 0 || url_len >= (int)sizeof(url)) return -1;

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

/* ── push threads ──────────────────────────────────────────────────── */

static void sig_block(void) {
    sigset_t m; sigfillset(&m); pthread_sigmask(SIG_BLOCK, &m, NULL);
}

#if defined(__APPLE__) || defined(__FreeBSD__)
#define SETNAME(n) pthread_setname_np(n)
#else
#define SETNAME(n) pthread_setname_np(pthread_self(), n)
#endif

static void *push_cpu(void *arg) {
    (void)arg; sig_block(); SETNAME("pyroscope_push");

    sample_t *merge_buf = malloc(BUF_SIZE * sizeof(sample_t));
    int64_t  *merge_vals = malloc(BUF_SIZE * sizeof(int64_t));
    if (!merge_buf || !merge_vals) { free(merge_buf); free(merge_vals); return NULL; }

    while (!g_push_stop) {
        /* ponytail: interruptible sleep, shutdown responds in ≤1s instead of interval+sleep */
        for (int i = 0; i < g_interval && !g_push_stop; i++) sleep(1);
        if (g_push_stop) break;

        pthread_mutex_lock(&buf_mutex);
        uint32_t n = active_count;
        sample_t *tmp = active_buf; active_buf = drain_buf; drain_buf = tmp;
        active_count = 0;
        pthread_mutex_unlock(&buf_mutex);

        if (n == 0) continue;
        memcpy(merge_buf, drain_buf, n * sizeof(sample_t));
        uint32_t uniq = merge_cpu(merge_buf, n, merge_vals);
        if (uniq == 0) continue;

        char *pp = NULL; size_t pl = 0;
        build_pprof(merge_buf, merge_vals, uniq,
                    "cpu", "nanoseconds", "cpu", "nanoseconds", &pp, &pl);
        if (!pp) continue;
        int code = push_pprof(g_endpoint, g_app_name, pp, pl);
        if (code != 200 && code != 204)
            fprintf(stderr, "pyroscope_php: CPU push failed code=%d\n", code);
        free(pp);
    }
    free(merge_buf); free(merge_vals);
    return NULL;
}

/* ── PHP API ───────────────────────────────────────────────────────── */

PHP_FUNCTION(pyroscope_php_folded) {
    pthread_mutex_lock(&buf_mutex);
    uint32_t n = active_count;
    if (n == 0) { pthread_mutex_unlock(&buf_mutex); RETURN_EMPTY_ARRAY(); }
    sample_t *slice = malloc(n * sizeof(sample_t));
    if (!slice) { pthread_mutex_unlock(&buf_mutex); RETURN_EMPTY_ARRAY(); }
    memcpy(slice, active_buf, n * sizeof(sample_t));
    pthread_mutex_unlock(&buf_mutex);

    qsort(slice, n, sizeof(sample_t), sample_cmp);
    array_init(return_value);
    uint32_t rs = 0;
    for (uint32_t i = 1; i <= n; i++) {
        int same = (i < n) && (strcmp(slice[i], slice[rs]) == 0);
        if (!same) {
            if (slice[rs][0]) {
                char line[MAX_STACK_LEN + 32];
                int w = snprintf(line, sizeof(line), "%s %u", slice[rs], i - rs);
                if (w > 0) add_next_index_stringl(return_value, line, w);
            }
            rs = i;
        }
    }
    free(slice);
}

PHP_FUNCTION(pyroscope_php_dump) {
    pthread_mutex_lock(&buf_mutex);
    uint32_t n = active_count;
    if (n == 0) { pthread_mutex_unlock(&buf_mutex); RETURN_EMPTY_ARRAY(); }
    sample_t *s = malloc(n * sizeof(sample_t));
    if (!s) { pthread_mutex_unlock(&buf_mutex); RETURN_EMPTY_ARRAY(); }
    memcpy(s, active_buf, n * sizeof(sample_t));
    pthread_mutex_unlock(&buf_mutex);
    array_init(return_value);
    for (uint32_t i = 0; i < n; i++) add_next_index_string(return_value, s[i]);
    free(s);
}

PHP_FUNCTION(pyroscope_php_reset) {
    pthread_mutex_lock(&buf_mutex); active_count = 0; pthread_mutex_unlock(&buf_mutex);
}
PHP_FUNCTION(pyroscope_php_count) {
    pthread_mutex_lock(&buf_mutex); uint32_t n = active_count; pthread_mutex_unlock(&buf_mutex);
    RETURN_LONG(n);
}
PHP_FUNCTION(pyroscope_php_buffer_cap) { RETURN_LONG(BUF_SIZE); }

PHP_FUNCTION(pyroscope_php_mode) { RETURN_STRING("cpu"); }

PHP_FUNCTION(pyroscope_php_enable) {
    zend_bool enabled;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_BOOL(enabled)
    ZEND_PARSE_PARAMETERS_END();
    g_enabled = enabled ? 1 : 0;
    RETURN_BOOL(g_enabled);
}

PHP_FUNCTION(pyroscope_php_is_enabled) {
    RETURN_BOOL(g_enabled ? 1 : 0);
}

ZEND_BEGIN_ARG_INFO_EX(ai_void, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_enable, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry pyroscope_php_functions[] = {
    PHP_FE(pyroscope_php_folded, ai_void)
    PHP_FE(pyroscope_php_dump, ai_void)
    PHP_FE(pyroscope_php_reset, ai_void)
    PHP_FE(pyroscope_php_count, ai_void)
    PHP_FE(pyroscope_php_buffer_cap, ai_void)
    PHP_FE(pyroscope_php_mode, ai_void)
    PHP_FE(pyroscope_php_enable, ai_enable)
    PHP_FE(pyroscope_php_is_enabled, ai_void)
    PHP_FE_END
};

PHP_MINIT_FUNCTION(pyroscope_php) {
    active_buf = calloc(BUF_SIZE, sizeof(sample_t));
    drain_buf  = calloc(BUF_SIZE, sizeof(sample_t));
    if (!active_buf || !drain_buf) {
        free(active_buf); free(drain_buf);
        active_buf = drain_buf = NULL;
        return FAILURE;
    }

    /* No curl_global_init here. PHP's curl extension owns global libcurl state
     * (it calls curl_global_init in its own MINIT); our calling it again — even
     * with CURL_GLOBAL_NOTHING — is first-call-wins and either clobbers the SSL
     * backend (if ALL) or skips it (if NOTHING, breaking HTTPS when we load
     * before the curl ext). If ext-curl is absent, curl_easy_init auto-inits
     * once, safely in the push thread. */
    orig_execute_ex = zend_execute_ex;
    zend_execute_ex = cp_execute_ex;
    active_count = 0;

    const char *ep = getenv("PYROSCOPE_ENDPOINT");
    const char *an = getenv("PYROSCOPE_APP_NAME");
    const char *iv = getenv("PYROSCOPE_INTERVAL");

    g_endpoint = strdup(ep ? ep : "http://127.0.0.1:4040");
    if (an) g_app_name = strdup(an);
    if (iv) { long v = atol(iv); if (v > 0 && v <= 3600) g_interval = (int)v; }

    if (g_app_name && pthread_create(&g_push_thread, NULL, push_cpu, NULL) != 0)
        free(g_app_name), g_app_name = NULL;  /* ponytail: create failed → skip push, don't join garbage tid */
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(pyroscope_php) {
    zend_execute_ex = orig_execute_ex;
    if (g_app_name) {
        g_push_stop = 1;
        pthread_join(g_push_thread, NULL);
        free(g_app_name);
    }
    free(g_endpoint);
    free(active_buf); free(drain_buf); pthread_mutex_destroy(&buf_mutex);
    /* No curl_global_cleanup — we never owned global libcurl state (see MINIT). */
    return SUCCESS;
}

PHP_MINFO_FUNCTION(pyroscope_php) {
    php_info_print_table_start();
    php_info_print_table_row(2, "pyroscope_php", "2.0.0");
    php_info_print_table_row(2, "format", "pprof");
    php_info_print_table_end();
}

zend_module_entry pyroscope_php_module_entry = {
    STANDARD_MODULE_HEADER,
    "pyroscope_php",
    pyroscope_php_functions,
    PHP_MINIT(pyroscope_php),
    PHP_MSHUTDOWN(pyroscope_php),
    NULL, NULL,
    PHP_MINFO(pyroscope_php),
    "2.0.0",
    STANDARD_MODULE_PROPERTIES
};

ZEND_GET_MODULE(pyroscope_php)
