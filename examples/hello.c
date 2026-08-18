/*
 * A minimal LQX program.
 *
 * It calls into the kernel through imported services rather than syscalls:
 * the loader patches the real addresses into these pointers before entry.
 * See docs/LQX.md.
 */

/* Imports. The loader writes each function's address here at load time. */
__attribute__((section(".data"))) void (*console_puts)(const char *) = 0;
__attribute__((section(".data"))) unsigned long long (*time_uptime_ms)(void) = 0;

int main(int argc, char **argv)
{
    console_puts("Hello from an LQX program.\n");

    if (argc > 1) {
        console_puts("Arguments:\n");
        for (int i = 1; i < argc; i++) {
            console_puts("  ");
            console_puts(argv[i]);
            console_puts("\n");
        }
    }

    return 0;
}
