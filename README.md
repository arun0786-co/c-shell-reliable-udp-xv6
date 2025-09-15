Mini Project 1 – Shell, Networking (S.H.A.M), and xv6

Project layout

- shell/
  - src/, include/, Makefile
  - Builds to shell/shell.out
- networking/
  - client.c, server.c, sham.h, Makefile, README.md
- xv6/
  - xv6_modifications.patch (ONLY files changed), report.md, readcount.c
- tester/
  - test.py, requirements.txt (local basic shell tests)

Build and test – Shell

# C Shell Implementation

A complete POSIX-compliant shell implementation in C, supporting advanced features like job control, signal handling, command history, and complex command parsing.

## 📋 Project Overview

This project implements a fully functional shell with the following major components:
- **Part A**: Basic shell functionality (prompt, input parsing, grammar validation)
- **Part B**: Built-in commands (hop, reveal, log)
- **Part C**: File redirection and pipes
- **Part D**: Sequential and background execution
- **Part E**: Advanced job control and signal handling

## 🔧 Build Instructions

```bash
make clean    # Clean build artifacts
make all      # Compile the shell
```

The makefile uses strict POSIX compliance flags:
```makefile
CFLAGS = -std=c99 -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -Wall -Wextra -Werror -Wno-unused-parameter -fno-asm
```

## 📚 Implementation Details

### Part A: Shell Input (65 points)

#### A.1: Shell Prompt (10 points)
**Implementation**: `display_prompt()` in `main.c`

**Key Features**:
- Dynamic username and hostname detection using environment variables
- Home directory replacement with `~` symbol
- Proper memory management for path display

**Challenges & Solutions**:
- **Challenge**: Handling cases where environment variables might be NULL
- **Solution**: Implemented fallback values and proper NULL checking
- **Challenge**: Memory management for path manipulation
- **Solution**: Careful allocation and deallocation of display paths

```c
void display_prompt(char* home_dir) {
    char* username = my_getenv("USER", environ);
    char* hostname = my_getenv("HOSTNAME", environ);
    char* current_dir = getcwd(NULL, 0);
    
    // Handle NULL environment variables
    if (!username) username = "arun";
    if (!hostname) hostname = "iiith";
    
    // Replace home directory with ~
    // ... path manipulation logic
}
```

#### A.2: User Input 
**Implementation**: `getline()` in main shell loop

**Key Features**:
- Continuous input loop with proper EOF handling
- Input sanitization and newline removal

#### A.3: Input Parsing 
**Implementation**: `parse_shell_cmd()` and related functions in `parser.c`

**Grammar Implementation**:
```
shell_cmd  ->  cmd_group ((& | ;) cmd_group)* &?
cmd_group ->  atomic (\| atomic)*
atomic -> name (name | input | output)*
input -> < name | <name
output -> > name | >name | >> name | >>name
name -> r"[^|&><;\s]+"
```

**Key Functions**:
- `parse_shell_cmd()`: Main grammar validator
- `parse_cmd_group()`: Handles pipe-separated commands
- `parse_atomic()`: Parses individual commands with redirections
- `parse_input_redirection()`: Handles `<` redirection
- `parse_output_redirection()`: Handles `>` and `>>` redirection

**Challenges & Solutions**:
- **Challenge**: Complex grammar validation with proper error handling
- **Solution**: Recursive descent parsing with comprehensive error checking
- **Challenge**: Whitespace handling between tokens
- **Solution**: Implemented `is_whitespace()` function and proper token skipping

### Part B: Shell Intrinsics

#### B.1: hop Command 
**Implementation**: `command_hop()` in `builtins.c`

**Key Features**:
- Support for `~`, `.`, `..`, `-`, and directory names
- Previous directory tracking
- Sequential argument processing

**Challenges & Solutions**:
- **Challenge**: Handling `..` navigation without going beyond root
- **Solution**: String manipulation to find parent directory safely
- **Challenge**: Previous directory tracking across multiple hops
- **Solution**: Maintained `prev_directory` pointer in main loop

#### B.2: reveal Command 
**Implementation**: `command_reveal()` in `builtins.c`

**Key Features**:
- `-a` flag for hidden files
- `-l` flag for line-by-line output
- Lexicographic sorting
- Directory navigation support

**Challenges & Solutions**:
- **Challenge**: Lexicographic sorting implementation
- **Solution**: Bubble sort algorithm with `my_strcmp()` function
- **Challenge**: Flag parsing and validation
- **Solution**: Character-by-character flag parsing with proper validation

#### B.3: log Command 
**Implementation**: `command_log()` in `builtins.c` with history management in `job_control.c`

**Key Features**:
- Persistent history across shell sessions
- `purge` and `execute` subcommands
- Duplicate command filtering
- File-based storage

**Challenges & Solutions**:
- **Challenge**: Persistent storage across sessions
- **Solution**: File-based history with `.shell_history` file
- **Challenge**: Memory management for history storage
- **Solution**: Circular buffer implementation with proper cleanup

### Part C: File Redirection and Pipes 

#### C.1: Command Execution
**Implementation**: `executor()` and `child_process()` in `excecutor.c`

**Key Features**:
- PATH environment variable searching
- Current directory execution fallback
- Proper error handling for missing commands

#### C.2: Input Redirection 
**Implementation**: `execute_single_command()` in `excecutor.c`

**Key Features**:
- `open()` with `O_RDONLY` flag
- `dup2()` for stdin redirection
- File descriptor leak prevention

#### C.3: Output Redirection 
**Implementation**: `execute_single_command()` in `excecutor.c`

**Key Features**:
- `>` for truncation, `>>` for appending
- `O_TRUNC` and `O_APPEND` flags
- Proper file creation with permissions

#### C.4: Command Piping 
**Implementation**: `execute_pipeline()` in `excecutor.c`

**Key Features**:
- `pipe()` system call for inter-process communication
- Multiple process forking
- Proper file descriptor management
- Pipeline completion waiting

**Challenges & Solutions**:
- **Challenge**: Complex file descriptor management
- **Solution**: Careful `dup2()` usage and proper cleanup
- **Challenge**: Pipeline error handling
- **Solution**: Continue execution even if individual commands fail

### Part D: Sequential and Background Execution 

#### D.1: Sequential Execution 
**Implementation**: `parse_command_sequence()` and `execute_command_sequence()` in `parser.c` and `excecutor.c`

**Key Features**:
- Semicolon (`;`) command separation
- Sequential execution with proper waiting
- Error handling for individual commands

**Challenges & Solutions**:
- **Challenge**: Parsing mixed operators (`;`, `&`, `|`)
- **Solution**: Hierarchical parsing with operator precedence
- **Challenge**: Memory management for command sequences
- **Solution**: Proper cleanup functions for all allocated structures

#### D.2: Background Execution 
**Implementation**: Job control system in `job_control.c`

**Key Features**:
- Background process tracking
- Job number assignment
- Process status monitoring
- Automatic cleanup of completed processes

**Challenges & Solutions**:
- **Challenge**: Background process tracking
- **Solution**: Global job array with PID tracking
- **Challenge**: Process completion detection
- **Solution**: `waitpid()` with `WNOHANG` flag for non-blocking checks

### Part E: Exotic Shell Intrinsics 

#### E.1: activities Command 
**Implementation**: `command_activities()` in `builtins.c`

**Key Features**:
- Process listing with status
- Lexicographic sorting by command name
- Automatic cleanup of terminated processes

#### E.2: ping Command 
**Implementation**: `command_ping()` in `builtins.c`

**Key Features**:
- Signal number modulo 32
- Process existence validation
- Proper error handling

#### E.3: Signal Handling 
**Implementation**: Signal handlers in `job_control.c`

**Key Features**:
- `SIGINT` (Ctrl-C) handling
- `SIGTSTP` (Ctrl-Z) handling
- `SIGCHLD` management
- Process group management

**Challenges & Solutions**:
- **Challenge**: Signal handler implementation
- **Solution**: Proper signal registration and handler functions
- **Challenge**: Process group management
- **Solution**: `setpgid()` and `tcsetpgrp()` for proper foreground/background handling

#### E.4: fg and bg Commands 
**Implementation**: `command_fg()` and `command_bg()` in `builtins.c`

**Key Features**:
- Job number resolution
- Process resumption with `SIGCONT`
- Proper job state management

## 🚧 Major Challenges and Solutions

### 1. Memory Management
**Challenge**: Double-free errors and memory leaks
**Solution**: 
- Careful tracking of allocated memory
- Proper cleanup functions for all data structures
- Avoiding freeing system-managed environment variables

### 2. Complex Parsing
**Challenge**: Implementing the complex grammar with proper error handling
**Solution**:
- Recursive descent parsing
- Comprehensive error checking at each level
- Proper tokenization with whitespace handling

### 3. Job Control
**Challenge**: Managing background processes and job states
**Solution**:
- Global job tracking array
- Proper process group management
- Non-blocking process status checking

### 4. Signal Handling
**Challenge**: Proper signal handling without breaking shell functionality
**Solution**:
- Signal handler registration
- Process group management
- Proper signal forwarding to child processes

### 5. File Descriptor Management
**Challenge**: Complex file descriptor handling in pipelines
**Solution**:
- Careful `dup2()` usage
- Proper cleanup of all file descriptors
- Error handling for failed redirections

## 🔍 Key Design Decisions

### 1. Modular Architecture
- Separated concerns into different source files
- Clear interfaces between modules
- Reusable utility functions

### 2. Memory Safety
- Consistent memory management patterns
- Proper cleanup functions
- Avoiding double-free errors

### 3. Error Handling
- Comprehensive error checking
- Graceful degradation
- User-friendly error messages

### 4. POSIX Compliance
- Strict adherence to POSIX standards
- Proper use of system calls
- Cross-platform compatibility



Build and demo – Networking
    # S.H.A.M. Protocol Implementation

A reliable UDP protocol implementation that simulates TCP functionality, including connection management, data sequencing, retransmission, flow control, and both file transfer and real-time chat modes.

## Overview

This project implements a custom protocol called S.H.A.M. (Sequence, Handshake, Acknowledgment, Management) that provides reliable communication over UDP. The implementation includes:

- **S.H.A.M. Packet Structure**: Custom header with sequence numbers, acknowledgments, flags, and window size
- **Connection Management**: 3-way handshake for establishment, 4-way handshake for termination
- **Data Sequencing**: 1024-byte data chunks with sequence numbers
- **Retransmission**: Timeout-based retransmission for lost packets
- **Flow Control**: Sliding window mechanism
- **Cumulative ACKs**: Efficient acknowledgment system
- **Packet Loss Simulation**: Configurable packet loss for testing
- **Verbose Logging**: Detailed logging with microsecond timestamps
- **Dual Modes**: File transfer and real-time chat

### 1. S.H.A.M. Packet Structure 
- **Sequence Number (seq_num)**: 32-bit field indicating byte-stream position
- **Acknowledgment Number (ack_num)**: 32-bit field for cumulative ACKs
- **Flags (flags)**: 16-bit field supporting SYN, ACK, FIN flags
- **Window Size (window_size)**: 16-bit field for flow control

### 2. Connection Management 
- **3-Way Handshake**: SYN → SYN-ACK → ACK for connection establishment
- **4-Way Handshake**: FIN → ACK → FIN → ACK for connection termination
- **State Management**: Proper connection state tracking

### 3. Data Sequencing and Retransmission 
- **Data Segmentation**: Files split into 1024-byte chunks
- **Sliding Window**: Configurable window size (default: 10 packets)
- **Cumulative ACKs**: Efficient acknowledgment of received data
- **Retransmission Timeout (RTO)**: 500ms timeout for packet retransmission
- **Selective Retransmission**: Only retransmit lost packets

### 4. Flow Control 
- **Window Size Management**: Dynamic window size based on receiver capacity
- **Buffer Management**: Proper handling of receiver buffer space
- **Congestion Control**: Basic flow control mechanisms

### 5. Implementation and Testing 
- **Command-Line Interface**: Proper argument parsing for both modes
- **File Transfer Mode**: Complete file transfer with MD5 verification
- **Chat Mode**: Real-time chat using select() for concurrent I/O
- **Standardized Output**: MD5 checksum output in required format

### 6. Packet Loss Simulation 
- **Configurable Loss Rate**: 0.0 to 1.0 probability
- **Testing Support**: Enables testing of retransmission logic
- **Logging**: DROP events logged when packets are lost

### 7. Verbose Logging 
- **Environment Variable**: RUDP_LOG=1 enables logging
- **Microsecond Timestamps**: High-precision timing using gettimeofday()
- **Comprehensive Events**: All protocol events logged with timestamps
- **Separate Log Files**: server_log.txt and client_log.txt


```bash
brew install openssl
```

```bash
make clean && make
```

**Server:**
```bash
./server <port> [--chat] [loss_rate]
```

**Client - File Transfer Mode:**
```bash
./client <server_ip> <server_port> <input_file> <output_file_name> [loss_rate]
```

**Client - Chat Mode:**
```bash
./client <server_ip> <server_port> --chat [loss_rate]
```

**Basic File Transfer:**
```bash
# Terminal 1
./server 8080

# Terminal 2
./client 127.0.0.1 8080 input.txt output.txt
```

**Chat Mode:**
```bash
# Terminal 1
./server 8080 --chat

# Terminal 2
./client 127.0.0.1 8080 --chat
```

**With Packet Loss Simulation:**
```bash
# Terminal 1
./server 8080 0.1  # 10% packet loss

# Terminal 2
./client 127.0.0.1 8080 input.txt output.txt 0.1
```

**With Verbose Logging:**
```bash
# Terminal 1
RUDP_LOG=1 ./server 8080

# Terminal 2
RUDP_LOG=1 ./client 127.0.0.1 8080 input.txt output.txt
```

## Protocol Details

### S.H.A.M. Header Format

```c
struct sham_header {
    uint32_t seq_num;      // Sequence Number
    uint32_t ack_num;      // Acknowledgment Number
    uint16_t flags;        // Control flags (SYN, ACK, FIN)
    uint16_t window_size;  // Flow control window size
} __attribute__((packed));
```

### Flag Definitions

- `SYN_FLAG (0x1)`: Synchronize - connection initiation
- `ACK_FLAG (0x2)`: Acknowledge - acknowledgment field significant
- `FIN_FLAG (0x4)`: Finish - connection termination

### Connection States

- `CLOSED`: No connection
- `LISTEN`: Server waiting for connection
- `SYN_SENT`: Client sent SYN, waiting for SYN-ACK
- `SYN_RCVD`: Server received SYN, sent SYN-ACK
- `ESTABLISHED`: Connection established
- `FIN_WAIT_1`: First FIN sent
- `FIN_WAIT_2`: ACK for first FIN received
- `CLOSE_WAIT`: FIN received, waiting to close
- `LAST_ACK`: FIN sent, waiting for ACK
- `TIME_WAIT`: Final ACK received

## Logging Format

When `RUDP_LOG=1` is set, the following events are logged:

```
[YYYY-MM-DD HH:MM:SS.microseconds] [LOG] <event_description>
```

**Example Log Entries:**
```
[2025-09-13 13:17:07.366927] [LOG] RCV SYN SEQ=4196707724
[2025-09-13 13:17:07.367512] [LOG] SND SYN-ACK SEQ=4196708388 ACK=4196707725
[2025-09-13 13:17:07.367627] [LOG] RCV ACK FOR SYN
[2025-09-13 13:17:07.367738] [LOG] RCV DATA SEQ=1 LEN=22
[2025-09-13 13:17:07.367769] [LOG] SND ACK=23 WIN=8192
```

## Implementation Notes

### Retransmission Logic
- Each sent packet is buffered with a timestamp
- Timeout checking occurs during data transmission
- Only unacknowledged packets are retransmitted
- RTO is set to 500ms (configurable)

### Flow Control
- Receiver advertises available buffer space in window_size field
- Sender respects advertised window size
- Window size is updated based on ACK reception

### Error Handling
- Graceful handling of packet loss
- Proper connection termination
- Comprehensive error logging

## Performance Characteristics

- **Throughput**: Limited by window size and RTO settings
- **Latency**: RTO-based retransmission adds latency for lost packets
- **Reliability**: High reliability through retransmission and ACKs
- **Efficiency**: Cumulative ACKs reduce network overhead

Build and run – xv6

1) Prereqs: RISC-V toolchain and qemu installed

2) Build kernel (default RR)
   make -C xv6

3) Alternate schedulers
   make -C xv6 clean && make -C xv6 SCHEDULER=FCFS
   make -C xv6 clean && make -C xv6 SCHEDULER=CFS

4) Run
   make -C xv6 qemu

5) System call demo inside xv6
   readcount

Notes for submission

- xv6/xv6_modifications.patch contains only files we modified or added (not the whole xv6 tree):
  - Makefile, kernel/{{proc.c,proc.h,syscall.c,sysfile.c,syscall.h,trap.c}}, user/{{user.h,usys.pl,readcount.c,schedulertest.c}}, report.md
- Networking server prints MD5 on successful file reception as required.
- Shell builds with make all in shell/ and passes the included tester.


