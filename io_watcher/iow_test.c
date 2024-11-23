#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../thread_pool/log.h"
#include "io_watcher.h"

#define PORT 8080
#define BACKLOG 1024

char addrstr[INET_ADDRSTRLEN] = {0};

struct context {
  iow_t *io_watcher;
  iow_event_t *src;
};

struct conn_ctx {
  iow_t *io_watcher;
  iow_event_t *src;
};

void client_handler(int fd, enum iow_event_type type, void *user_ptr) {
  assert(fd && user_ptr);
  static char buffer[1024] = {0};

  struct conn_ctx *ctx = user_ptr;

  if (type & IOW_INPUT) {
    log_info("client available input");
    ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);

  try_again:
    if (bytes == 0) {
      log_info("(-) client closed connection");
      iow_unregister(ctx->io_watcher, ctx->src);
      iow_event_delete(ctx->src);
      close(fd);
      return;
    }

    if (bytes == -1) {
      // reading interrupted by a signal
      if (errno == EINTR) {
        log_warn("(!) %s", strerror(errno));
        goto try_again;
      }
      // reading would block current thread
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        log_warn("(!) reading shouldn't block (%s)", strerror(errno));
        return;
      }
    }
    buffer[bytes] = '\0';
    printf("client: %s", buffer);
    return;
  }

  if (type & IOW_OUTPUT) {
    log_info("client ready to receive data");
    return;
  }
}

void accept_client(int fd, enum iow_event_type type, void *user_ptr) {
  assert(type == IOW_INPUT && user_ptr);

  struct context *ctx = user_ptr;

  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);

  int peerfd = accept4(fd, (struct sockaddr *)&addr, &addrlen,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (peerfd == -1) {
    log_warn("couldn't accept connection (%d)", strerror(errno));
    return;
  }

  inet_ntop(AF_INET, &addr.sin_addr, addrstr, sizeof(addrstr));
  log_info("(+) (%s:%d) conneted", addrstr, ntohs(addr.sin_port));

  struct conn_ctx *peer_ctx = malloc(sizeof(*peer_ctx));
  assert(peer_ctx);
  iow_event_t *event = iow_event_new(peerfd, IOW_INPUT | IOW_OUTPUT | IOW_EDGETRIGER, client_handler, peer_ctx);
  *peer_ctx = (struct conn_ctx) {
    .io_watcher = ctx->io_watcher,
    .src = event,
  };

  // register peer fd with this `iow`
  iow_register(ctx->io_watcher, event);
}

int main(void) {
  log_info("Creating socket...");
  int sfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
  struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(PORT),
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
      // .sin_addr.s_addr = INADDR_ANY,
  };

  int on = 1;
  int r = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (r == -1) {
    log_fatal("couldn't set socket option SO_REUSEADDR (%s)", strerror(errno));
    exit(EXIT_FAILURE);
  }

  inet_ntop(AF_INET, &addr.sin_addr.s_addr, (char *)addrstr, sizeof(addrstr));

  log_info("Binding to address (%s:%d)...", addrstr, PORT);
  r = bind(sfd, (struct sockaddr *)&addr, sizeof(addr));
  if (r == -1) {
    log_fatal("couldn't bind to address (%s) (%s)", addrstr, strerror(errno));
    exit(EXIT_FAILURE);
  }

  log_info("Listening for incomming connection");
  r = listen(sfd, BACKLOG);
  if (r == -1) {
    log_fatal("couldn't listen on address (%s)", addrstr);
    exit(EXIT_FAILURE);
  }

  iow_t iow;
  iow_init(&iow, 1024);

  struct context ctx = {
      .io_watcher = &iow,
  };

  iow_event_t *e =
      iow_event_new(sfd, IOW_INPUT | IOW_EDGETRIGER, accept_client, &ctx);

  iow_register(&iow, e);

  log_info("Starting server...");
  while (1) {
    iow_poll_events(&iow);
  }

  return 0;
}
