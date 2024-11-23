#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "io_watcher.h"

iow_event_t *iow_event_new(int fd, enum iow_event_type type,
                           iow_event_handler_f handler, void *user_ptr) {
  assert(fd && handler);
  iow_event_t *event = malloc(sizeof(*event));
  assert(event);
  *event = (iow_event_t){
      .fd = fd,
      .type = type,
      .handler = handler,
      .user_ptr = user_ptr,
  };
  return event;
}

void iow_event_delete(iow_event_t *event) {
  assert(event);
  // TODO: user provided cleanup function
  if (event->user_ptr)
    free(event->user_ptr);
  free(event);
}


int iow_init(iow_t *iow, size_t capacity) {
  assert(iow);
  iow->backend = epoll_create1(0);
  assert(iow->backend != -1);
  iow->events = malloc(sizeof(struct epoll_event) * capacity);
  assert(iow->events);
  iow->capacity = capacity;
  iow->nregistered = 0;
  return 0;
}

void iow_destroy(iow_t *iow) {
  assert(iow);
  free(iow->events);
  close(iow->backend);
  iow->capacity = iow->nregistered = 0;
}

void iow_register(iow_t *iow, const iow_event_t *e) {
  assert(iow && e);
  struct epoll_event event = {
    .events = e->type,
    .data.ptr = (void *)e,
  };
  int r = epoll_ctl(iow->backend, EPOLL_CTL_ADD, e->fd, &event);
  assert(r != -1);
}

void iow_unregister(iow_t *iow, const iow_event_t *e) {
  assert(iow && e);
  int r = epoll_ctl(iow->backend, EPOLL_CTL_DEL, e->fd, NULL);
  assert(r != -1);
}

int iow_poll_events(iow_t *iow) {
  assert(iow);
  int nevents = epoll_wait(iow->backend, iow->events, iow->capacity, -1);
  iow_event_t *event;
  for (int i = 0; i < nevents; ++i) {
    event = iow->events[i].data.ptr;
    event->handler(event->fd, iow->events[i].events, event->user_ptr);
  }
  return nevents;
}

