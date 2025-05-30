#include "consts.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "io.h"

/*
 * the following variables are only informational, 
 * not necessarily required for a valid implementation.
 * feel free to edit/remove.
 */

bool debug = true;
int state = 0;           // Current state for handshake
int our_send_window = 0; // Total number of bytes in our send buf
int their_receiving_window = MIN_WINDOW;   // Receiver window size
int our_max_receiving_window = MIN_WINDOW; // Our max receiving window
int our_recv_window = 0;                   // Bytes in our recv buf
int dup_acks = 0;        // Duplicate acknowledgements received
uint32_t ack = 0;        // Acknowledgement number
uint32_t seq = 0;        // Sequence number
uint32_t last_ack = 0;   // Last ACK number to keep track of duplicate ACKs
bool pure_ack = false;  // Require ACK to be sent out
packet* base_pkt = NULL; // Lowest outstanding packet to be sent out

buffer_node* recv_buf =
    NULL; // Linked list storing out of order received packets
buffer_node* send_buf =
    NULL; // Linked list storing packets that were sent but not acknowledged

ssize_t (*input)(uint8_t*, size_t); // Get data from layer
void (*output)(uint8_t*, size_t);   // Output data from layer

struct timeval start; // Last packet sent at this time
struct timeval now;   // Temp for current time

bool CLIENT_SYN_SENT = false;
bool SERVER_SYNACK_SENT = false;
bool CLIENT_ACK_SENT = false;


// Get data from standard input / make handshake packets
/* SERVER_AWAIT: Waiting for the clien
   CLIENT_START: CLIENT sends SYN
   SERVER_START: SERVER receives SYN, sends SYN-ACK
   CLIENT_AWAIT: CLIENT recieves SYN-ACK
*/
packet* get_data() {
    switch (state) {
        case SERVER_AWAIT: {
            // SERVER initializes as SERVER_AWAIT
            // SERVER STAYS ON RECEIVING
            return NULL;
            break;
        }
        case CLIENT_START: {
            // CLIENT initializes as CLIENT_START
            // CLIENT sends SYN
            if(!CLIENT_SYN_SENT) {
                packet* pkt = malloc(sizeof(packet) + MAX_PAYLOAD);

                pkt->seq = htons(seq);
                pkt->flags = SYN;
                pkt->length = htons(0);
                CLIENT_SYN_SENT = true;
                return pkt;
            }
            return NULL;
        }
        case SERVER_START: {
            // SERVER becomes SERVER_START after receiving SYN 
            if (!SERVER_SYNACK_SENT) {
                packet* pkt = malloc(sizeof(packet) + MAX_PAYLOAD);
                pkt->seq = htons(seq);
                pkt->ack = htons(ack);
                pkt->flags = SYN | ACK;
                pkt->length = htons(0);
                SERVER_SYNACK_SENT = true;
                return pkt;
            }
            return NULL;
        }
        case CLIENT_AWAIT: { 
            // CLIENT becomes CLIENT_AWAIT after receiving SYN-ACK
            if (!CLIENT_ACK_SENT) {
                packet* pkt = malloc(sizeof(packet) + MAX_PAYLOAD);
                pkt->seq = htons(seq);
                pkt->ack = htons(ack); 
                pkt->flags = ACK;
                pkt->length = htons(0);
                state = NORMAL;
                CLIENT_ACK_SENT = true;
                return pkt;
            }
            return NULL;
        }
        default: {
            if (our_send_window >= their_receiving_window) {
                return NULL;
            }
        
            uint8_t buffer[MAX_PAYLOAD] = {0};
            ssize_t bytes_read = input(buffer, MAX_PAYLOAD);
        
            if (bytes_read <= 0) {
                return NULL;
            }
        
            packet* pkt = malloc(sizeof(packet) + bytes_read);
            if (!pkt) return NULL;
        
            pkt->seq = htons(seq);
            pkt->ack = htons(ack);
            pkt->length = htons(bytes_read);
            pkt->win = htons(our_max_receiving_window);
            pkt->flags = 0;
            pkt->unused = 0;
            memcpy(pkt->payload, buffer, bytes_read);
        
            our_send_window += bytes_read;
            seq += bytes_read;
        
            buffer_node* node = malloc(sizeof(buffer_node));
            if (!node) {
                free(pkt);
                return NULL;
            }
            node->pkt = pkt;
            node->next = NULL;
        
            if (send_buf == NULL) {
                send_buf = node;
                base_pkt = pkt;
            } else {
                buffer_node* cur = send_buf;
                while (cur->next) cur = cur->next;
                cur->next = node;
            }
        
            gettimeofday(&start, NULL);
            return pkt;
        }
    }
}

// Process data received from socket
void recv_data(packet* pkt) {
    switch (state) {
        case SERVER_AWAIT: {
            // SERVER initializes as SERVER_AWAIT. ON RECEIVING VALID SYN:
            // SERVER gets SYN, Sends SYN-ACK
            if (pkt != NULL && (pkt->flags) & SYN) {
                 state = SERVER_START;
                ack = ntohs(pkt->seq) + 1;
            }   
            return;
        }
        case CLIENT_START: {
            // CLIENT initializes as CLIENT_START
            // CLIENT sends SYN
            // CURRENT: Waiting for SYN-ACK
            if (pkt != NULL && pkt->flags == (SYN | ACK) && ntohs(pkt->ack) == seq + 1) {
                // IF correct ACK is received, move on to next part of handshake
                 state = CLIENT_AWAIT;
                ack = ntohs(pkt->seq) + 1;
            }   

            return;
        }
        case SERVER_START: {
            // SERVER becomes SERVER_START after receiving SYN 
            if (pkt != NULL && pkt->flags == ACK && ntohs(pkt->ack) == seq + 1) {
                // If correct SYN  ACK  from part 3 of handshake is received, act to normal
                 state = NORMAL;
            }
            return;
        }
        case CLIENT_AWAIT: {
            // CLIENT AWAITS for SYN-ACK
            // Becomes NORMAL after sending ACK
            if (pkt != NULL && ntohs(pkt->flags) == SYN | ACK && ntohs(pkt->ack) == seq + 1) {
                            }   
            return;
        }
        default: {
            if (pkt == NULL) return;
            // Receiver; process incoming data
            int payload_len = ntohs(pkt->length);
            if (payload_len > 0) {
                output(pkt->payload, payload_len);
                ack = ntohs(pkt->seq) + payload_len;
                pure_ack = true;
            }
        
            // Sender; process incoming ACKs
            their_receiving_window = ntohs(pkt->win);
            uint16_t incoming_ack = ntohs(pkt->ack);
        
            if (incoming_ack > last_ack) {
                while (send_buf != NULL &&
                       ntohs(send_buf->pkt->seq) + ntohs(send_buf->pkt->length) <= incoming_ack) {
                    buffer_node* tmp = send_buf;
                    send_buf = send_buf->next;
                    free(tmp->pkt);
                    free(tmp);
                }
        
                base_pkt = (send_buf != NULL) ? send_buf->pkt : NULL;
                our_send_window = 0;
                last_ack = incoming_ack;
                dup_acks = 0;
                seq = incoming_ack;

            }
            else if (incoming_ack == last_ack) {
                dup_acks += 1;
            }
            return;
        }

    }
}

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int initial_state,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {

    // Set initial state (whether client or server)
    state = initial_state;

    // Set input and output function pointers
    input = input_p;
    output = output_p;

    // Set socket for nonblocking
    int flags = fcntl(sockfd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, flags);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int) {1}, sizeof(int));

    // Set initial sequence number
    uint32_t r;
    int rfd = open("/dev/urandom", 'r');
    read(rfd, &r, sizeof(uint32_t));
    close(rfd);
    srand(r);
    seq = (rand() % 10) * 100 + 100;

    // Setting timers
    gettimeofday(&now, NULL);
    gettimeofday(&start, NULL);

    // Create buffer for incoming data
    char buffer[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet* pkt = (packet*) &buffer;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Start listen loop
    while (true) {
        memset(buffer, 0, sizeof(packet) + MAX_PAYLOAD);
        // Get data from socket
        int bytes_recvd = recvfrom(sockfd, &buffer, sizeof(buffer), 0,
                                   (struct sockaddr*) addr, &addr_size);
        // If data, process it
        if (bytes_recvd > 0) {
            // print_diag(pkt, RECV);
            recv_data(pkt);
        }

        packet* tosend = get_data();
        // Data available to send
        if (tosend != NULL) {
            // print_diag(pkt, SEND);
            sendto(sockfd, tosend, sizeof(packet) + MAX_PAYLOAD, 0,
                (struct sockaddr*) addr, addr_size);
        }
        // Received a packet and must send an ACK
        else if (pure_ack) {
            packet* ack_pkt = malloc(sizeof(packet));
            if (!ack_pkt) return;
        
            ack_pkt->seq = htons(seq);
            ack_pkt->ack = htons(ack);
            ack_pkt->length = htons(0);
            ack_pkt->win = htons(our_max_receiving_window);
            ack_pkt->flags = htons(ACK);
            ack_pkt->unused = 0;
        
            sendto(sockfd, ack_pkt, sizeof(packet), 0,
                   (struct sockaddr*) addr, addr_size);
            print_diag(ack_pkt, SEND);
        
            free(ack_pkt);
            pure_ack = false;
        }        

        // Check if timer went off
        gettimeofday(&now, NULL);
        if (TV_DIFF(now, start) >= RTO && base_pkt != NULL) {
            fprintf(stderr, "Timeout: Resending base packet (seq %hu)\n", ntohs(base_pkt->seq));
            sendto(sockfd, base_pkt, sizeof(packet) + ntohs(base_pkt->length), 0,
                    (struct sockaddr*) addr, addr_size);
            gettimeofday(&start, NULL);
        }
        // Duplicate ACKS detected
        else if (dup_acks == DUP_ACKS && base_pkt != NULL){
            fprintf(stderr, "Triple duplicate ACKs: Fast retransmit (seq %hu)\n", ntohs(base_pkt->seq));
            sendto(sockfd, base_pkt, sizeof(packet) + ntohs(base_pkt->length), 0,
                   (struct sockaddr*) addr, addr_size);
            gettimeofday(&start, NULL);
            dup_acks = 0;
        }
        // No data to send, so restart timer
        else if (base_pkt == NULL) {
            gettimeofday(&start, NULL);
        }
    }
}