/*
 * A minimal QTX program.
 * See docs/QTX.md.
 */

#include <qito/console.h>

int main(int argc, char **argv) {
    console_puts("Hello from a QTX program.\n");
    if (argc>1) {
        console_puts("Args:\n");
        for (int i=0;i<argc;i++) {
            console_puts("  ");
            console_puts(argv[i]);
            console_puts("\n");
        }
    }
    return 0;
}
