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

// Establish connection (client side of 3-way handshake)
int establish_connection(int sockfd, struct sockaddr_in *server_addr, uint32_t *client_seq, uint32_t *server_seq) {
    struct sham_packet packet;
    int len;
    
    // Step 1: Send SYN
    *client_seq = generate_initial_seq();
    packet.header.seq_num = htonl(*client_seq);
    packet.header.ack_num = htonl(0);
    packet.header.flags = htons(SYN_FLAG);
    packet.header.window_size = htons(MAX_BUFFER_SIZE);
    
    if (send_packet(sockfd, server_addr, &packet, sizeof(struct sham_header)) < 0) {
        return -1;
    }
    
    log_event("SND SYN SEQ=%u", *client_seq);
    
    // Step 2: Receive SYN-ACK
    if (receive_packet(sockfd, server_addr, &packet, &len) < 0) {
        return -1;
    }
    
    if (!(ntohs(packet.header.flags) & (SYN_FLAG | ACK_FLAG))) {
        fprintf(stderr, "Expected SYN-ACK packet\n");
        return -1;
    }
    
    *server_seq = ntohl(packet.header.seq_num);
    if (ntohl(packet.header.ack_num) != *client_seq + 1) {
        fprintf(stderr, "Invalid ACK number\n");
        return -1;
    }
    
    log_event("RCV SYN-ACK SEQ=%u ACK=%u", *server_seq, *client_seq + 1);
    
    // Step 3: Send ACK
    packet.header.seq_num = htonl(*client_seq + 1);
    packet.header.ack_num = htonl(*server_seq + 1);
    packet.header.flags = htons(ACK_FLAG);
    packet.header.window_size = htons(MAX_BUFFER_SIZE);
    
    if (send_packet(sockfd, server_addr, &packet, sizeof(struct sham_header)) < 0) {
        return -1;
    }
    
    log_event("SND ACK FOR SYN");
    (*client_seq)++;
    
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

// Send file with flow control and retransmission
int send_file(int sockfd, struct sockaddr_in *server_addr, const char *filename, float loss_rate) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open input file");
        return -1;
    }
    
    uint32_t seq_num = 1;
    uint16_t window_size = (uint16_t)MAX_BUFFER_SIZE;
    char buffer[MAX_PAYLOAD_SIZE];
    size_t bytes_read;
    
    struct sham_packet packet;
    int len;
    
    // Send file data
    while ((bytes_read = fread(buffer, 1, MAX_PAYLOAD_SIZE, file)) > 0) {
        // Create data packet
        packet.header.seq_num = htonl(seq_num);
        packet.header.ack_num = htonl(0);
        packet.header.flags = htons(0);
        packet.header.window_size = htons(window_size);
        
        memcpy(packet.data, buffer, bytes_read);
        
        // Simulate packet loss
        if (is_packet_lost(loss_rate)) {
            log_event("DROP DATA SEQ=%u", seq_num);
            continue;
        }
        
        // Send packet
        if (send_packet(sockfd, server_addr, &packet, sizeof(struct sham_header) + bytes_read) < 0) {
            fclose(file);
            return -1;
        }
        
        log_event("SND DATA SEQ=%u LEN=%zu", seq_num, bytes_read);
        
        // Wait for ACK
        if (receive_packet(sockfd, server_addr, &packet, &len) < 0) {
            fclose(file);
            return -1;
        }
        
        if (ntohs(packet.header.flags) & ACK_FLAG) {
            uint32_t ack_num = ntohl(packet.header.ack_num);
            log_event("RCV ACK=%u", ack_num);
            
            if (ack_num == seq_num + bytes_read) {
                seq_num += bytes_read;
            }
        }
    }
    
    fclose(file);
    
    // Send FIN to indicate end of file
    packet.header.seq_num = htonl(seq_num);
    packet.header.ack_num = htonl(0);
    packet.header.flags = htons(FIN_FLAG);
    packet.header.window_size = htons(window_size);
    
    if (send_packet(sockfd, server_addr, &packet, sizeof(struct sham_header)) < 0) {
        return -1;
    }
    
    log_event("SND FIN SEQ=%u", seq_num);
    
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
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <input_file> <output_file_name> [loss_rate]\n", argv[0]);
        fprintf(stderr, "   or: %s <server_ip> <server_port> --chat [loss_rate]\n", argv[0]);
        exit(1);
    }
    
    char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    int chat_mode_flag = 0;
    char *input_file = NULL;
    char *output_file = NULL;
    float loss_rate = 0.0;
    
    // Parse arguments
    if (strcmp(argv[3], "--chat") == 0) {
        chat_mode_flag = 1;
        if (argc > 4) {
            loss_rate = atof(argv[4]);
        }
    } else {
        input_file = argv[3];
        if (argc > 4) {
            output_file = argv[4];
        } else {
            output_file = "received_file";
        }
        if (argc > 5) {
            loss_rate = atof(argv[5]);
        }
    }
    
    // Initialize logging
    init_logging("client_log.txt");
    
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
    server_addr.sin_port = htons(server_port);
    
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid server IP address");
        close(sockfd);
        exit(1);
    }
    
    printf("Connecting to server %s:%d\n", server_ip, server_port);
    if (chat_mode_flag) {
        printf("Chat mode enabled\n");
    }
    if (loss_rate > 0.0) {
        printf("Packet loss simulation enabled: %.2f%%\n", loss_rate * 100);
    }
    
    // Establish connection
    uint32_t client_seq, server_seq;
    if (establish_connection(sockfd, &server_addr, &client_seq, &server_seq) < 0) {
        fprintf(stderr, "Failed to establish connection\n");
        close(sockfd);
        cleanup_logging();
        exit(1);
    }
    
    printf("Connection established with server\n");
    
    if (chat_mode_flag) {
        // Chat mode
        if (chat_mode(sockfd, &server_addr, 0) < 0) {
            fprintf(stderr, "Chat mode failed\n");
        }
    } else {
        // File transfer mode
        if (send_file(sockfd, &server_addr, input_file, loss_rate) < 0) {
            fprintf(stderr, "File transfer failed\n");
        } else {
            printf("File transfer completed\n");
        }
    }
    
    // Terminate connection
    terminate_connection(sockfd, &server_addr, &client_seq, 1);
    
    close(sockfd);
    cleanup_logging();
    return 0;
}
