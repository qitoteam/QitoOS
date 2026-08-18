/*
 * Qira OS - network, executable and icon commands for UltraShell
 *
 * These live apart from ultrashell.c so the core file stays about the shell
 * rather than growing every time a subsystem gains a command. The table here
 * is appended to UltraShell's at registration.
 */

#include <kernel/shell.h>
#include <kernel/net.h>
#include <kernel/http.h>
#include <kernel/git.h>
#include <kernel/lqx.h>
#include <kernel/qac.h>
#include <kernel/font.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>

/* --- networking -------------------------------------------------------- */

static int cmd_fetch(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: fetch <url> [-o file] [-H]");
        shell_printf(sh, "  -o  write the body to a file instead of the screen\n");
        shell_printf(sh, "  -H  show the response headers\n");
        return 1;
    }

    const char *url         = argv[1];
    const char *destination = NULL;
    bool_t      show_headers = false;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            destination = argv[++i];
        } else if (strcmp(argv[i], "-H") == 0) {
            show_headers = true;
        }
    }

    struct http_response response;
    uint64_t start = time_uptime_ms();

    if (http_get(url, &response) != 0) {
        shell_error(sh, "fetch: %s", response.error);
        return 1;
    }

    uint64_t elapsed = time_uptime_ms() - start;

    shell_color(sh, "\033[96m");
    shell_printf(sh, "%d %s", response.status,
                 http_status_text(response.status));
    shell_reset_color(sh);
    shell_printf(sh, "  %llu bytes in %llu ms\n",
                 (unsigned long long)response.body_length,
                 (unsigned long long)elapsed);

    if (show_headers) {
        shell_printf(sh, "\n%s\n\n", response.headers);
    }

    if (destination) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh, destination, resolved, sizeof(resolved));

        if (fs_write_file(resolved, response.body, response.body_length) == 0) {
            shell_printf(sh, "wrote %llu bytes to %s\n",
                         (unsigned long long)response.body_length, resolved);
        } else {
            shell_error(sh, "fetch: cannot write %s", resolved);
            http_free(&response);
            return 1;
        }
    } else {
        shell_write(sh, response.body, response.body_length);
        if (response.body_length &&
            response.body[response.body_length - 1] != '\n') {
            shell_printf(sh, "\n");
        }
    }

    http_free(&response);
    return 0;
}

static int cmd_lookup(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: lookup <hostname>");
        return 1;
    }

    char server[24];
    net_format_ip(net_dns_server(), server, sizeof(server));
    shell_printf(sh, "Resolver: %s\n", server);

    uint64_t start = time_uptime_ms();
    ipv4_addr_t address;

    if (dns_resolve(argv[1], &address, 5000) != 0) {
        shell_error(sh, "lookup: no answer for %s", argv[1]);
        return 1;
    }

    char text[24];
    net_format_ip(address, text, sizeof(text));
    shell_printf(sh, "%s has address %s  (%llu ms)\n", argv[1], text,
                 (unsigned long long)(time_uptime_ms() - start));
    return 0;
}

static int cmd_git(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(sh, "usage: git <ls-remote|clone|version> [url]\n\n");
        shell_printf(sh, "Qira speaks git's smart HTTP protocol. Reference\n");
        shell_printf(sh, "discovery is complete; clone downloads a packfile\n");
        shell_printf(sh, "but cannot unpack it yet, because that needs zlib\n");
        shell_printf(sh, "inflate and delta resolution.\n\n");
        shell_printf(sh, "There is no TLS, so only http:// remotes work.\n");
        return 1;
    }

    if (strcmp(argv[1], "version") == 0) {
        shell_printf(sh, "qira-git, part of Qira OS\n");
        shell_printf(sh, "protocol: smart HTTP (git-upload-pack)\n");
        shell_printf(sh, "supported: ls-remote, fetch-pack\n");
        shell_printf(sh, "missing:   packfile inflation, working trees, push\n");
        return 0;
    }

    if (strcmp(argv[1], "ls-remote") == 0) {
        if (argc < 3) {
            shell_error(sh, "usage: git ls-remote <url>");
            return 1;
        }

        static struct git_ref refs[GIT_MAX_REFS];
        char error[128] = "";

        shell_printf(sh, "Querying %s...\n", argv[2]);

        int count = git_ls_remote(argv[2], refs, GIT_MAX_REFS, error,
                                  sizeof(error));
        if (count < 0) {
            shell_error(sh, "git: %s", error);
            return 1;
        }

        for (int i = 0; i < count; i++) {
            const char *kind;
            switch (git_ref_kind(refs[i].name)) {
            case GIT_REF_BRANCH: kind = "branch"; break;
            case GIT_REF_TAG:    kind = "tag";    break;
            case GIT_REF_HEAD:   kind = "HEAD";   break;
            default:             kind = "ref";    break;
            }

            shell_printf(sh, "%s\t%-8s %s\n", refs[i].hash, kind, refs[i].name);
        }

        shell_printf(sh, "\n%d reference(s)\n", count);
        return 0;
    }

    if (strcmp(argv[1], "clone") == 0) {
        if (argc < 3) {
            shell_error(sh, "usage: git clone <url> [destination]");
            return 1;
        }

        static struct git_ref refs[GIT_MAX_REFS];
        char error[128] = "";

        shell_printf(sh, "Cloning %s\n", argv[2]);
        shell_printf(sh, "Discovering references...\n");

        int count = git_ls_remote(argv[2], refs, GIT_MAX_REFS, error,
                                  sizeof(error));
        if (count <= 0) {
            shell_error(sh, "git: %s", error[0] ? error : "no references found");
            return 1;
        }

        /* Prefer HEAD, then the default branch. */
        const char *want = refs[0].hash;
        const char *name = refs[0].name;
        for (int i = 0; i < count; i++) {
            if (strcmp(refs[i].name, "HEAD") == 0 ||
                strcmp(refs[i].name, "refs/heads/main") == 0 ||
                strcmp(refs[i].name, "refs/heads/master") == 0) {
                want = refs[i].hash;
                name = refs[i].name;
                break;
            }
        }

        shell_printf(sh, "Fetching %s (%s)...\n", git_ref_short_name(name), want);

        char destination[FS_PATH_MAX];
        if (argc > 3) {
            shell_resolve(sh, argv[3], destination, sizeof(destination));
        } else {
            shell_resolve(sh, "repository.pack", destination,
                          sizeof(destination));
        }

        struct git_fetch_result result;
        if (git_fetch_pack(argv[2], want, destination, &result, error,
                           sizeof(error)) != 0) {
            shell_error(sh, "git: %s", error);
            return 1;
        }

        shell_printf(sh, "\nReceived a v%u packfile: %u objects, %llu bytes\n",
                     result.pack_version, result.object_count,
                     (unsigned long long)result.pack_size);

        if (result.saved) {
            shell_printf(sh, "Saved to %s\n", result.path);
        }

        shell_color(sh, "\033[93m");
        shell_printf(sh, "\nThe packfile was downloaded but not unpacked.\n");
        shell_reset_color(sh);
        shell_printf(sh, "Qira cannot inflate packfiles yet, so no working\n");
        shell_printf(sh, "tree was created. The pack itself is intact and can\n");
        shell_printf(sh, "be opened with git on another machine.\n");
        return 0;
    }

    shell_error(sh, "git: unknown subcommand '%s'", argv[1]);
    return 1;
}

/* --- executables ------------------------------------------------------- */

static int cmd_lqx(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_printf(sh, "usage: lqx <info|verify|exports|run> [file]\n");
        return 1;
    }

    if (strcmp(argv[1], "exports") == 0) {
        shell_printf(sh, "Kernel services available to LQX programs:\n\n");
        int count = lqx_export_count();
        for (int i = 0; i < count; i++) {
            shell_printf(sh, "  %-22s", lqx_export_name(i));
            if ((i % 3) == 2) {
                shell_printf(sh, "\n");
            }
        }
        if (count % 3) {
            shell_printf(sh, "\n");
        }
        shell_printf(sh, "\n%d service(s) exported.\n", count);
        return 0;
    }

    if (argc < 3) {
        shell_error(sh, "usage: lqx %s <file>", argv[1]);
        return 1;
    }

    char resolved[FS_PATH_MAX];
    shell_resolve(sh, argv[2], resolved, sizeof(resolved));

    if (strcmp(argv[1], "info") == 0) {
        struct qx_header header;
        if (lqx_probe(resolved, &header) != 0) {
            shell_error(sh, "lqx: %s is not an LQX image", argv[2]);
            return 1;
        }
        shell_printf(sh, "%s\n", resolved);
        lqx_describe(&header, sh);
        return 0;
    }

    if (strcmp(argv[1], "verify") == 0) {
        struct fs_stat stat;
        if (fs_stat(resolved, &stat) != 0) {
            shell_error(sh, "lqx: %s: no such file", argv[2]);
            return 1;
        }

        void *buffer = kmalloc(stat.size);
        if (!buffer) {
            shell_error(sh, "lqx: out of memory");
            return 1;
        }

        size_t got = 0;
        fs_read_file(resolved, buffer, stat.size, &got);

        const char *reason = NULL;
        int result = lqx_validate(buffer, got, &reason);
        kfree(buffer);

        if (result == 0) {
            shell_color(sh, "\033[92m");
            shell_printf(sh, "%s is a valid LQX image\n", argv[2]);
            shell_reset_color(sh);
            return 0;
        }

        shell_color(sh, "\033[91m");
        shell_printf(sh, "%s is not loadable: %s\n", argv[2],
                     reason ? reason : "unknown");
        shell_reset_color(sh);
        return 1;
    }

    if (strcmp(argv[1], "run") == 0) {
        struct lqx_image image;
        if (lqx_load(resolved, &image) != 0) {
            shell_error(sh, "lqx: could not load %s (see the log for why)",
                        argv[2]);
            return 1;
        }

        int pid = lqx_run(&image, argc - 2, argv + 2);
        if (pid < 0) {
            shell_error(sh, "lqx: could not start the program");
            lqx_unload(&image);
            return 1;
        }

        shell_printf(sh, "started '%s' as pid %d\n", image.name, pid);
        return 0;
    }

    shell_error(sh, "lqx: unknown subcommand '%s'", argv[1]);
    return 1;
}

/* --- icons ------------------------------------------------------------- */

static int cmd_qac(struct shell *sh, int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        int count = qac_registry_count();
        shell_printf(sh, "Loaded icons (%d):\n\n", count);
        for (int i = 0; i < count; i++) {
            const char *name = qac_registry_name(i);
            const struct qac_image *image = qac_get(name);
            shell_printf(sh, "  %-16s %dx%d\n", name,
                         image ? image->width : 0, image ? image->height : 0);
        }
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        if (argc < 3) {
            shell_error(sh, "usage: qac info <file>");
            return 1;
        }

        char resolved[FS_PATH_MAX];
        shell_resolve(sh, argv[2], resolved, sizeof(resolved));

        char   buffer[2048];
        size_t got = 0;
        if (fs_read_file(resolved, buffer, sizeof(buffer), &got) != 0) {
            shell_error(sh, "qac: %s: cannot read", argv[2]);
            return 1;
        }

        struct qac_header header;
        if (qac_probe(buffer, got, &header) != 0) {
            shell_error(sh, "qac: %s is not a QAC icon", argv[2]);
            return 1;
        }

        shell_printf(sh, "%s\n", resolved);
        shell_printf(sh, "  %-14s QACI version %u\n", "Format", header.version);
        shell_printf(sh, "  %-14s %.12s\n", "Name", header.name);
        shell_printf(sh, "  %-14s %u\n", "Frames", header.frame_count);
        shell_printf(sh, "  %-14s %u bytes\n", "Payload", header.payload_size);
        shell_printf(sh, "  %-14s 0x%08x\n", "Checksum", header.checksum);

        /* Decode the largest frame so the real dimensions can be shown. */
        struct qac_image image;
        if (qac_load(resolved, 256, &image) == 0) {
            shell_printf(sh, "  %-14s %dx%d\n", "Largest frame", image.width,
                         image.height);
            int raw = image.width * image.height * 4;
            shell_printf(sh, "  %-14s %dx smaller than raw\n", "Compression",
                         header.payload_size
                             ? raw / (int)header.payload_size
                             : 1);
            qac_free(&image);
        }
        return 0;
    }

    shell_error(sh, "qac: unknown subcommand '%s'", argv[1]);
    shell_printf(sh, "usage: qac [list|info <file>]\n");
    return 1;
}

/* --- fonts ------------------------------------------------------------- */

static int cmd_fonts(struct shell *sh, int argc, char **argv)
{
    /*
     * "set" needs both a target and a font id. Check for the incomplete form
     * only — an earlier version tested argc >= 3 here, which swallowed the
     * complete four-argument form before it could ever be handled.
     */
    if (argc < 4 && argc >= 2 && strcmp(argv[1], "set") == 0) {
        shell_error(sh, "usage: fonts set <ui|terminal> <font-id>");
        return 1;
    }

    if (argc >= 4 && strcmp(argv[1], "set") == 0) {
        if (strcmp(argv[2], "ui") == 0) {
            font_set_ui(argv[3]);
        } else if (strcmp(argv[2], "terminal") == 0) {
            font_set_terminal(argv[3]);
        } else {
            shell_error(sh, "fonts: expected 'ui' or 'terminal'");
            return 1;
        }
        shell_printf(sh, "%s font set to %s\n", argv[2], argv[3]);
        return 0;
    }

    shell_printf(sh, "%-18s %-10s %-6s %s\n", "ID", "WEIGHT", "MONO",
                 "DESCRIPTION");

    for (int i = 0; i < font_count(); i++) {
        const struct font *font = font_at(i);
        bool_t is_ui       = (font == font_ui());
        bool_t is_terminal = (font == font_terminal());

        shell_printf(sh, "%-18s %-10s %-6s %s", font->id,
                     font->weight == FONT_BOLD ? "bold" : "regular",
                     font->monospace ? "yes" : "no", font->description);

        if (is_ui || is_terminal) {
            shell_color(sh, "\033[92m");
            shell_printf(sh, "  [%s%s%s]", is_ui ? "interface" : "",
                         (is_ui && is_terminal) ? ", " : "",
                         is_terminal ? "terminal" : "");
            shell_reset_color(sh);
        }
        shell_printf(sh, "\n");
    }

    shell_printf(sh, "\nChange one with: fonts set <ui|terminal> <id>\n");

    /* A specimen line, so the difference is visible rather than described. */
    shell_printf(sh, "\nSpecimen: The quick brown fox jumps over 1,234 lazy "
                     "dogs. Il1O0\n");
    return 0;
}

/* --- the table --------------------------------------------------------- */

static const struct shell_command commands[] = {
    {"fetch", "download a URL over HTTP", "fetch <url> [-o file] [-H]",
     "Fetches a page or file over plain HTTP. Qira has no TLS, so https\n"
     "addresses are refused.",
     cmd_fetch, 0},

    {"lookup", "resolve a hostname", "lookup <hostname>", NULL, cmd_lookup, 0},

    {"git", "interact with a git remote",
     "git <ls-remote|clone|version> [url]",
     "Speaks git's smart HTTP protocol. Reference discovery works fully;\n"
     "clone downloads a packfile but cannot unpack it yet.",
     cmd_git, 0},

    {"lqx", "inspect and run Qira executables",
     "lqx <info|verify|exports|run> [file]",
     "LQX is the native executable format: a QX header followed by sections,\n"
     "imports and symbols. See docs/LQX.md.",
     cmd_lqx, 0},

    {"qac", "inspect Qira icon files", "qac [list|info <file>]",
     "QAC is the icon format: several sizes per file, each independently\n"
     "encoded as raw, run-length or palette data.",
     cmd_qac, 0},

    {"fonts", "list or change the system typefaces",
     "fonts [set <ui|terminal> <id>]", NULL, cmd_fonts, 0},
};

const struct shell_command *ultrashell_net_commands(int *count)
{
    *count = (int)ARRAY_SIZE(commands);
    return commands;
}
