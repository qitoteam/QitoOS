/*
 * Qira OS - git client
 *
 * Speaks git's smart HTTP protocol, which is the transport GitHub and most
 * hosts expose over port 80/443. Two operations are implemented:
 *
 *   ls-remote   GET  $URL/info/refs?service=git-upload-pack
 *   fetch       POST $URL/git-upload-pack with a want/done negotiation
 *
 * Both use pkt-line framing: every message is prefixed with its length as
 * four hexadecimal digits, and "0000" is a flush packet.
 *
 * What works and what does not
 * ----------------------------
 *
 * Reference discovery is complete: `git ls-remote` against a real host
 * returns the actual branch and tag list. Fetching downloads a real packfile
 * and reports what it received, but the packfile is *not* unpacked into a
 * working tree: that needs zlib inflate and delta resolution, which is a
 * substantial amount of code that has not been written yet. `git clone`
 * therefore saves the packfile and says so, rather than pretending to have
 * checked anything out.
 *
 * Since Qira has no TLS, only http:// remotes can be reached.
 */

#include <kernel/git.h>
#include <kernel/http.h>
#include <kernel/net.h>
#include <kernel/string.h>
#include <kernel/mm.h>
#include <kernel/printf.h>
#include <kernel/log.h>
#include <kernel/fs.h>

/* Read the four-digit hexadecimal length that prefixes a pkt-line. */
static int pkt_length(const char *data)
{
    int value = 0;

    for (int i = 0; i < 4; i++) {
        char c = data[i];
        int  digit;

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

/* Write a pkt-line: the payload prefixed with its total length. */
static size_t pkt_write(char *out, size_t capacity, const char *payload)
{
    size_t payload_len = strlen(payload);
    size_t total       = payload_len + 4;

    if (total + 1 > capacity) {
        return 0;
    }
    snprintf(out, capacity, "%04x%s", (unsigned)total, payload);
    return total;
}

/*
 * Parse the reference advertisement returned by info/refs. Each line after
 * the service header is "<sha> <refname>", possibly with capabilities after a
 * NUL on the first one.
 */
static int parse_refs(const char *data, size_t len, struct git_ref *refs,
                      int max_refs, char *capabilities, size_t cap_size)
{
    size_t position = 0;
    int    count    = 0;
    bool_t first    = true;

    while (position + 4 <= len && count < max_refs) {
        int length = pkt_length(data + position);

        if (length < 0) {
            break;
        }
        if (length == 0) {
            position += 4;    /* flush packet */
            continue;
        }
        if (length < 4 || position + (size_t)length > len) {
            break;
        }

        const char *line     = data + position + 4;
        size_t      line_len = (size_t)length - 4;
        position += (size_t)length;

        /* Skip the "# service=git-upload-pack" header. */
        if (line_len && line[0] == '#') {
            continue;
        }
        if (line_len < 41) {
            continue;
        }

        /* Capabilities follow a NUL on the first reference line. */
        size_t usable = line_len;
        for (size_t i = 0; i < line_len; i++) {
            if (line[i] == '\0') {
                usable = i;
                if (first && capabilities && i + 1 < line_len) {
                    size_t copy = MIN(line_len - i - 1, cap_size - 1);
                    memcpy(capabilities, line + i + 1, copy);
                    capabilities[copy] = '\0';
                }
                break;
            }
        }
        first = false;

        /* "<40 hex chars> <refname>" */
        if (usable < 42 || line[40] != ' ') {
            continue;
        }

        memcpy(refs[count].hash, line, 40);
        refs[count].hash[40] = '\0';

        size_t name_len = usable - 41;
        while (name_len > 0 &&
               (line[41 + name_len - 1] == '\n' ||
                line[41 + name_len - 1] == '\r')) {
            name_len--;
        }
        name_len = MIN(name_len, sizeof(refs[count].name) - 1);
        memcpy(refs[count].name, line + 41, name_len);
        refs[count].name[name_len] = '\0';

        count++;
    }

    return count;
}

int git_ls_remote(const char *url, struct git_ref *refs, int max_refs,
                  char *error, size_t error_size)
{
    char endpoint[512];

    /* Trim a trailing slash so the path does not double up. */
    char base[400];
    strlcpy(base, url, sizeof(base));
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base[--base_len] = '\0';
    }

    snprintf(endpoint, sizeof(endpoint),
             "%s/info/refs?service=git-upload-pack", base);

    struct http_response response;
    if (http_get(endpoint, &response) != 0) {
        if (error) {
            strlcpy(error, response.error[0] ? response.error
                                             : "the request failed",
                    error_size);
        }
        return -1;
    }

    if (response.status != 200) {
        if (error) {
            snprintf(error, error_size, "the server returned %d %s",
                     response.status, http_status_text(response.status));
        }
        http_free(&response);
        return -1;
    }

    char capabilities[256] = "";
    int  count = parse_refs(response.body, response.body_length, refs, max_refs,
                            capabilities, sizeof(capabilities));

    if (capabilities[0]) {
        KLOG_DEBUG("git", "server capabilities: %s", capabilities);
    }

    http_free(&response);

    if (count == 0 && error) {
        strlcpy(error, "the server advertised no references", error_size);
    }
    return count;
}

int git_fetch_pack(const char *url, const char *want_hash,
                   const char *destination, struct git_fetch_result *out,
                   char *error, size_t error_size)
{
    char base[400];
    strlcpy(base, url, sizeof(base));
    size_t base_len = strlen(base);
    while (base_len > 0 && base[base_len - 1] == '/') {
        base[--base_len] = '\0';
    }

    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint), "%s/git-upload-pack", base);

    /*
     * The negotiation: one "want" line naming the commit, a flush, then
     * "done". Capabilities are kept minimal because Qira cannot handle
     * side-band multiplexing or delta compression yet.
     */
    char request[512];
    size_t position = 0;

    char want[128];
    snprintf(want, sizeof(want), "want %s agent=qira/%s\n", want_hash,
             "0.4");
    position += pkt_write(request + position, sizeof(request) - position, want);

    /* A flush packet ends the want list. */
    memcpy(request + position, "0000", 4);
    position += 4;

    position += pkt_write(request + position, sizeof(request) - position, "done\n");

    struct http_options options = {
        .method = "POST",
        .extra_headers =
            "Content-Type: application/x-git-upload-pack-request\r\n"
            "Accept: application/x-git-upload-pack-result\r\n",
        .body        = request,
        .body_length = position,
        .max_size    = 4 * 1024 * 1024,
    };

    struct http_response response;
    if (http_request(endpoint, &options, &response) != 0) {
        if (error) {
            strlcpy(error, response.error[0] ? response.error
                                             : "the fetch request failed",
                    error_size);
        }
        return -1;
    }

    if (response.status != 200) {
        if (error) {
            snprintf(error, error_size, "the server returned %d %s",
                     response.status, http_status_text(response.status));
        }
        http_free(&response);
        return -1;
    }

    /*
     * The reply is pkt-lines ("NAK", progress) followed by the raw packfile,
     * which starts with the signature "PACK".
     */
    const char *pack = NULL;
    size_t      pack_len = 0;

    for (size_t i = 0; i + 4 <= response.body_length; i++) {
        if (memcmp(response.body + i, "PACK", 4) == 0) {
            pack     = response.body + i;
            pack_len = response.body_length - i;
            break;
        }
    }

    if (!pack) {
        if (error) {
            strlcpy(error, "the response contained no packfile", error_size);
        }
        http_free(&response);
        return -1;
    }

    /* The packfile header records its version and object count. */
    uint32_t version = 0;
    uint32_t objects = 0;
    if (pack_len >= 12) {
        version = ((uint32_t)(uint8_t)pack[4] << 24) |
                  ((uint32_t)(uint8_t)pack[5] << 16) |
                  ((uint32_t)(uint8_t)pack[6] << 8) | (uint8_t)pack[7];
        objects = ((uint32_t)(uint8_t)pack[8] << 24) |
                  ((uint32_t)(uint8_t)pack[9] << 16) |
                  ((uint32_t)(uint8_t)pack[10] << 8) | (uint8_t)pack[11];
    }

    if (out) {
        out->pack_size     = pack_len;
        out->pack_version  = version;
        out->object_count  = objects;
        out->saved         = false;
        out->path[0]       = '\0';
    }

    /* Save the packfile so the download is not thrown away. */
    if (destination) {
        if (fs_write_file(destination, pack, pack_len) == 0) {
            if (out) {
                out->saved = true;
                strlcpy(out->path, destination, sizeof(out->path));
            }
        } else if (error) {
            snprintf(error, error_size, "could not write %s", destination);
        }
    }

    KLOG_INFO("git", "fetched a v%u packfile: %u objects, %llu bytes", version,
              objects, (unsigned long long)pack_len);

    http_free(&response);
    return 0;
}

const char *git_ref_short_name(const char *full)
{
    if (strncmp(full, "refs/heads/", 11) == 0) {
        return full + 11;
    }
    if (strncmp(full, "refs/tags/", 10) == 0) {
        return full + 10;
    }
    if (strncmp(full, "refs/remotes/", 13) == 0) {
        return full + 13;
    }
    return full;
}

git_ref_kind_t git_ref_kind(const char *full)
{
    if (strncmp(full, "refs/heads/", 11) == 0) {
        return GIT_REF_BRANCH;
    }
    if (strncmp(full, "refs/tags/", 10) == 0) {
        return GIT_REF_TAG;
    }
    if (strcmp(full, "HEAD") == 0) {
        return GIT_REF_HEAD;
    }
    return GIT_REF_OTHER;
}
