#include "../src/tcp_listener.h"
#include <stdio.h>

int main(void) {
  // create a blocking TCP/IPv4 socket
  int sock = tcp_listener_ipv4("127.0.0.1", 8080, 1, 0);
  if (sock != -1) {
    printf("socket()\n");
  }
  tcp_listener_ipv4_close(sock);
  return 1;
}
