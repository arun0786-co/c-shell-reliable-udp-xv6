# Makefile for S.H.A.M. Protocol Implementation
# Supports both Linux and macOS compilation

# Detect OS
UNAME_S := $(shell uname -s)

# Compiler flags
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -g

# OpenSSL library detection and linking
ifeq ($(UNAME_S),Darwin)
    # macOS with Homebrew
    OPENSSL_INCLUDE = -I$(shell brew --prefix openssl)/include
    OPENSSL_LIB = -L$(shell brew --prefix openssl)/lib
    LIBS = -lcrypto
else
    # Linux
    OPENSSL_INCLUDE = 
    OPENSSL_LIB = 
    LIBS = -lcrypto
endif

# Final compilation flags
FINAL_CFLAGS = $(CFLAGS) $(OPENSSL_INCLUDE)
FINAL_LDFLAGS = $(OPENSSL_LIB) $(LIBS)

# Target executables
TARGETS = server client

# Default target
all: $(TARGETS)

# Server executable
server: server.c sham.h
	$(CC) $(FINAL_CFLAGS) -o server server.c $(FINAL_LDFLAGS)

# Client executable
client: client.c sham.h
	$(CC) $(FINAL_CFLAGS) -o client client.c $(FINAL_LDFLAGS)


# Clean build artifacts
clean:
	rm -f $(TARGETS) *.o *.log server_log.txt client_log.txt received_file*

# Install dependencies (macOS)
install-macos:
	@echo "Installing OpenSSL via Homebrew..."
	brew install openssl

# Install dependencies (Linux)
install-linux:
	@echo "Installing OpenSSL development libraries..."
	sudo apt update && sudo apt install libssl-dev

# Test file transfer
test-file: all
	@echo "Testing file transfer mode..."
	@echo "Hello World from S.H.A.M. Protocol!" > test_input.txt
	@echo "Starting server in background..."
	./server 8080 &
	@sleep 2
	@echo "Starting client..."
	./client 127.0.0.1 8080 test_input.txt test_output.txt
	@echo "Comparing files..."
	@if diff test_input.txt received_file_8080; then \
		echo "✓ File transfer test passed!"; \
	else \
		echo "✗ File transfer test failed!"; \
	fi
	@pkill -f "./server 8080" || true
	@rm -f test_input.txt test_output.txt received_file_8080

# Test chat mode
test-chat: all
	@echo "Testing chat mode..."
	@echo "Starting server in background..."
	./server 8081 --chat &
	@sleep 2
	@echo "Starting client..."
	@echo "Starting chat client (type messages, /quit to exit)..."
	./client 127.0.0.1 8081 --chat
	@pkill -f "./server 8081" || true

# Test with packet loss simulation
test-loss: all
	@echo "Testing with packet loss simulation (10%)..."
	@echo "Hello World with packet loss!" > test_loss_input.txt
	@echo "Starting server in background..."
	./server 8082 0.1 &
	@sleep 2
	@echo "Starting client..."
	./client 127.0.0.1 8082 test_loss_input.txt test_loss_output.txt 0.1
	@echo "Comparing files..."
	@if diff test_loss_input.txt received_file_8082; then \
		echo "✓ Packet loss test passed!"; \
	else \
		echo "✗ Packet loss test failed!"; \
	fi
	@pkill -f "./server 8082" || true
	@rm -f test_loss_input.txt test_loss_output.txt received_file_8082

# Test with logging enabled
test-log: all
	@echo "Testing with verbose logging..."
	@echo "Test data for logging" > test_log_input.txt
	@echo "Starting server in background..."
	RUDP_LOG=1 ./server 8083 &
	@sleep 2
	@echo "Starting client..."
	RUDP_LOG=1 ./client 127.0.0.1 8083 test_log_input.txt test_log_output.txt
	@echo "Checking log files..."
	@if [ -f server_log.txt ]; then \
		echo "✓ Server log created"; \
		head -5 server_log.txt; \
	else \
		echo "✗ Server log not found"; \
	fi
	@if [ -f client_log.txt ]; then \
		echo "✓ Client log created"; \
		head -5 client_log.txt; \
	else \
		echo "✗ Client log not found"; \
	fi
	@pkill -f "./server 8083" || true
	@rm -f test_log_input.txt test_log_output.txt

# Run all tests
test: test-file test-chat test-loss test-log
	@echo "All tests completed!"

# Help target
help:
	@echo "Available targets:"
	@echo "  all          - Build server and client"
	@echo "  server       - Build server only"
	@echo "  client       - Build client only"
	@echo "  clean        - Remove build artifacts"
	@echo "  install-macos - Install OpenSSL on macOS"
	@echo "  install-linux - Install OpenSSL on Linux"
	@echo "  test-file    - Test file transfer mode"
	@echo "  test-chat    - Test chat mode"
	@echo "  test-loss    - Test with packet loss"
	@echo "  test-log     - Test with logging"
	@echo "  test         - Run all tests"
	@echo "  help         - Show this help"

# Phony targets
.PHONY: all clean install-macos install-linux test-file test-chat test-loss test-log test help
