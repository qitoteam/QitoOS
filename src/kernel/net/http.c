/*
 * QitoOS - HTTP client
 *
 * A blocking HTTP/1.1 client over the TCP layer. It handles the parts a
 * fetch tool and the git client actually need: GET and POST, request and
 * response headers, chunked transfer encoding, and redirects.
 *
 * TLS is not implemented, so this speaks plain HTTP only. That is stated
 * plainly wherever a user can reach it, rather than failing mysteriously on
 * an https:// URL.
 */

#include <kernel/http.h>
#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/mm.h>
#include <kernel/printf.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/version.h>

#define HTTP_TIMEOUT_MS   8000
#define HTTP_MAX_REDIRECT 4

int http_parse_url(const char *url, struct http_url *out)
{
    if (!url || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->port = 80;

    const char *cursor = url;

    /* Scheme. */
    if (strncmp(cursor, "http://", 7) == 0) {
        cursor += 7;
        out->secure = false;
    } else if (strncmp(cursor, "https://", 8) == 0) {
        cursor += 8;
        out->secure = true;
        out->port   = 443;
    } else if (strstr(cursor, "://")) {
        return -1;   /* some scheme we do not speak */
    }

    /* Host, up to a colon or a slash. */
    size_t index = 0;
    while (*cursor && *cursor != ':' && *cursor != '/' &&
           index < sizeof(out->host) - 1) {
        out->host[index++] = *cursor++;
    }
    out->host[index] = '\0';

    if (index == 0) {
        return -1;
    }

    /* Optional port. */
    if (*cursor == ':') {
        cursor++;
        int port = 0;
        while (isdigit((uint8_t)*cursor)) {
            port = port * 10 + (*cursor++ - '0');
        }
        if (port > 0 && port < 65536) {
            out->port = (uint16_t)port;
        }
    }

    /* Path; an empty path means "/". */
    if (*cursor == '/') {
        strlcpy(out->path, cursor, sizeof(out->path));
    } else {
        strlcpy(out->path, "/", sizeof(out->path));
    }

    return 0;
}

/* Case-insensitive search for a header, returning the value. */
static const char *find_header(const char *headers, const char *name,
                               char *out, size_t size)
{
    size_t name_len = strlen(name);
    const char *line = headers;

    while (line && *line) {
        if (strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *value = line + name_len + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }

            size_t index = 0;
            while (value[index] && value[index] != '\r' && value[index] != '\n' &&
                   index < size - 1) {
                out[index] = value[index];
                index++;
            }
            out[index] = '\0';
            return out;
        }

        line = strstr(line, "\r\n");
        if (!line) {
            break;
        }
        line += 2;
    }
    return NULL;
}

/* Decode a chunked body in place. Returns the decoded length. */
static size_t decode_chunked(char *body, size_t len)
{
    size_t read_at  = 0;
    size_t write_at = 0;

    while (read_at < len) {
        /* Each chunk begins with its length in hexadecimal. */
        size_t chunk_size = 0;
        bool_t any_digits = false;

        while (read_at < len) {
            char c = body[read_at];
            int  value;

            if (c >= '0' && c <= '9') {
                value = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                value = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                value = c - 'A' + 10;
            } else {
                break;
            }
            chunk_size = chunk_size * 16 + (size_t)value;
            any_digits = true;
            read_at++;
        }

        if (!any_digits) {
            break;
        }

        /* Skip any chunk extension, then the CRLF. */
        while (read_at < len && body[read_at] != '\n') {
            read_at++;
        }
        read_at++;

        if (chunk_size == 0) {
            break;   /* the terminating chunk */
        }
        if (read_at + chunk_size > len) {
            chunk_size = len - read_at;
        }

        memmove(body + write_at, body + read_at, chunk_size);
        write_at += chunk_size;
        read_at += chunk_size;

        /* Skip the CRLF that follows the chunk data. */
        while (read_at < len && (body[read_at] == '\r' || body[read_at] == '\n')) {
            read_at++;
        }
    }

    body[write_at] = '\0';
    return write_at;
}

int http_request(const char *url, const struct http_options *options,
                 struct http_response *out)
{
    if (!url || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    char current[512];
    strlcpy(current, url, sizeof(current));

    for (int redirect = 0; redirect <= HTTP_MAX_REDIRECT; redirect++) {
        struct http_url parsed;

        if (http_parse_url(current, &parsed) != 0) {
            strlcpy(out->error, "malformed URL", sizeof(out->error));
            return -1;
        }
        if (parsed.secure) {
            strlcpy(out->error,
                    "https is not supported: Qito has no TLS implementation",
                    sizeof(out->error));
            return -1;
        }

        /* Resolve the host. */
        ipv4_addr_t address;
        if (dns_resolve(parsed.host, &address, 4000) != 0) {
            snprintf(out->error, sizeof(out->error), "cannot resolve %s",
                     parsed.host);
            return -1;
        }

        struct tcp_socket *socket = tcp_connect(address, parsed.port, 5000);
        if (!socket) {
            snprintf(out->error, sizeof(out->error), "cannot connect to %s:%u",
                     parsed.host, parsed.port);
            return -1;
        }

        /* Build the request. */
        const char *method = (options && options->method) ? options->method : "GET";
        char request[1536];
        int  length = snprintf(
            request, sizeof(request),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: QitoOS/%s\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n",
            method, parsed.path, parsed.host, QITO_VERSION_STRING);

        if (options && options->extra_headers) {
            length += snprintf(request + length, sizeof(request) - (size_t)length,
                               "%s", options->extra_headers);
        }
        if (options && options->body && options->body_length) {
            length += snprintf(request + length, sizeof(request) - (size_t)length,
                               "Content-Length: %llu\r\n",
                               (unsigned long long)options->body_length);
        }
        length += snprintf(request + length, sizeof(request) - (size_t)length,
                           "\r\n");

        if (tcp_send(socket, request, (size_t)length) < 0) {
            tcp_close(socket);
            strlcpy(out->error, "failed to send the request", sizeof(out->error));
            return -1;
        }

        if (options && options->body && options->body_length) {
            tcp_send(socket, options->body, options->body_length);
        }

        /* Read the whole response. */
        size_t capacity = options && options->max_size ? options->max_size
                                                       : HTTP_DEFAULT_MAX;
        char  *buffer = kmalloc(capacity + 1);
        if (!buffer) {
            tcp_close(socket);
            strlcpy(out->error, "out of memory", sizeof(out->error));
            return -1;
        }

        size_t received = 0;
        uint64_t deadline = time_uptime_ms() + HTTP_TIMEOUT_MS;

        while (received < capacity && time_uptime_ms() < deadline) {
            ssize_t got = tcp_receive(socket, buffer + received,
                                      capacity - received, 1500);
            if (got <= 0) {
                if (tcp_socket_state(socket) != TCP_ESTABLISHED) {
                    break;
                }
                continue;
            }
            received += (size_t)got;
        }

        buffer[received] = '\0';
        tcp_close(socket);

        if (received == 0) {
            kfree(buffer);
            strlcpy(out->error, "the server sent no response",
                    sizeof(out->error));
            return -1;
        }

        /* Split the status line, headers and body. */
        char *separator = strstr(buffer, "\r\n\r\n");
        if (!separator) {
            kfree(buffer);
            strlcpy(out->error, "malformed response", sizeof(out->error));
            return -1;
        }

        *separator = '\0';
        char  *body     = separator + 4;
        size_t body_len = received - (size_t)(body - buffer);

        /* "HTTP/1.1 200 OK" */
        int status = 0;
        if (strncmp(buffer, "HTTP/1.", 7) == 0) {
            const char *space = strchr(buffer, ' ');
            if (space) {
                status = atoi(space + 1);
            }
        }
        out->status = status;

        char value[256];

        /* Follow a redirect, if the caller has not disabled it. */
        if (status >= 300 && status < 400 &&
            (!options || !options->no_redirect) &&
            find_header(buffer, "Location", value, sizeof(value))) {
            KLOG_DEBUG("http", "%d redirect to %s", status, value);

            if (value[0] == '/') {
                snprintf(current, sizeof(current), "http://%s%s", parsed.host,
                         value);
            } else {
                strlcpy(current, value, sizeof(current));
            }
            kfree(buffer);
            continue;
        }

        /* Decode a chunked body. */
        if (find_header(buffer, "Transfer-Encoding", value, sizeof(value)) &&
            strstr(value, "chunked")) {
            body_len = decode_chunked(body, body_len);
        }

        if (find_header(buffer, "Content-Type", value, sizeof(value))) {
            strlcpy(out->content_type, value, sizeof(out->content_type));
        }

        /*
         * Hand the body back in its own allocation so the caller does not
         * have to keep the header text alive with it.
         */
        out->body = kmalloc(body_len + 1);
        if (!out->body) {
            kfree(buffer);
            strlcpy(out->error, "out of memory", sizeof(out->error));
            return -1;
        }
        memcpy(out->body, body, body_len);
        out->body[body_len] = '\0';
        out->body_length    = body_len;

        strlcpy(out->headers, buffer,
                MIN(sizeof(out->headers), (size_t)(separator - buffer) + 1));

        kfree(buffer);

        KLOG_INFO("http", "%s %s -> %d, %llu bytes", method, parsed.host, status,
                  (unsigned long long)body_len);
        return 0;
    }

    strlcpy(out->error, "too many redirects", sizeof(out->error));
    return -1;
}

int http_get(const char *url, struct http_response *out)
{
    return http_request(url, NULL, out);
}

void http_free(struct http_response *response)
{
    if (response && response->body) {
        kfree(response->body);
        response->body        = NULL;
        response->body_length = 0;
    }
}

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 408: return "Request Timeout";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return (status >= 200 && status < 300) ? "Success" : "Error";
    }
}
