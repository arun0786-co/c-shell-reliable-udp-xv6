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

A reliable byte stream over UDP: three-way handshake, byte sequence numbers,
cumulative ACKs, a sliding window with flow control, retransmission,
simulated packet loss, and a chat mode.

```
cd networking
make        # the server needs OpenSSL for MD5 (libssl-dev, or brew install openssl@3)
./server <port> [--chat] [loss_rate]
./client <server_ip> <server_port> <input_file> <output_file_name> [loss_rate]
./client <server_ip> <server_port> --chat [loss_rate]
make test   # 20 checks on 127.0.0.1
```

In file mode the server saves the file as `<output_file_name>` in its own
directory and, once the connection is closed, prints only
`MD5: <hash>` on stdout. The client prints a one-line summary on stderr. In
chat mode `/quit`, Ctrl-D or Ctrl-C starts the close, and the other side
closes too. `loss_rate` makes the receiving side drop incoming data packets
with that probability; in file mode only the server receives data, so the
client's rate has no effect there. `RUDP_LOG=1` writes the event log to
`server_log.txt` or `client_log.txt`.

### How it works

- `sham.c` is the transport, shared by both programs and both modes. The
  application writes bytes into a send buffer and calls `sham_poll` in a
  loop. `sham_poll` waits with `select` for a packet, a timer or the
  application's own descriptor (stdin in chat mode), then sends what the
  windows allow, retransmits, and passes in-order data to a callback that
  writes the file or prints the message.
- Sequence numbers count bytes from a random initial number. SYN and FIN use
  one number each, so the ACK of a FIN cannot be mistaken for the ACK of the
  last data byte. Sequence numbers are compared through their 32-bit
  difference, so comparisons still work when the numbers wrap around.
- At most 10 packets are in flight, and never more unacknowledged bytes than
  the receiver advertised. The receiver's window is its 16 KiB buffer minus
  the span of out-of-order data it holds (in-order data is handed over at
  once), so the window shrinks after a loss and opens again when the gap is
  filled.
- Each packet keeps its own send time. When the oldest unacknowledged packet
  is 500 ms old, that packet alone is retransmitted: the later ones have
  probably arrived, and a cumulative ACK cannot tell. Three duplicate ACKs
  retransmit it at once (fast retransmit). During recovery, an ACK that stops
  at the next gap retransmits that packet too.
- SYN, SYN-ACK and FIN are retransmitted as well, so a lost control packet
  cannot hang either side. The side whose FIN went first waits 1 s before
  exiting, so it can answer the other FIN again if its last ACK was lost
  (TCP's TIME_WAIT).
- The stream starts with the output file name and a NUL byte. The server
  keeps only the last path component of the name, writes to `<name>.part`,
  and renames it when the transfer is complete.
- Timers use `CLOCK_MONOTONIC`, and only the log timestamps use
  `gettimeofday`. In the Docker VM used for testing, the wall clock jumped
  4.6 s back and forth every few seconds. Timers on that clock would have
  fired late or early.

### Changes from the submitted version

The submitted version was stop-and-wait with no timers, so it only worked
when nothing was lost.

- A lost data packet or ACK left both programs blocked in `recvfrom` forever.
- The client applied the loss rate to its own sends and skipped the lost
  chunk for good: with 20% loss the server saved 159040 of 200000 bytes,
  printed a wrong MD5 and exited 0.
- The server did not buffer out-of-order packets and always advertised the
  same window, and the sender never read the window.
- Data started at sequence number 1 instead of ISN + 1, and the client sent
  two FINs from two different sequence spaces.
- In chat mode, `len` was read before it was set, so the code wrote to
  `data[-12]` and printed bytes past the end of the message. `/quit` on
  either side deadlocked both programs, and end of input made `select` spin.
- The output file name never reached the server, which always wrote
  `received_file_<port>`.
- The server printed status lines on stdout next to the MD5 line, and
  printed the MD5 before the connection was closed.
- MD5 used OpenSSL's deprecated `MD5_*` functions, and the Makefile's
  `brew --prefix openssl` now names an OpenSSL version that may not be
  installed, so the build failed on macOS.

### Testing

`make test` covers files of 0 bytes to 1 MiB at 0 to 50% loss, the log
format and the required log events, the 10-packet and advertised windows
(checked by replaying the client log), chat in both directions with each
side quitting and with 30% loss, and error cases. It passes on macOS and on
Linux, including builds with AddressSanitizer and UBSan.

Separately, transfers went through a UDP proxy that dropped packets of every
kind in both directions (handshake, ACKs and FINs included), duplicated some
and reordered some. All 10 runs at 15% drop and all 8 runs at 40% drop
arrived with the right MD5.

Time to send 1 MiB on Linux, three runs each, including the 1 s wait at the
end:

| Data packets lost | Timeout only | With fast retransmit |
|---|---|---|
| 5% | 17.7 to 25.7 s | 1.5 to 2.5 s |
| 10% | 30.8 to 34.8 s | 5.0 to 6.6 s |
| 20% | 59.5 to 70.1 s | 17.2 to 21.7 s |

## xv6

`xv6/xv6_modifications.patch` adds the `getreadcount` system call and FCFS
and CFS schedulers, chosen at build time with `make SCHEDULER=FCFS` or
`make SCHEDULER=CFS`. `xv6/report.md` describes the design.
