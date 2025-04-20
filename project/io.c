#include <stdint.h>
#include <sys/fcntl.h>
#include <unistd.h>

void init_io() {
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

ssize_t input_io(uint8_t* buf, size_t max_length) {
    ssize_t len = read(STDIN_FILENO, buf, max_length);
    return len > 0 ? len : 0;
}

void output_io(uint8_t* buf, size_t length) {
    write(STDOUT_FILENO, buf, length); 
}
