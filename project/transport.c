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
/*
 * the following variables are only informational, 
 * not necessarily required for a valid implementation.
 * feel free to edit/remove.
 */

bool debug = true;
bool extra_debug = false;
bool test_cases = false;
int state = 0;           // Current state for handshake
int our_send_window = 0; // Total number of bytes in our send buf
int their_receiving_window = MIN_WINDOW;   // Receiver window size
int our_max_receiving_window = MIN_WINDOW; // Our max receiving window
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

/* RECEIVER WINDOW HANDLING */
buffer_node* rec_head = NULL;
buffer_node* rec_tail = NULL;

packet* process_pkt() {
    /* Process Receiver Head
       DO NOT process if pkt->seq + 1 != ACK*/
    if (rec_head == NULL) return NULL;
    uint16_t node_seq = ntohs(rec_head->pkt->seq); 
    uint16_t node_len = ntohs(rec_head->pkt->length);

    // Case : Not in order
    if(node_seq != ack) {
        return NULL;
    }

    buffer_node* old_head = rec_head;
    packet* pkt = old_head->pkt;

    rec_head = old_head->next;
    if (rec_head == NULL)
        rec_tail = NULL; 
    free(old_head);

    ack = node_seq + 1;
    pure_ack = true;
    our_max_receiving_window += 500;
    return pkt;
}

void queue(packet* pkt) {
    /* HANDLE RECEIVING DATA PACKETS
       Responsible for sending out ACKs
       Does not process ACKs
       ONCE added to queue, send ACK 
       SEQ & Length here refers to incoming packet's
    */
    uint16_t pkt_seq = ntohs(pkt->seq);
    uint16_t pkt_len = ntohs(pkt->length);
    uint16_t pkt_end = pkt_seq + 1;

    if (pkt_end <= ack) {
        free(pkt);
        pure_ack = true;
        return;
    }

    // Create Buffer Node
    buffer_node* node = malloc(sizeof(buffer_node));
    node->pkt = pkt;
    node->next = NULL;

    // Case 1: Queue is empty 
    if (rec_head == NULL) {
        rec_head = node;
        rec_tail = node;
    }
    else {
        buffer_node *prev = NULL;
        buffer_node *cur = rec_head;
        while (cur != NULL && ntohs(cur->pkt->seq) < pkt_seq) {
            prev = cur;
            cur = cur->next;
        }

        if (cur != NULL && ntohs(cur->pkt->seq) == pkt_seq) {
            // CASE : Duplicate Packet & Out of Order
            free(pkt);
            free(node);
            pure_ack = true;
            return;
        }

        if (prev == NULL) {
            // At Head
            node->next = rec_head;
            rec_head = node;
            pure_ack = true;
        } else {
            // At Middle or Tail 
            prev->next = node;
            node->next = cur;
            if (cur == NULL) 
                rec_tail = node; 
            pure_ack = true;
        }   
    }
}

// Get data from standard input / make handshake packets
/* SERVER_AWAIT: Waiting for the clien
   CLIENT_START: CLIENT sends SYN
   SERVER_START: SERVER receives SYN, sends SYN-ACK
   CLIENT_AWAIT: CLIENT recieves SYN-ACK
*/

bool is_data_packet(packet* pkt) {
    return (ntohs(pkt->length) > 0) || (ntohs(pkt->flags) == 0);
}

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
                packet* pkt = malloc(sizeof(packet));

                pkt->seq = htons(seq);
                pkt->flags = SYN;
                pkt->length = htons(0);
                pkt->win = htons(our_max_receiving_window);
                CLIENT_SYN_SENT = true;
                return pkt;
            }
            return NULL;
        }
        case SERVER_START: {
            // SERVER becomes SERVER_START after receiving SYN 
            if (!SERVER_SYNACK_SENT) {
                packet* pkt = malloc(sizeof(packet));
                pkt->seq = htons(seq);
                pkt->ack = htons(ack);
                pkt->flags = SYN | ACK;
                pkt->length = htons(0);
                pkt->win = htons(our_max_receiving_window);
                SERVER_SYNACK_SENT = true;
                return pkt;
            }
            return NULL;
        }
        case CLIENT_AWAIT: {
            if (!CLIENT_ACK_SENT) {
                uint8_t buffer[MAX_PAYLOAD] = {0};
                ssize_t bytes_read = input(buffer, MAX_PAYLOAD);
        
                packet* pkt = NULL;
                if (bytes_read > 0) {
                    // Data + ACK (adjust memory size!)
                    pkt = malloc(sizeof(packet) + bytes_read);
                    if (!pkt) return NULL;
        
                    pkt->seq = htons(seq);
                    pkt->ack = htons(ack);
                    pkt->length = htons(bytes_read);
                    pkt->win = htons(our_max_receiving_window);
                    pkt->flags = ACK;
                    pkt->unused = 0;
                    memcpy(pkt->payload, buffer, bytes_read);
        
                    // Add to send_buf
                    buffer_node* node = malloc(sizeof(buffer_node));
                    node->pkt = pkt;
                    node->next = NULL;
                    if (!send_buf) {
                        send_buf = node;
                        base_pkt = pkt;
                    } else {
                        buffer_node* cur = send_buf;
                        while (cur->next) cur = cur->next;
                        cur->next = node;
                    }
        
                    our_send_window += bytes_read;
                    seq++;
                } else {
                    // Pure ACK only
                    pkt = malloc(sizeof(packet));
                    if (!pkt) return NULL;
        
                    pkt->seq = htons(0);
                    pkt->ack = htons(ack);
                    pkt->length = htons(0);
                    pkt->win = htons(our_max_receiving_window);
                    pkt->flags = ACK;
                    pkt->unused = 0;
                    seq++;
                }
        
                CLIENT_ACK_SENT = true;
                state = NORMAL;
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
            if (extra_debug) {
                fprintf(stderr, "[GET_DATA] Read %zd bytes from input\n", bytes_read);            
            }
            packet* pkt = malloc(sizeof(packet) + bytes_read);
            if (!pkt) return NULL;
        
            pkt->seq = htons(seq);
            pkt->ack = htons(ack);
            pkt->length = htons(bytes_read);
            if(extra_debug){
                fprintf(stderr, "[GET_DATA] Sending payload of length %zd, htons: %04x\n", bytes_read, htons(bytes_read));
            }

            pkt->win = htons(our_max_receiving_window);
            pkt->flags = ACK;
            pkt->unused = 0;
            memcpy(pkt->payload, buffer, bytes_read);
        
            our_send_window += bytes_read;
            seq += 1;
        
            if (bytes_read > 0) {
                buffer_node* node = malloc(sizeof(buffer_node));
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
            }
        
            gettimeofday(&start, NULL);
            return pkt;
        }
    }
}

// Process data received from socket
void recv_data(packet* pkt) {
    if(pkt == NULL) return;
    // uint16_t flags = ntohs(pkt->flags);
    uint16_t flags = pkt->flags;
    switch (state) {
        case SERVER_AWAIT: {
            // SERVER initializes as SERVER_AWAIT. ON RECEIVING VALID SYN:
            // SERVER gets SYN, Sends SYN-ACK
            if  (flags & SYN) {
                state = SERVER_START;
                ack = ntohs(pkt->seq) + 1;
            }  
            return; 
        }
        case CLIENT_START: {
            // CLIENT initializes as CLIENT_START
            // CLIENT sends SYN
            // CURRENT: Waiting for SYN-ACK
            if ((flags & SYN) && (flags & ACK) && ntohs(pkt->ack) == seq + 1) {
                // IF correct ACK is received, move on to next part of handshake
                state = CLIENT_AWAIT;
                ack = ntohs(pkt->seq) + 1;
            }
            return;   
        }
        case SERVER_START: {
            // SERVER becomes SERVER_START after receiving SYN 
            if ((flags & ACK) && ntohs(pkt->ack) == seq + 1) {
                // If correct SYN  ACK  from part 3 of handshake is received, act to normal
                 state = NORMAL;

                 if (ntohs(pkt->length) > 0) {
                    packet* copy = malloc(sizeof(packet) + ntohs(pkt->length));
                    if (copy) {
                        memcpy(copy, pkt, sizeof(packet) + ntohs(pkt->length));
                        queue(copy);
                    }
                }
                seq++;
                if (extra_debug) fprintf(stderr, "[SERVER_START REC] SEQ: %u, ACK: %u", seq, ack);
            }
            return;
        }
        case CLIENT_AWAIT: {
            // CLIENT AWAITS for SYN-ACK
            // Becomes NORMAL after sending ACK
            return;
        }
        default: {
            if (pkt == NULL) return;
            // Receiver; process incoming data
            int payload_len = ntohs(pkt->length);
            if (payload_len > 0) {
                // Queue a copy of the packet avoid double frees
                packet* copy = malloc(sizeof(packet) + payload_len);
                if (!copy) return;
                memcpy(copy, pkt, sizeof(packet) + payload_len);
                queue(copy); 

                while ((copy = process_pkt()) != NULL) {
                    output(copy->payload, ntohs(copy->length));
                    free(copy);
                    pure_ack = true;
                }
            }
        
            // Sender and process incoming ACKs
            their_receiving_window = ntohs(pkt->win);
            uint16_t incoming_ack = ntohs(pkt->ack);
        
            if (incoming_ack > last_ack) {
                uint32_t bytes_freed = 0;
            
                while (send_buf != NULL &&
                       ntohs(send_buf->pkt->seq) + 1 <= incoming_ack) {
                    if (base_pkt == send_buf->pkt) {
                        base_pkt = (send_buf->next != NULL) ? send_buf->next->pkt : NULL;
                    }
            
                    bytes_freed += ntohs(send_buf->pkt->length);
            
                    buffer_node* tmp = send_buf;
                    send_buf = send_buf->next;
                    free(tmp->pkt);
                    free(tmp);
                }
            
                if (bytes_freed > our_send_window) {
                    our_send_window = 0;
                } else {
                    our_send_window -= bytes_freed;
                }
            
                last_ack = incoming_ack;
                dup_acks = 0;

                if (seq < incoming_ack) {
                    // This should be ok, if the rest is properly handled
                    // And incoming ACKs  should only ever be greater than seq
                    seq = incoming_ack;
                }
            }            
            else if (incoming_ack == last_ack) {
                dup_acks += 1;
            }
            return;
        }

    }
}

void test_recv_data_flow() {
    // Initial in order sequence:  SEQ 100, LEN 10
    ack = 100;
    state = NORMAL;
    packet* pkt1 = malloc(sizeof(packet) + 10);
    pkt1->seq = htons(100);
    pkt1->length = htons(10);
    memcpy(pkt1->payload, "ABCDEFGHIJ", 10);
    recv_data(pkt1);  // Should output

    // Out of order sequence: SEQ 120, LEN 10
    packet* pkt2 = malloc(sizeof(packet) + 11);
    pkt2->seq = htons(120);
    pkt2->length = htons(11);
    memcpy(pkt2->payload, "UVWXYZabc\n", 11);
    recv_data(pkt2);  // Should buffer, not output

    // Filled gap: SEQ 110, LEN 10
    packet* pkt3 = malloc(sizeof(packet) + 10);
    pkt3->seq = htons(110);
    pkt3->length = htons(10);
    memcpy(pkt3->payload, "KLMNOPQRST", 10);
    recv_data(pkt3);  // Should process 110 and 120

    // Output should be ABCDEFGHIJKLMNOPQRSTUVWXYZabc
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
    // if (state == CLIENT_START) seq = 456; // Matching ex in spec
    // if (state == SERVER_AWAIT) seq = 789;

    // Setting timers
    gettimeofday(&now, NULL);
    gettimeofday(&start, NULL);

    // Create buffer for incoming data
    char buffer[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet* pkt = (packet*) &buffer;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    if (test_cases)
        test_recv_data_flow();

    // Start listen loop
    while (true) {
        memset(buffer, 0, sizeof(packet) + MAX_PAYLOAD);
        // Get data from socket
        int bytes_recvd = recvfrom(sockfd, &buffer, sizeof(buffer), 0,
                                   (struct sockaddr*) addr, &addr_size);
        // If data, process it
        if (bytes_recvd > 0) {
            if (debug) {
                print_diag(pkt, RECV);
            }
            recv_data(pkt);
        }

        packet* tosend = get_data();
        // Data available to send
        if (tosend != NULL) {
            if (debug) {
                print_diag(tosend, SEND);

                if(extra_debug)
                    fprintf(stderr, "[About to Send] Sending packet: SEQ=%u, ACK=%u, LEN=%u, WIN=%u, FLAGS=%u\n",
                        ntohs(pkt->seq), ntohs(pkt->ack), ntohs(pkt->length),
                        ntohs(pkt->win), pkt->flags);
            }
            sendto(sockfd, tosend, sizeof(packet) + ntohs(tosend->length), 0,
                (struct sockaddr*) addr, addr_size);

            if (is_data_packet(tosend)) {
                gettimeofday(&start,NULL);
            }


            if(extra_debug) 
                fprintf("State: %i  -  SEQ: %i  -  ACK: %i\n", state, seq, ack);

            if (!(send_buf && tosend == base_pkt)) {
                free(tosend);
            };
        }
        // Received a packet and must send an ACK
        else if (pure_ack) {
            packet* ack_pkt = malloc(sizeof(packet));
            if (!ack_pkt) return;
        
            ack_pkt->seq = htons(0);
            ack_pkt->ack = htons(ack);
            ack_pkt->length = htons(0);
            ack_pkt->win = htons(our_max_receiving_window);
            ack_pkt->flags = ACK;
            ack_pkt->unused = 0;
        
            sendto(sockfd, ack_pkt, sizeof(packet), 0,
                   (struct sockaddr*) addr, addr_size);
            // if (debug) 
            print_diag(ack_pkt, SEND);
        
            free(ack_pkt);
            pure_ack = false;
        }        

        // Check if timer went off
        gettimeofday(&now, NULL);
        if (TV_DIFF(now, start) >= RTO && base_pkt != NULL) {
            if (debug)
                fprintf(stderr, "Timeout: Resending base packet (seq %hu)\n", ntohs(base_pkt->seq));
            sendto(sockfd, base_pkt, sizeof(packet) + ntohs(base_pkt->length), 0,
                    (struct sockaddr*) addr, addr_size);
            gettimeofday(&start, NULL);
        }
        // Duplicate ACKS detected
        else if (dup_acks == DUP_ACKS && base_pkt != NULL){
            if (debug)
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