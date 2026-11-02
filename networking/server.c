#include "sham.h"

// Global variables
FILE *log_file = NULL;
int logging_enabled = 0;

// Initialize logging system
void init_logging(const char *log_filename) {
    if (getenv("RUDP_LOG") && strcmp(getenv("RUDP_LOG"), "1") == 0) {
        logging_enabled = 1;
        log_file = fopen(log_filename, "w");
        if (!log_file) {
            perror("Failed to open log file");
            exit(1);
        }
    }
}

// Log event with microsecond precision timestamp
void log_event(const char *format, ...) {
    if (!logging_enabled || !log_file) return;
    
    char time_buffer[30];
    struct timeval tv;
    time_t curtime;
    
    gettimeofday(&tv, NULL);
    curtime = tv.tv_sec;
    
    // Format the time part
    strftime(time_buffer, 30, "%Y-%m-%d %H:%M:%S", localtime(&curtime));
    
    // Add microseconds and print to the log file
    fprintf(log_file, "[%s.%06d] [LOG] ", time_buffer, (int)tv.tv_usec);
    
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    
    fprintf(log_file, "\n");
    fflush(log_file);
}

void cleanup_logging() {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

// Generate initial sequence number
uint32_t generate_initial_seq() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec);
}

// Simulate packet loss
int is_packet_lost(float loss_rate) {
    if (loss_rate <= 0.0) return 0;
    return ((float)rand() / RAND_MAX) < loss_rate;
}

// Calculate MD5 checksum
void calculate_md5(const char *filename, char *md5_hash) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file for MD5 calculation");
        return;
    }
    
    MD5_CTX md5_ctx;
    MD5_Init(&md5_ctx);
    
    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        MD5_Update(&md5_ctx, buffer, bytes_read);
    }
    
    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &md5_ctx);
    
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(md5_hash + (i * 2), "%02x", digest[i]);
    }
    md5_hash[32] = '\0';
    
    fclose(file);
}

// Send packet with logging
int send_packet(int sockfd, struct sockaddr_in *dest_addr, struct sham_packet *packet, int len) {
    int result = sendto(sockfd, packet, len, 0, (struct sockaddr*)dest_addr, sizeof(*dest_addr));
    if (result < 0) {
        perror("sendto failed");
        return -1;
    }
    return result;
}

// Receive packet with logging
int receive_packet(int sockfd, struct sockaddr_in *src_addr, struct sham_packet *packet, int *len) {
    socklen_t addr_len = sizeof(*src_addr);
    int result = recvfrom(sockfd, packet, sizeof(*packet), 0, (struct sockaddr*)src_addr, &addr_len);
    if (result < 0) {
        perror("recvfrom failed");
        return -1;
    }
    *len = result;
    return result;
}

// Print packet information for debugging
void print_packet_info(const char *prefix, struct sham_packet *packet, int len) {
    printf("%s: SEQ=%u ACK=%u FLAGS=0x%x WIN=%u LEN=%d\n", 
           prefix, ntohl(packet->header.seq_num), ntohl(packet->header.ack_num), 
           ntohs(packet->header.flags), ntohs(packet->header.window_size), len);
}

// Accept connection (server side of 3-way handshake)
int accept_connection(int sockfd, struct sockaddr_in *client_addr, uint32_t *client_seq, uint32_t *server_seq) {
    struct sham_packet packet;
    int len;
    
    // Step 1: Receive SYN
    if (receive_packet(sockfd, client_addr, &packet, &len) < 0) {
        return -1;
    }
    
    if (!(ntohs(packet.header.flags) & SYN_FLAG)) {
        fprintf(stderr, "Expected SYN packet\n");
        return -1;
    }
    
    *client_seq = ntohl(packet.header.seq_num);
    log_event("RCV SYN SEQ=%u", *client_seq);
    
    // Step 2: Send SYN-ACK
    *server_seq = generate_initial_seq();
    packet.header.seq_num = htonl(*server_seq);
    packet.header.ack_num = htonl(*client_seq + 1);
    packet.header.flags = htons(SYN_FLAG | ACK_FLAG);
    packet.header.window_size = htons(MAX_BUFFER_SIZE);
    
    if (send_packet(sockfd, client_addr, &packet, sizeof(struct sham_header)) < 0) {
        return -1;
    }
    
    log_event("SND SYN-ACK SEQ=%u ACK=%u", *server_seq, *client_seq + 1);
    
    // Step 3: Receive ACK
    if (receive_packet(sockfd, client_addr, &packet, &len) < 0) {
        return -1;
    }
    
    if (!(ntohs(packet.header.flags) & ACK_FLAG)) {
        fprintf(stderr, "Expected ACK packet\n");
        return -1;
    }
    
    if (ntohl(packet.header.ack_num) != *server_seq + 1) {
        fprintf(stderr, "Invalid ACK number\n");
        return -1;
    }
    
    log_event("RCV ACK FOR SYN");
    return 0;
}

// Terminate connection (4-way handshake)
int terminate_connection(int sockfd, struct sockaddr_in *peer_addr, uint32_t *seq_num, int is_initiator) {
    struct sham_packet packet;
    int len;
    
    if (is_initiator) {
        // Send FIN
        packet.header.seq_num = htonl(*seq_num);
        packet.header.ack_num = htonl(0);
        packet.header.flags = htons(FIN_FLAG);
        packet.header.window_size = htons(MAX_BUFFER_SIZE);
        
        if (send_packet(sockfd, peer_addr, &packet, sizeof(struct sham_header)) < 0) {
            return -1;
        }
        
        log_event("SND FIN SEQ=%u", *seq_num);
        (*seq_num)++;
        
        // Receive ACK
        if (receive_packet(sockfd, peer_addr, &packet, &len) < 0) {
            return -1;
        }
        
        if (!(ntohs(packet.header.flags) & ACK_FLAG)) {
            fprintf(stderr, "Expected ACK for FIN\n");
            return -1;
        }
        
        log_event("RCV ACK FOR FIN");
        
        // Receive FIN
        if (receive_packet(sockfd, peer_addr, &packet, &len) < 0) {
            return -1;
        }
        
        if (!(ntohs(packet.header.flags) & FIN_FLAG)) {
            fprintf(stderr, "Expected FIN packet\n");
            return -1;
        }
        
        log_event("RCV FIN SEQ=%u", ntohl(packet.header.seq_num));
        
        // Send final ACK
        packet.header.seq_num = htonl(*seq_num);
        packet.header.ack_num = htonl(ntohl(packet.header.seq_num) + 1);
        packet.header.flags = htons(ACK_FLAG);
        packet.header.window_size = htons(MAX_BUFFER_SIZE);
        
        if (send_packet(sockfd, peer_addr, &packet, sizeof(struct sham_header)) < 0) {
            return -1;
        }
        
        log_event("SND ACK FOR FIN");
    } else {
        // Receive FIN
        if (receive_packet(sockfd, peer_addr, &packet, &len) < 0) {
            return -1;
        }
        
        if (!(ntohs(packet.header.flags) & FIN_FLAG)) {
            fprintf(stderr, "Expected FIN packet\n");
            return -1;
        }
        
        log_event("RCV FIN SEQ=%u", ntohl(packet.header.seq_num));
        
        // Send ACK
        packet.header.seq_num = htonl(*seq_num);
        packet.header.ack_num = htonl(ntohl(packet.header.seq_num) + 1);
        packet.header.flags = htons(ACK_FLAG);
        packet.header.window_size = htons(MAX_BUFFER_SIZE);
        
        if (send_packet(sockfd, peer_addr, &packet, sizeof(struct sham_header)) < 0) {
            return -1;
        }
        
        log_event("SND ACK FOR FIN");
        
        // Send FIN
        packet.header.seq_num = htonl(*seq_num);
        packet.header.ack_num = htonl(0);
        packet.header.flags = htons(FIN_FLAG);
        packet.header.window_size = htons(MAX_BUFFER_SIZE);
        
        if (send_packet(sockfd, peer_addr, &packet, sizeof(struct sham_header)) < 0) {
            return -1;
        }
        
        log_event("SND FIN SEQ=%u", *seq_num);
        (*seq_num)++;
        
        // Receive final ACK
        if (receive_packet(sockfd, peer_addr, &packet, &len) < 0) {
            return -1;
        }
        
        if (!(ntohs(packet.header.flags) & ACK_FLAG)) {
            fprintf(stderr, "Expected final ACK\n");
            return -1;
        }
        
        log_event("RCV ACK FOR FIN");
    }
    
    return 0;
}

// Receive file with flow control and retransmission
int receive_file(int sockfd, struct sockaddr_in *client_addr, const char *output_filename, float loss_rate) {
    FILE *file = fopen(output_filename, "wb");
    if (!file) {
        perror("Failed to open output file");
        return -1;
    }
    
    uint32_t expected_seq = 1;
    uint16_t window_size = (uint16_t)MAX_BUFFER_SIZE;
    
    struct sham_packet packet;
    int len;
    
    while (1) {
        if (receive_packet(sockfd, client_addr, &packet, &len) < 0) {
            break;
        }
        
        // Check if it's a FIN packet
        if (ntohs(packet.header.flags) & FIN_FLAG) {
            log_event("RCV FIN SEQ=%u", ntohl(packet.header.seq_num));
            break;
        }
        
        // Check if it's a data packet
        if (len > (int)sizeof(struct sham_header)) {
            uint32_t seq_num = ntohl(packet.header.seq_num);
            int data_len = len - sizeof(struct sham_header);
            
            // Simulate packet loss
            if (is_packet_lost(loss_rate)) {
                log_event("DROP DATA SEQ=%u", seq_num);
                continue;
            }
            
            log_event("RCV DATA SEQ=%u LEN=%d", seq_num, data_len);
            
            // Check if this is the expected sequence number
            if (seq_num == expected_seq) {
                // Write data to file
                fwrite(packet.data, 1, data_len, file);
                expected_seq += data_len;
                
                // Process any buffered packets
                // (In a full implementation, you'd check for buffered packets here)
            } else if (seq_num > expected_seq) {
                // Buffer out-of-order packet
                // (In a full implementation, you'd buffer this packet)
            }
            
            // Send ACK
            packet.header.seq_num = htonl(0);
            packet.header.ack_num = htonl(expected_seq);
            packet.header.flags = htons(ACK_FLAG);
            packet.header.window_size = htons(window_size);
            
            if (send_packet(sockfd, client_addr, &packet, sizeof(struct sham_header)) < 0) {
                break;
            }
            
            log_event("SND ACK=%u WIN=%u", expected_seq, window_size);
        }
    }
    
    fclose(file);
    return 0;
}

// Chat mode implementation
int chat_mode(int sockfd, struct sockaddr_in *peer_addr, int is_server) {
    fd_set read_fds;
    char input_buffer[1024];
    char message[1024];
    
    printf("Chat mode started. Type '/quit' to exit.\n");
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(0, &read_fds);  // stdin
        FD_SET(sockfd, &read_fds);
        
        int max_fd = (sockfd > 0) ? sockfd : 0;
        
        if (select(max_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select failed");
            return -1;
        }
        
        // Check for input from stdin
        if (FD_ISSET(0, &read_fds)) {
            if (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
                // Remove newline
                input_buffer[strcspn(input_buffer, "\n")] = 0;
                
                // Check for quit command
                if (strcmp(input_buffer, "/quit") == 0) {
                    printf("Initiating connection termination...\n");
                    break;
                }
                
                // Send message
                snprintf(message, sizeof(message), "[%s]: %s", 
                        is_server ? "Server" : "Client", input_buffer);
                
                struct sham_packet packet;
                packet.header.seq_num = htonl(1); // Simple sequence for chat
                packet.header.ack_num = htonl(0);
                packet.header.flags = htons(0);
                packet.header.window_size = htons(MAX_BUFFER_SIZE);
                
                int msg_len = strlen(message);
                memcpy(packet.data, message, msg_len);
                
                if (send_packet(sockfd, peer_addr, &packet, sizeof(struct sham_header) + msg_len) < 0) {
                    perror("Failed to send message");
                    return -1;
                }
            }
        }
        
        // Check for incoming data
        if (FD_ISSET(sockfd, &read_fds)) {
            struct sham_packet packet;
            int len = 0;
            struct sockaddr_in from_addr;
            socklen_t addr_len = sizeof(from_addr);
            
            if (recvfrom(sockfd, &packet, sizeof(packet), 0, 
                        (struct sockaddr*)&from_addr, &addr_len) > 0) {
                int data_len = len - sizeof(struct sham_header);
                packet.data[data_len] = '\0';
                printf("%s\n", packet.data);
            }
        }
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <port> [--chat] [loss_rate]\n", argv[0]);
        exit(1);
    }
    
    int port = atoi(argv[1]);
    int chat_mode_flag = 0;
    float loss_rate = 0.0;
    
    // Parse arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--chat") == 0) {
            chat_mode_flag = 1;
        } else {
            loss_rate = atof(argv[i]);
        }
    }
    
    // Initialize logging
    init_logging("server_log.txt");
    
    // Create socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(1);
    }
    
    // Set up server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    // Bind socket
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(1);
    }
    
    printf("Server listening on port %d\n", port);
    if (chat_mode_flag) {
        printf("Chat mode enabled\n");
    }
    if (loss_rate > 0.0) {
        printf("Packet loss simulation enabled: %.2f%%\n", loss_rate * 100);
    }
    
    // Wait for client connection
    struct sockaddr_in client_addr;
    uint32_t client_seq, server_seq;
    
    if (accept_connection(sockfd, &client_addr, &client_seq, &server_seq) < 0) {
        fprintf(stderr, "Failed to establish connection\n");
        close(sockfd);
        cleanup_logging();
        exit(1);
    }
    
    printf("Connection established with client\n");
    
    if (chat_mode_flag) {
        // Chat mode
        if (chat_mode(sockfd, &client_addr, 1) < 0) {
            fprintf(stderr, "Chat mode failed\n");
        }
    } else {
        // File transfer mode
        char output_filename[256];
        snprintf(output_filename, sizeof(output_filename), "received_file_%d", port);
        
        if (receive_file(sockfd, &client_addr, output_filename, loss_rate) < 0) {
            fprintf(stderr, "File transfer failed\n");
        } else {
            // Calculate and print MD5 checksum
            char md5_hash[33];
            calculate_md5(output_filename, md5_hash);
            printf("MD5: %s\n", md5_hash);
        }
    }
    
    // Terminate connection
    terminate_connection(sockfd, &client_addr, &server_seq, 0);
    
    close(sockfd);
    cleanup_logging();
    return 0;
}
