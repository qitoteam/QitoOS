/*
 * Qira OS - HTTP client
 *
 * Plain HTTP/1.1 over the TCP layer. There is no TLS, so https:// URLs are
 * refused with an explanatory message rather than failing obscurely.
 */
#ifndef QIRA_HTTP_H
#define QIRA_HTTP_H

#include <kernel/types.h>

#define HTTP_DEFAULT_MAX (256 * 1024)

struct http_url {
    char     host[128];
    char     path[256];
    uint16_t port;
    bool_t   secure;
};

struct http_options {
    const char *method;          /* defaults to GET                       */
    const char *extra_headers;   /* CRLF-terminated lines                 */
    const void *body;
    size_t      body_length;
    size_t      max_size;        /* response cap; 0 for the default       */
    bool_t      no_redirect;
};

struct http_response {
    int    status;
    char  *body;                 /* heap allocated; free with http_free() */
    size_t body_length;
    char   content_type[64];
    char   headers[1024];
    char   error[128];
};

int  http_parse_url(const char *url, struct http_url *out);
int  http_get(const char *url, struct http_response *out);
int  http_request(const char *url, const struct http_options *options,
                  struct http_response *out);
void http_free(struct http_response *response);

const char *http_status_text(int status);

#endif /* QIRA_HTTP_H */
