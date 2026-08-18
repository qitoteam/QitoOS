# Shells

Qira OS ships two shells. They are not two skins over one command set —
they have different command tables, different prompts and different jobs.

| | QCSH | UltraShell |
| --- | --- | --- |
| Full name | QiraConfigShell | UltraShell |
| Purpose | system configuration, administration, diagnostics | general-purpose interactive work |
| Commands | 35 | 61 |
| Prompt | `qcsh>` | `user@qira:~$` |
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

## QCSH — QiraConfigShell

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
| `fsck` | check QiraFS integrity |
| `netinfo` | interfaces, addresses, routes and statistics |
| `ping <host>` | ICMP echo |
| `users` | accounts and their capabilities |
| `perms <path>` | permission bits for a path |
| `reboot`, `poweroff` | ACPI shutdown or reset |

### Example

```
qcsh> sysinfo
Qira OS 0.3.0 (Aurora)
  processor    : QEMU Virtual CPU version 2.5+ (1 core)
  memory       : 511M total, 8M used, 503M free
  uptime       : 0d 00:00:12
  display      : 1024x768x32
  tasks        : 4 running

qcsh> diag
  [ OK ]  Physical memory allocator    511M managed
  [ OK ]  Kernel heap                  8M allocated
  [ OK ]  Virtual memory               page tables consistent
  [ OK ]  Process scheduler            running
  [ OK ]  Root filesystem              QiraFS, 42 entries
  [ OK ]  Interrupt controller         PIC remapped, 16 lines
  [ OK ]  System timer                 100 Hz
  [ OK ]  Framebuffer                  1024x768x32
8 checks, 8 passed

qcsh> config set terminal.font qira-mono-bold
qcsh> config save
configuration written to /etc/qira.conf
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

### Networking and formats

| Command | Description |
| --- | --- |
| `fetch <url>` | HTTP GET |
| `lookup <host>` | DNS resolution |
| `git clone\|ls-remote <url>` | git smart-HTTP client |
| `lqx info\|exports [path]` | inspect an LQX executable, list kernel exports |
| `qac list\|info [path]` | inspect QAC icons |
| `fonts [set <ui\|terminal> <id>]` | list or choose typefaces |

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
