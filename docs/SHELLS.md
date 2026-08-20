# Shells

QitoOS ships two shells. They are not two skins over one command set —
they have different command tables, different prompts and different jobs.

| | QCSH | UltraShell |
| --- | --- | --- |
| Full name | QitoConfigShell | UltraShell |
| Purpose | system configuration, administration, diagnostics | general-purpose interactive work |
| Commands | 35 | 61 |
| Prompt | `qcsh>` | `user@qito:~$` |
| Inspiration | `busybox`-style admin tools, `systemctl` | Bash and PowerShell |
| Source | [`src/user/shells/qcsh/`](../src/user/shells/qcsh/) | [`src/user/shells/ultrashell/`](../src/user/shells/ultrashell/) |

Both are built on [`src/user/shells/shell_core.c`](../src/user/shells/shell_core.c),
which provides tokenising, quoting, line editing, history and dispatch.
Switch between them at any time: type `qcsh` in UltraShell, or `ush` in
QCSH.

---

## Shared infrastructure

### Parsing

The tokeniser handles single quotes (literal), double quotes (with `$VAR`
expansion) and backslash escapes. Environment variables expand as `$NAME`
or `${NAME}`.

### Exit status

`$?` holds the status of the last command. By convention:

| Status | Meaning |
| ---: | --- |
| 0 | success |
| 1–125 | command-specific failure |
| 126 | permission denied |
| 127 | command not found |

### Line editing

| Key | Action |
| --- | --- |
| ← → | move the cursor |
| ↑ ↓ | walk the history |
| Home / End | start / end of line |
| Ctrl-A / Ctrl-E | start / end of line |
| Ctrl-U | clear the line |
| Ctrl-W | delete the previous word |
| Ctrl-L | clear the screen |
| Ctrl-C | abandon the current line |
| Tab | complete a command or path |

### Limits

Lines are capped at 1024 characters. Commands needing a large working
buffer call `shell_scratch()` for a shared 16 KB region rather than
declaring a big local — the kernel task stack is 64 KB and a couple of
kilobytes of locals in a deep call chain will overflow it.

---

## QCSH — QitoConfigShell

The administrator's shell. Everything here inspects or changes system
state.

### System information

| Command | Description |
| --- | --- |
| `sysinfo` | one-screen summary: version, CPU, memory, uptime, display |
| `cpuinfo` | vendor, model, family, feature flags |
| `meminfo` | physical, heap and page-frame usage |
| `hwinfo` | PCI devices, storage, input and network hardware |
| `uptime` | time since boot and the load average |
| `version` | kernel version, codename and build ID |

### Diagnostics

| Command | Description |
| --- | --- |
| `diag` | eight subsystem health checks |
| `selftest` | 21 in-kernel assertions across heap, PMM, VMM, scheduler, FS and string routines |
| `benchmark` | memory bandwidth, heap churn and context-switch timings |
| `dmesg` | the kernel ring buffer |
| `loglevel [n]` | show or set verbosity, 0–4 |
| `irqinfo` | interrupt counts per vector |

### Processes

| Command | Description |
| --- | --- |
| `ps` | every task with state, priority and CPU time |
| `top` | continuously updating process view |
| `taskinfo <pid>` | detail for one task, including stack usage |
| `kill <pid>` | terminate a task |

### Configuration and services

| Command | Description |
| --- | --- |
| `config get\|set\|list\|save\|reset` | the key/value configuration store |
| `hostname [name]` | show or set the hostname |
| `service list\|start\|stop\|status` | manage system services |
| `pkg list\|info\|verify` | the component registry — 24 components |

### Storage, network, security, power

| Command | Description |
| --- | --- |
| `df` | filesystem usage |
| `mount` | mounted filesystems |
| `fsck` | check QitoFS integrity |
| `netinfo` | interfaces, addresses, routes and statistics |
| `ping <host>` | ICMP echo |
| `users` | accounts and their capabilities |
| `perms <path>` | permission bits for a path |
| `reboot`, `poweroff` | ACPI shutdown or reset |

### Example

```
qcsh> sysinfo
QitoOS 0.4.0 (Nova)
  processor    : QEMU Virtual CPU version 2.5+ (1 core)
  memory       : 511M total, 8M used, 503M free
  uptime       : 0d 00:00:12
  display      : 1024x768x32
  tasks        : 6 running
  ahci         : 1 port, persistence enabled
  qtx          : 27 services exported, Ring3 isolation
  qdl          : 1 loaded (libdemo.qdl)
  qti          : 12 icons loaded (16/32/64/128/256 default 64)
  qtpkg        : 4 packages in entry.var

qcsh> diag
  [ OK ]  Physical memory allocator    511M managed
  [ OK ]  Kernel heap                  8M allocated
  [ OK ]  Virtual memory               page tables consistent
  [ OK ]  Process scheduler            running (Ring3 tasks isolated)
  [ OK ]  Root filesystem              QitoFS, 18 entries + 12 QTI icons + 1 QTX + 1 QDL
  [ OK ]  Interrupt controller         PIC remapped 32-47, APIC/HPET high-res timing
  [ OK ]  System timer                 PIT 100 Hz, HPET monotonic, frame pacer 60fps
  [ OK ]  Framebuffer                  1024x768x32 + gfx3d depth buffer
8 checks, 8 passed

qcsh> config set terminal.font qito-mono-bold
qcsh> config save
configuration written to /etc/qito.conf

qcsh> qti list
Loaded QTI icons (12), default 64 px:
  terminal 64x64, files 64x64, etc – 5 sizes 16/32/64/128/256

qcsh> qtx exports
Kernel services available to QTX programs:
  console_write  console_puts  kmalloc  kfree  qti_load  qti_get  etc – 27 services

qcsh> qtpkg list
PACKAGE              VERSIONS  URLS
qasm                 2 vers: [1.0.0] [0.9.0]
  -> 1.0.0 => http://example.com/qasm-1.0.0.qtpkg_profile
```

---

## UltraShell

The general-purpose shell — 61 commands across four modules.

### Navigation and inspection

`pwd` `cd` `ls` `tree` `find` `stat` `df` `which`

`ls` supports `-l` (long), `-a` (all) and `-h` (human-readable sizes).

### Reading and writing files

`cat` `head` `tail` `wc` `grep` `sort` `uniq` `hexdump` `checksum`
`touch` `mkdir` `rm` `cp` `mv` `write`

`grep` supports `-i`, `-v`, `-n` and `-c`. `wc` supports `-l`, `-w` and `-c`,
and reads from a pipe when given no file:

```
ush> ls /etc | wc -l
      12
```

### Text and arithmetic

`echo` `rev` `upper` `lower` `calc` `seq` `yes` `test`

`calc` is an integer expression evaluator with correct precedence and
parentheses:

```
ush> calc (7+3)*4
40
ush> calc 2^10 % 1000
24
```

### Environment and scripting

`set` `unset` `env` `alias` `unalias` `source` `repeat` `history`

```
ush> set GREETING="hello world"
ush> echo $GREETING
hello world
ush> repeat 3 echo tick
tick
tick
tick
```

`source` runs a file of commands, which is how `/etc/profile` is applied
at startup.

### Networking, executables, icons, packages and tooling

| Command | Description |
| --- | --- |
| `fetch <url>` | HTTP GET (plain HTTP only, https gives honest TLS error) |
| `lookup <host>` | DNS resolution |
| `qtx info\|verify\|exports\|run\|list [path]` | inspect QTX executables (QX header 88B format X, W^X, checksum), list kernel exports, run as Ring3 user task |
| `qdl list\|load\|unload\|info [path]` | QDL dynamic libraries, format D, library flag, export table, refcounted, on-demand from /lib/*.qdl |
| `qti list\|info [path]` | QTI icons, real pixels, 5 sizes 16/32/64/128/256 default 64, RAW/RLE/INDEX encodings, largest-first |
| `qtpkg install\|update\|list\|search\|remove\|info\|upgrade\|-os update\|-fix os\|-fix --driver\|rollback` | package manager, entry /user/qtpkg/entry.var [version] (url); syntax, profile .qtpkg_profile, payload, checksums, TLS honest error, snapshots, -fix re-fetches corrupt |
| `qasm <input.s> -o <output.qtx> [--shared]` | QitoOS assembler, genuine x86-64 subset (mov, add, sub, lea, jmp, call, ret, etc), directives .section .text .data etc, produces .qtx/.qdl |
| `qcc <input.c> -o <output.qtx> [--shared]` | QitoOS C compiler, C subset (int/char/void/pointers/arrays/structs, if/else/while/for), thin driver over GCC, produces .qtx/.qdl |
| `fonts [set <ui\|terminal> <id>]` | list or choose typefaces qito-sans/mono, 8x16, bold derived row\|(row>>1) |

### Utilities

| Command | Description |
| --- | --- |
| `copy` / `paste` / `clipboard` | the system clipboard, shared with the editor and Notes |
| `random [max]` | a random number |
| `time <cmd>` | time a command |
| `watch <cmd>` | re-run a command periodically |
| `free` | memory usage |
| `load` | load average |
| `date` `uptime` `whoami` `sleep` `clear` | the usual |

### Pipelines, redirection and chaining

```
ush> cat /proc/meminfo | grep Heap
ush> ls -l /etc | sort | head -5
ush> echo "hello" > /tmp/a.txt
ush> cat /etc/motd >> /tmp/a.txt
ush> mkdir /tmp/x && echo made it
ush> fsck /dev/nope || echo that failed
```

Supported operators: `|`, `>`, `>>`, `&&`, `||` and `;`.

Pipelines are implemented by buffering each stage's output and feeding it
to the next as input. This is not concurrent — stage *n* runs to
completion before stage *n+1* starts — which is invisible for the data
sizes a shell handles but means an infinite producer like `yes | head`
will not terminate. Use `seq` instead.

### Default aliases

| Alias | Expands to |
| --- | --- |
| `ll` | `ls -l` |
| `la` | `ls -a` |
| `..` | `cd ..` |
| `md` | `mkdir` |

---

## Adding a command

Both shells use the same table-driven registration, so the recipe is
identical. To add one to UltraShell:

1. Write the handler. Keep locals small; use `shell_scratch()` for
   buffers over about 1 KB.

   ```c
   static int cmd_hello(struct shell *sh, int argc, char **argv)
   {
       shell_printf(sh, "hello, %s\n", argc > 1 ? argv[1] : "world");
       return 0;
   }
   ```

2. Add it to the command table in the relevant module:

   ```c
   { "hello", cmd_hello, "greet someone", "hello [name]" },
   ```

3. If you are adding a whole module, register it in
   `build_command_table()` in `ultrashell.c`. The table holds up to 80
   commands.

4. `help` and tab completion pick the command up automatically from the
   table — there is nothing else to update.

For QCSH the table lives in `qcsh.c`. Prefer QCSH for anything that
changes system state and UltraShell for anything a user runs day to day.

---

## Limitations

- No job control: no background execution, no `&`, no `fg`/`bg`.
- No shell functions or user-defined loops. `repeat` covers the common
  case; anything more belongs in a `source`d script.
- No globbing. Commands that accept multiple paths take them literally;
  `*` is not expanded.
- Pipelines are sequential rather than concurrent (see above).
- No here-documents and no `2>` stderr redirection.
