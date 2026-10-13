#include <stdint.h>
#include <unistd.h>

int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size);
int write_func(int fd, char const *buf, int len);
void *read_thread(void *arg_);
void *write_thread(void *arg_);
