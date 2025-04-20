#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>

// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int type,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {

    while (true) {

    }
}
