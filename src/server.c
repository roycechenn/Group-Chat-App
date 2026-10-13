#include "../include/server.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

int write_func(int fd, const char *buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, buf + sent, len - sent);
    if (n <= 0)
      return -1;
    sent += (size_t)n;
  }
  return 0;
}

int writeTypeOne(int clientSize, struct clientInfo *clientInfo) {
  char buf_type1[2];
  buf_type1[0] = 1;
  buf_type1[1] = '\n';

  for (int i = 0; i < clientSize; i++) {
    if (clientInfo[i].active)
      if (write_func(clientInfo[i].cfd, buf_type1, 2) == -1)
        handle_error("write");
  }

  return 0;
}

int main(int argc, char *argv[]) {
  if (argc != 3) {
    fprintf(stderr, "Must have 3 arguments\n");
    exit(EXIT_FAILURE);
  }

  uint16_t portNumber = (uint16_t)atoi(argv[1]);
  int backlog = atoi(argv[2]);
  int doneSize = 0;

  struct clientInfo clientInfo[backlog];
  for (int i = 0; i < backlog; i++) {
    clientInfo[i].buf_len = 0;
    clientInfo[i].is_done = 0;
    clientInfo[i].active = 0;
  }

  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1)
    handle_error("socket");

  int flags = fcntl(sfd, F_GETFL, 0);
  if (flags == -1)
    handle_error("fcntl");
  if (fcntl(sfd, F_SETFL, flags | O_NONBLOCK) == -1)
    handle_error("fcntl");

  struct sockaddr_in addr, client_addr;
  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(portNumber);
  addr.sin_addr.s_addr = INADDR_ANY;

  socklen_t client_len = sizeof(client_addr);

  if (bind(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1)
    handle_error("bind");

  if (listen(sfd, backlog) == -1)
    handle_error("listen");

  for (;;) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sfd, &read_fds);
    int maxfd = sfd;

    for (int i = 0; i < backlog; i++) { // adds active fd to monitor
      if (clientInfo[i].active) {
        FD_SET(clientInfo[i].cfd, &read_fds);
        if (clientInfo[i].cfd > maxfd)
          maxfd = clientInfo[i].cfd;
      }
    }

    if (select(maxfd + 1, &read_fds, NULL, NULL, NULL) == -1)
      handle_error("select");

    if (FD_ISSET(sfd, &read_fds)) {
      for (;;) {
        client_len = sizeof(client_addr);
        int cfd = accept(sfd, (struct sockaddr *)&client_addr, &client_len);
        if (cfd == -1) {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          handle_error("accept");
        }
        int slot = -1;
        for (int i = 0; i < backlog; i++) { // find empty slot
          if (!clientInfo[i].active) {
            slot = i;
            break;
          }
        }

        if (slot != -1) {
          clientInfo[slot].cfd = cfd;
          clientInfo[slot].ip = client_addr.sin_addr.s_addr;
          clientInfo[slot].port = client_addr.sin_port;
          clientInfo[slot].is_done = 0;
          clientInfo[slot].buf_len = 0;
          clientInfo[slot].active = 1;
        } else {
          close(cfd);
        }
      }
    }

    // check active and valid cfd
    for (int i = 0; i < backlog; i++) {
      if (!clientInfo[i].active)
        continue;
      if (!FD_ISSET(clientInfo[i].cfd, &read_fds))
        continue;

      ssize_t num_read =
          read(clientInfo[i].cfd, clientInfo[i].buf + clientInfo[i].buf_len,
               BUF_SIZE - clientInfo[i].buf_len);

      if (num_read == 0) {
        close(clientInfo[i].cfd);
        clientInfo[i].active = 0;
        continue;
      }
      if (num_read < 0)
        handle_error("read");

      clientInfo[i].buf_len += (int)num_read;
    }
    int progress;
    do {
      progress = 0;
      for (int i = 0; i < backlog; i++) {
        if (!clientInfo[i].active)
          continue;
        if (clientInfo[i].buf_len == 0)
          continue;

        // check for new line, if none not complete, if yes then set len break
        int new_line_idx = -1;
        for (int j = 1; j < clientInfo[i].buf_len; j++) {
          if (clientInfo[i].buf[j] == '\n') {
            new_line_idx = j;
            break;
          }
        }

        if (new_line_idx == -1) // msg not fully received
          continue;

        progress = 1;
        if (clientInfo[i].buf[0] == 0) { // type 0

          // formatting the out buf, type ip port msg
          char buf_out[BUF_SIZE];
          int n = 0;
          buf_out[n++] = 0;
          memcpy(buf_out + n, &clientInfo[i].ip, sizeof(uint32_t));
          n += sizeof(uint32_t);
          memcpy(buf_out + n, &clientInfo[i].port, sizeof(uint16_t));
          n += sizeof(uint16_t);
          memcpy(buf_out + n, clientInfo[i].buf + 1, new_line_idx);
          n += new_line_idx;

          // sends msg to everyone
          for (int j = 0; j < backlog; j++) {
            if (clientInfo[j].active) {
              if (write_func(clientInfo[j].cfd, buf_out, n) == -1)
                handle_error("write");
            }
          }
        } else if (clientInfo[i].buf[0] == 1 && new_line_idx == 1) { // type 1

          if (!clientInfo[i].is_done) {
            clientInfo[i].is_done = 1;
            doneSize++;
          }

          if (doneSize == backlog) {
            if (writeTypeOne(backlog, clientInfo) != 0)
              handle_error("write");
            for (int j = 0; j < backlog; j++)
              close(clientInfo[j].cfd);
            close(sfd);
            return 0;
          }
        }

        // [msg1][msg2][msg3] with '\n' shift buf and decrement buf_len
        int end_msg = new_line_idx + 1;
        memmove(clientInfo[i].buf, clientInfo[i].buf + end_msg,
                clientInfo[i].buf_len - end_msg);
        clientInfo[i].buf_len -= end_msg;
      }
    } while (progress);
  }
  close(sfd);
  return 0;
}
