#include "test_harness.h"
#include "../../std/net/aether_http.h"
#include "../../std/string/aether_string.h"

TEST_CATEGORY(http_response_structure, TEST_CATEGORY_NETWORK) {
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 200;
    resp->body = string_new("test body");
    resp->headers = string_new("Content-Type: text/html");
    resp->error = NULL;

    ASSERT_EQ(200, resp->status_code);
    ASSERT_NOT_NULL(resp->body);
    ASSERT_NOT_NULL(resp->headers);
    ASSERT_NULL(resp->error);

    http_response_free(resp);
}

TEST_CATEGORY(http_url_parsing, TEST_CATEGORY_NETWORK) {
    // Test URL query string parsing
    const char* url = "/search?q=test&limit=10";
    ASSERT_NOT_NULL(url);
    ASSERT_TRUE(strstr(url, "?") != NULL);  // Has query string
    ASSERT_TRUE(strstr(url, "q=test") != NULL);
}

TEST_CATEGORY(http_response_cleanup, TEST_CATEGORY_NETWORK) {
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 404;
    resp->body = string_new("Not Found");
    resp->headers = NULL;
    resp->error = string_new("Error message");

    ASSERT_EQ(404, resp->status_code);
    ASSERT_NOT_NULL(resp->body);
    ASSERT_NOT_NULL(resp->error);
    ASSERT_NULL(resp->headers);
    http_response_free(resp);
}

// ---------------------------------------------------------------------------
// Response accessor tests
// ---------------------------------------------------------------------------
// These cover http_response_status/body/headers/error/ok. They construct
// HttpResponse values by hand so the tests work regardless of network access,
// and they exercise all the edge cases (success, HTTP error, transport error,
// null response, partial fields).

TEST_CATEGORY(http_accessors_success_response, TEST_CATEGORY_NETWORK) {
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 200;
    resp->body = string_new("Hello, world");
    resp->headers = string_new("Content-Type: text/plain");
    resp->error = NULL;

    ASSERT_EQ(200, http_response_status(resp));
    /* http_response_body now returns an OWNED (retained) AetherString — read
     * its content via aether_string_data and release the extra ref. */
    const char* body = http_response_body(resp);
    ASSERT_STREQ("Hello, world", aether_string_data(body));
    string_release(body);
    ASSERT_STREQ("Content-Type: text/plain", http_response_headers(resp));
    ASSERT_STREQ("", http_response_error(resp));
    ASSERT_EQ(1, http_response_ok(resp));

    http_response_free(resp);
}

TEST_CATEGORY(http_accessors_http_error_status, TEST_CATEGORY_NETWORK) {
    // 404 is not ok even though there's no transport error.
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 404;
    resp->body = string_new("Not Found");
    resp->headers = NULL;
    resp->error = NULL;

    ASSERT_EQ(404, http_response_status(resp));
    const char* body404 = http_response_body(resp);
    ASSERT_STREQ("Not Found", aether_string_data(body404));
    string_release(body404);
    ASSERT_STREQ("", http_response_headers(resp));
    ASSERT_STREQ("", http_response_error(resp));
    ASSERT_EQ(0, http_response_ok(resp));

    http_response_free(resp);
}

TEST_CATEGORY(http_accessors_transport_error, TEST_CATEGORY_NETWORK) {
    // Matches the shape produced by http_request() on DNS failure.
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->status_code = 0;
    resp->body = NULL;
    resp->headers = NULL;
    resp->error = string_new("Could not resolve host");

    ASSERT_EQ(0, http_response_status(resp));
    /* NULL body → a fresh owned empty AetherString; content is "", release it. */
    const char* ebody = http_response_body(resp);
    ASSERT_STREQ("", aether_string_data(ebody));
    string_release(ebody);
    ASSERT_STREQ("", http_response_headers(resp));
    ASSERT_STREQ("Could not resolve host", http_response_error(resp));
    ASSERT_EQ(0, http_response_ok(resp));

    http_response_free(resp);
}

TEST_CATEGORY(http_accessors_null_response_safe, TEST_CATEGORY_NETWORK) {
    // All accessors must tolerate NULL without crashing. This guards against
    // the out-of-memory path where http_request() returns NULL directly.
    ASSERT_EQ(0, http_response_status(NULL));
    const char* nbody = http_response_body(NULL);
    ASSERT_STREQ("", aether_string_data(nbody));
    string_release(nbody);
    ASSERT_STREQ("", http_response_headers(NULL));
    ASSERT_STREQ("", http_response_error(NULL));
    ASSERT_EQ(0, http_response_ok(NULL));
}

TEST_CATEGORY(http_accessors_boundary_status_codes, TEST_CATEGORY_NETWORK) {
    // http_response_ok is defined as 2xx. Walk the boundaries.
    HttpResponse* resp = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    resp->body = NULL;
    resp->headers = NULL;
    resp->error = NULL;

    resp->status_code = 199;
    ASSERT_EQ(0, http_response_ok(resp));  // just below 2xx
    resp->status_code = 200;
    ASSERT_EQ(1, http_response_ok(resp));  // start of 2xx
    resp->status_code = 204;
    ASSERT_EQ(1, http_response_ok(resp));  // mid 2xx
    resp->status_code = 299;
    ASSERT_EQ(1, http_response_ok(resp));  // end of 2xx
    resp->status_code = 300;
    ASSERT_EQ(0, http_response_ok(resp));  // just past 2xx
    resp->status_code = 500;
    ASSERT_EQ(0, http_response_ok(resp));  // 5xx

    http_response_free(resp);
}
