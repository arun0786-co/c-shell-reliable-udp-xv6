#ifndef SHAM_H
#define SHAM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <openssl/md5.h>
#include <fcntl.h>
#include <sys/select.h>
#include <stdarg.h>

// S.H.A.M. Header Structure
struct sham_header {
    uint32_t seq_num;      // Sequence Number
    uint32_t ack_num;      // Acknowledgment Number
    uint16_t flags;        // Control flags (SYN, ACK, FIN)
    uint16_t window_size;  // Flow control window size
} __attribute__((packed));

// Flag definitions
#define SYN_FLAG 0x1
#define ACK_FLAG 0x2
#define FIN_FLAG 0x4

// Protocol constants
#define MAX_PAYLOAD_SIZE 1024
#define WINDOW_SIZE 10
#define RTO_TIMEOUT 500000  // 500ms in microseconds
#define MAX_BUFFER_SIZE 8192
#define MAX_FILENAME_LEN 256

// Packet structure
struct sham_packet {
    struct sham_header header;
    char data[MAX_PAYLOAD_SIZE];
};

// Connection state
typedef enum {
    CLOSED,
    LISTEN,
    SYN_SENT,
    SYN_RCVD,
    ESTABLISHED,
    FIN_WAIT_1,
    FIN_WAIT_2,
    CLOSE_WAIT,
    LAST_ACK,
    TIME_WAIT
} connection_state_t;

// Packet buffer entry for retransmission
struct packet_buffer_entry {
    struct sham_packet packet;
    int len;
    struct timeval timestamp;
    int retransmitted;
};

// Global variables for logging
extern FILE *log_file;
extern int logging_enabled;

// Function prototypes
void init_logging(const char *log_filename);
void log_event(const char *format, ...);
void cleanup_logging();

// Utility functions
uint32_t generate_initial_seq();
int is_packet_lost(float loss_rate);
void calculate_md5(const char *filename, char *md5_hash);

// S.H.A.M. protocol functions
int send_packet(int sockfd, struct sockaddr_in *dest_addr, struct sham_packet *packet, int len);
int receive_packet(int sockfd, struct sockaddr_in *src_addr, struct sham_packet *packet, int *len);
void print_packet_info(const char *prefix, struct sham_packet *packet, int len);

// Connection management
int establish_connection(int sockfd, struct sockaddr_in *server_addr, uint32_t *client_seq, uint32_t *server_seq);
int accept_connection(int sockfd, struct sockaddr_in *client_addr, uint32_t *client_seq, uint32_t *server_seq);
int terminate_connection(int sockfd, struct sockaddr_in *peer_addr, uint32_t *seq_num, int is_initiator);

// Data transfer functions
int send_file(int sockfd, struct sockaddr_in *server_addr, const char *filename, float loss_rate);
int receive_file(int sockfd, struct sockaddr_in *client_addr, const char *output_filename, float loss_rate);
int chat_mode(int sockfd, struct sockaddr_in *peer_addr, int is_server);

// Flow control and retransmission
int send_data_with_flow_control(int sockfd, struct sockaddr_in *dest_addr, 
                               const char *data, int data_len, uint32_t *seq_num, 
                               uint16_t *window_size, float loss_rate);
int receive_data_with_flow_control(int sockfd, struct sockaddr_in *src_addr, 
                                  char *buffer, int *buffer_len, uint32_t *expected_seq, 
                                  uint16_t *window_size, float loss_rate);

#endif // SHAM_H
