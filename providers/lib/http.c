#include "http.h"
#include "provider.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ATTEMPTS                = 4,
    CONNECT_TIMEOUT_SECONDS = 30,
    STALL_TIMEOUT_SECONDS   = 300,
    FIRST_BACKOFF_TICKS     = 10,
    BACKOFF_TICK_NS         = 100000000,
};

typedef enum { COMPLETE, ABORTED, FAILED } outcome;

static struct {
    const http_stream_api *api;
    buf                    pending, body;
    long                   http_status;
    http_failure           failure;
} st;

static void consume_lines(void)
{
    buf *pending = &st.pending;
    char *nl;
    while (pending->n && (nl = memchr(pending->p, '\n', pending->n))) {
        size_t n = (size_t)(nl - pending->p);
        span line = { pending->p, n && nl[-1] == '\r' ? n - 1 : n };
        if (line.n > 5 && !memcmp(line.p, "data:", 5)) {
            json data = json_trim((span){ line.p + 5, line.n - 5 });
            if (!st.api->event(data)) buf_set(&st.body, data);
        }
        buf_drop_front(pending, n + 1);
    }
}

static size_t on_data(char *bytes, size_t size, size_t n, void *curl)
{
    size_t len = size * n;
    if (!st.http_status) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &st.http_status);
    if (st.http_status != 200 || !st.api) {
        buf_put(&st.body, bytes, len);
        return len;
    }
    buf_put(&st.pending, bytes, len);
    consume_lines();
    return len;
}

static int on_progress(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)clientp, (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
    return provider_aborted() ? 1 : 0;
}

static CURLcode perform(const char *url, const strv *headers, span body, char *curl_error)
{
    CURL *c = curl_easy_init();
    struct curl_slist *h = NULL;
    for (size_t i = 0; i < headers->n; i++) h = curl_slist_append(h, headers->v[i]);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (body.p) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.p);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.n);
    }
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT_SECONDS);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)STALL_TIMEOUT_SECONDS);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st.http_status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return rc;
}

static outcome failed(http_failure failure)
{
    free(st.failure.message);
    st.failure = failure;
    return FAILED;
}

static outcome transfer(const char *url, const strv *headers, span body, http_failure_of failure)
{
    char curl_error[CURL_ERROR_SIZE] = "";
    st.pending.n = st.body.n = 0;
    st.http_status = 0;
    CURLcode rc = perform(url, headers, body, curl_error);
    if (provider_aborted()) return ABORTED;
    if (rc != CURLE_OK)
        return failed((http_failure){ "network", xstrdup(*curl_error ? curl_error : curl_easy_strerror(rc)), true, NO_BLOCK });
    if (st.http_status != 200 || (st.api && st.body.n)) return failed(failure(st.http_status, buf_span(&st.body)));
    if (st.api && !st.api->complete()) {
        buf message = { 0 };
        buf_printf(&message, "stream ended before %s", st.api->last_event);
        return failed((http_failure){ "network", buf_cstr(&message), true, NO_BLOCK });
    }
    return COMPLETE;
}

static bool backoff(int attempt)
{
    struct timespec tick = { 0, BACKOFF_TICK_NS };
    for (int i = 0; i < FIRST_BACKOFF_TICKS << (attempt - 1) && !provider_aborted(); i++) nanosleep(&tick, NULL);
    return !provider_aborted();
}

static void give_up(void)
{
    provider_error_at(st.failure.kind, st.failure.message, st.http_status, st.failure.at);
}

bool http_stream(const char *url, const strv *headers, span body, const http_stream_api *api)
{
    st.api = api;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    for (int attempt = 1;; attempt++) {
        if (provider_aborted()) return false;
        outcome o = transfer(url, headers, body, api->failure);
        if (o != FAILED) return o == COMPLETE;
        if (!st.failure.retryable || provider_emitted() || attempt == ATTEMPTS || !backoff(attempt)) {
            if (provider_aborted()) return false;
            give_up();
        }
    }
}

json http_get(const char *url, const strv *headers, http_failure_of failure)
{
    st.api = NULL;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (transfer(url, headers, NOSPAN, failure) == FAILED) give_up();
    return buf_span(&st.body);
}
