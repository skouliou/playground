#ifndef _IO_WATCHER_H
#define _IO_WATCHER_H

#include <assert.h>
#include <stddef.h>
#include <sys/epoll.h>

enum iow_event_type {
  IOW_INPUT = EPOLLIN,
  IOW_OUTPUT = EPOLLOUT,
  IOW_EDGETRIGER = EPOLLET,
};

struct iow_s {
  int backend;
  size_t nregistered;
  size_t capacity;
  struct epoll_event *events;
};


typedef struct iow_s iow_t;
typedef struct iow_event_s iow_event_t;
typedef void (*iow_event_handler_f)(int fd, enum iow_event_type type,
                                    void *user_ptr);

struct iow_event_s {
  void *user_ptr;
  iow_event_handler_f handler;
  enum iow_event_type type;
  int fd;
};

iow_event_t *iow_event_new(int fd, enum iow_event_type type,
                           iow_event_handler_f handler, void *user_ptr);

void iow_event_delete(iow_event_t *event);

int iow_init(iow_t *iow, size_t capacity);

void iow_destroy(iow_t *iow);

void iow_register(iow_t *iow, const iow_event_t *e);

void iow_unregister(iow_t *iow, const iow_event_t *e);

int iow_poll_events(iow_t *iow);

#endif // _IO_WATCHER_H
