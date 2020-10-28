/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
  +----------------------------------------------------------------------+
*/

#include <poll.h>

#include "swoole.h"
#include "swoole_socket.h"
#include "swoole_reactor.h"
using swoole::Reactor;
using swoole::ReactorHandler;
using swoole::network::Socket;

static int swReactorPoll_add(Reactor *reactor, Socket *socket, int events);
static int swReactorPoll_set(Reactor *reactor, Socket *socket, int events);
static int swReactorPoll_del(Reactor *reactor, Socket *socket);
static int swReactorPoll_wait(Reactor *reactor, struct timeval *timeo);
static void swReactorPoll_free(Reactor *reactor);
static bool swReactorPoll_exist(Reactor *reactor, int fd);

struct swReactorPoll {
    uint32_t max_fd_num;
    Socket **fds;
    struct pollfd *events;
};

int swReactorPoll_create(Reactor *reactor, int max_fd_num) {
    swReactorPoll *object = new swReactorPoll();
    object->fds = new Socket *[max_fd_num];
    object->events = new struct pollfd[max_fd_num];

    object->max_fd_num = max_fd_num;
    reactor->max_event_num = max_fd_num;
    reactor->object = object;
    reactor->add = swReactorPoll_add;
    reactor->del = swReactorPoll_del;
    reactor->set = swReactorPoll_set;
    reactor->wait = swReactorPoll_wait;
    reactor->free = swReactorPoll_free;

    return SW_OK;
}

static void swReactorPoll_free(Reactor *reactor) {
    swReactorPoll *object = (swReactorPoll *) reactor->object;
    delete[] object->fds;
    delete[] object->events;
    delete object;
}

static int swReactorPoll_add(Reactor *reactor, Socket *socket, int events) {
    int fd = socket->fd;
    if (swReactorPoll_exist(reactor, fd)) {
        swWarn("fd#%d is already exists", fd);
        return SW_ERR;
    }

    swReactorPoll *object = (swReactorPoll *) reactor->object;
    int cur = reactor->event_num;
    if (reactor->event_num == object->max_fd_num) {
        swWarn("too many connection, more than %d", object->max_fd_num);
        return SW_ERR;
    }

    reactor->_add(socket, events);

    swTrace("fd=%d, events=%d", fd, events);

    object->fds[cur] = socket;
    object->events[cur].fd = fd;
    object->events[cur].events = 0;

    if (Reactor::isset_read_event(events)) {
        object->events[cur].events |= POLLIN;
    }
    if (Reactor::isset_write_event(events)) {
        object->events[cur].events |= POLLOUT;
    }
    if (Reactor::isset_error_event(events)) {
        object->events[cur].events |= POLLHUP;
    }

    return SW_OK;
}

static int swReactorPoll_set(Reactor *reactor, Socket *socket, int events) {
    uint32_t i;
    swReactorPoll *object = (swReactorPoll *) reactor->object;

    swTrace("fd=%d, events=%d", socket->fd, events);

    for (i = 0; i < reactor->event_num; i++) {
        // found
        if (object->events[i].fd == socket->fd) {
            object->events[i].events = 0;
            if (Reactor::isset_read_event(events)) {
                object->events[i].events |= POLLIN;
            }
            if (Reactor::isset_write_event(events)) {
                object->events[i].events |= POLLOUT;
            }
            // execute parent method
            reactor->_set(socket, events);
            return SW_OK;
        }
    }

    return SW_ERR;
}

static int swReactorPoll_del(Reactor *reactor, Socket *socket) {
    uint32_t i;
    swReactorPoll *object = (swReactorPoll *) reactor->object;

    if (socket->removed) {
        swoole_error_log(SW_LOG_WARNING, SW_ERROR_EVENT_SOCKET_REMOVED, 
            "failed to delete event[%d], it has already been removed", socket->fd);
        return SW_ERR;
    }

    for (i = 0; i < reactor->event_num; i++) {
        if (object->events[i].fd == socket->fd) {
            for (; i < reactor->event_num; i++) {
                if (i == reactor->event_num) {
                    object->fds[i] = nullptr;
                    object->events[i].fd = 0;
                    object->events[i].events = 0;
                } else {
                    object->fds[i] = object->fds[i + 1];
                    object->events[i] = object->events[i + 1];
                }
            }
            reactor->_del(socket);
            return SW_OK;
        }
    }
    return SW_ERR;
}

static int swReactorPoll_wait(Reactor *reactor, struct timeval *timeo) {
    swReactorPoll *object = (swReactorPoll *) reactor->object;
    swEvent event;
    ReactorHandler handler;

    int ret;

    if (reactor->timeout_msec == 0) {
        if (timeo == nullptr) {
            reactor->timeout_msec = -1;
        } else {
            reactor->timeout_msec = timeo->tv_sec * 1000 + timeo->tv_usec / 1000;
        }
    }

    reactor->before_wait();

    while (reactor->running) {
        if (reactor->onBegin != nullptr) {
            reactor->onBegin(reactor);
        }
        ret = poll(object->events, reactor->event_num, reactor->get_timeout_msec());
        if (ret < 0) {
            if (!reactor->catch_error()) {
                swSysWarn("poll error");
                break;
            } else {
                goto _continue;
            }
        } else if (ret == 0) {
            reactor->execute_end_callbacks(true);
            SW_REACTOR_CONTINUE;
        } else {
            for (uint32_t i = 0; i < reactor->event_num; i++) {
                event.socket = object->fds[i];
                event.fd = object->events[i].fd;
                event.reactor_id = reactor->id;
                event.type = event.socket->fd_type;

                swTrace("Event: fd=%d|reactor_id=%d|type=%d", event.fd, reactor->id, event.type);
                // in
                if ((object->events[i].revents & POLLIN) && !event.socket->removed) {
                    if (object->events[i].revents & (POLLHUP | POLLERR)) {
                        event.socket->event_hup = 1;
                    }
                    handler = reactor->get_handler(SW_EVENT_READ, event.type);
                    ret = handler(reactor, &event);
                    if (ret < 0) {
                        swSysWarn("poll[POLLIN] handler failed. fd=%d", event.fd);
                    }
                }
                // out
                if ((object->events[i].revents & POLLOUT) && !event.socket->removed) {
                    handler = reactor->get_handler(SW_EVENT_WRITE, event.type);
                    ret = handler(reactor, &event);
                    if (ret < 0) {
                        swSysWarn("poll[POLLOUT] handler failed. fd=%d", event.fd);
                    }
                }
                // error
                if ((object->events[i].revents & (POLLHUP | POLLERR)) && !event.socket->removed) {
                    // ignore ERR and HUP, because event is already processed at IN and OUT handler.
                    if ((object->events[i].revents & POLLIN) || (object->events[i].revents & POLLOUT)) {
                        continue;
                    }
                    handler = reactor->get_handler(SW_EVENT_ERROR, event.type);
                    ret = handler(reactor, &event);
                    if (ret < 0) {
                        swSysWarn("poll[POLLERR] handler failed. fd=%d", event.fd);
                    }
                }
                if (!event.socket->removed && (event.socket->events & SW_EVENT_ONCE)) {
                    swReactorPoll_del(reactor, event.socket);
                }
            }
        }
    _continue:
        reactor->execute_end_callbacks(false);
        SW_REACTOR_CONTINUE;
    }
    return SW_OK;
}

static bool swReactorPoll_exist(Reactor *reactor, int fd) {
    swReactorPoll *object = (swReactorPoll *) reactor->object;
    for (uint32_t i = 0; i < reactor->event_num; i++) {
        if (object->events[i].fd == fd) {
            return true;
        }
    }
    return false;
}
