# C shell, reliable UDP and xv6 schedulers

Three systems programs in C, written for Mini Project 1 of Operating Systems
and Networks at IIIT Hyderabad (2025) and reworked later:

- `shell/`: a shell with pipes, redirection and job control
- `networking/`: S.H.A.M., a reliable byte stream over UDP
- `xv6/`: a `getreadcount` system call and extra schedulers for xv6, as a patch

## Shell

```
cd shell
make all        # builds shell.out with the course's strict POSIX flags
./shell.out
```

The directory the shell starts in is its home, shown as `~` in the prompt
`<user@host:path>`.

| Feature | Notes |
|---|---|
| Syntax check | prints `Invalid Syntax!` for input outside the grammar below |
| Programs | anything on `PATH`, or a path such as `/bin/echo` |
| `<`, `>`, `>>` | applied left to right, so the last one of each kind wins |
| `\|` | one process per command, all started before the shell waits |
| `;` and `&` | sequential and background groups |
| `hop [dir...]` | change directory; `~`, `.`, `..`, `-` and paths, applied in order |
| `reveal [-a] [-l] [dir]` | list a directory in ASCII order |
| `log`, `log purge`, `log execute <n>` | last 15 commands, kept in `~/.shell_history` |
| `activities` | running and stopped jobs, sorted by command |
| `ping <pid> <signal>` | send signal `n % 32` to a process |
| `fg [n]`, `bg [n]` | resume a job in the foreground or background |
| Ctrl-C, Ctrl-Z | interrupt or stop the foreground job, never the shell |
| Ctrl-D | print `logout`, kill all jobs and exit |

Grammar:

```
shell_cmd -> cmd_group ((& | ;) cmd_group)* &?
cmd_group -> atomic (| atomic)*
atomic    -> name (name | input | output)*
input     -> < name
output    -> > name | >> name
```

### How it works

- `parser.c` splits the line into tokens, then walks them with one function
  per grammar rule. The same pass checks the syntax and builds a tree:
  a line holds groups, a group holds piped commands, a command holds its
  arguments and redirections.
- `executor.c` runs that tree, and every line goes through it. A builtin that
  is alone in a foreground group runs inside the shell, so `hop` changes the
  shell's own directory. Anywhere else (in a pipe, or with `&`) it runs in a
  child, as in bash. A group becomes one job: one process per command, joined
  by pipes, all in one process group. Pipes are connected first and
  redirections applied after, so `ls > f | wc -l` writes the listing to `f`
  and `wc` counts 0.
- `jobs.c` does job control. The foreground job's process group is given the
  terminal with `tcsetpgrp`, so Ctrl-C and Ctrl-Z from the keyboard reach the
  job and not the shell. A background job that reads the terminal is stopped
  by the kernel with SIGTTIN. The shell's own SIGINT and SIGTSTP handlers
  (installed with `sigaction` and `SA_RESTART`) pass the signal on to the
  foreground job, which covers signals sent to the shell's pid. Status
  changes of background jobs are collected with
  `waitpid(-pgid, WNOHANG | WUNTRACED | WCONTINUED)` before each prompt and
  after each line of input.
- Errors go to stderr, so a pipe never carries them to the next command.

### Changes from the submitted version

The course autograder's 125 test cases were replayed against the shell on
Linux. The submitted version passed 85; this one passes 121. The four left
are described in the next section.

- Background jobs never printed `[n] pid`: the line had been commented out.
  That failed every background test and most job-control tests.
- Any command that was not found crashed the shell with
  `free(): invalid pointer`. The PATH list was built without a NULL at the
  end, and the loop that freed it walked past the end.
- A line was parsed and run by one of three different code paths, chosen by
  which characters it contained. Builtins existed only in one of them, so
  `hop dir ; pwd` printed `Command not found!`.
- Signal handlers were installed with `signal()`. Under the strict POSIX
  flags glibc gives that the old System V behaviour: the handler is reset
  after one signal and interrupted calls fail with EINTR, which showed up as
  `waitpid: Interrupted system call`. The handlers were also empty, and jobs
  had no process groups, so Ctrl-Z, `fg` and `bg` could not work.
- The parser accepted a trailing `;`, and accepted `cmd < > file` because a
  failed alternative did not rewind its position.
- `hop` stopped at the first bad directory. `reveal -` treated `-` as an
  empty flag list, `reveal ..` used the first `/` instead of the last,
  sorting ignored case, and `reveal` with no argument listed home instead of
  the current directory.
- `log execute` parsed the command and threw it away. Out-of-range indexes
  printed the wrong message, and the history filter skipped every command
  starting with the letters `log`.
- `/bin/echo` was not found, because the name was always joined to each
  PATH directory. With `cat < missing < file` only the last file was opened,
  so the command ran when it should not have.
- `ping 123 abc` used `atoi`, which turned `abc` into signal 0.
- Arguments lived in fixed 64-entry arrays filled without a bounds check:
  `echo` with 70 arguments piped into `wc` wrote past the end of the heap
  buffer. All arrays now grow as needed.

### Tests that disagree with the spec

- Tests 44 and 47 expect `hop test/folder1 bad test/folder2` to end in
  `test/folder2` from the starting directory, which means resolving every
  argument against the directory `hop` started in. The spec applies the
  arguments one after another (its example `hop rudy/osnmp1 .. -` goes
  into a folder, up one level and back), so `test/folder2` is looked up
  inside `test/folder1` and fails.
- Tests 64 and 66 expect `pwd` to be missing from the history. The spec says
  to store every command that does not run `log`, and the course tester's
  own transcript of those tests shows `pwd` in the `log` output.

## Networking: S.H.A.M.

A reliable byte stream over UDP with a three-way handshake, sequence numbers,
cumulative ACKs, retransmission on timeout, a sliding window, simulated
packet loss and a chat mode.

```
cd networking
make
./server <port> [--chat] [loss_rate]
./client <server_ip> <port> <input_file> <output_file> [loss_rate]
./client <server_ip> <port> --chat [loss_rate]
```

`RUDP_LOG=1` writes a timestamped event log to `server_log.txt` and
`client_log.txt`. The server prints the MD5 of a received file.

## xv6

`xv6/xv6_modifications.patch` adds the `getreadcount` system call and FCFS
and CFS schedulers, chosen at build time with `make SCHEDULER=FCFS` or
`make SCHEDULER=CFS`. `xv6/report.md` describes the design.
