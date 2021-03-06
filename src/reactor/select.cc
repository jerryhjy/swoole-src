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

#include "swoole.h"
#include "swoole_socket.h"
#include "swoole_reactor.h"
#include <unordered_map>

#include <sys/select.h>

using swoole::Reactor;
using swoole::ReactorHandler;
using swoole::network::Socket;

struct swReactorSelect {
    fd_set rfds;
    fd_set wfds;
    fd_set efds;
    std::unordered_map<int, Socket *> fds;
    int maxfd;

    swReactorSelect() {
        maxfd = 0;
    }
};

#define SW_FD_SET(fd, set)                                                                                             \
    do {                                                                                                               \
        if (fd < FD_SETSIZE) FD_SET(fd, set);                                                                          \
    } while (0)
#define SW_FD_CLR(fd, set)                                                                                             \
    do {                                                                                                               \
        if (fd < FD_SETSIZE) FD_CLR(fd, set);                                                                          \
    } while (0)
#define SW_FD_ISSET(fd, set) ((fd < FD_SETSIZE) && FD_ISSET(fd, set))

static int swReactorSelect_add(Reactor *reactor, Socket *socket, int events);
static int swReactorSelect_set(Reactor *reactor, Socket *socket, int events);
static int swReactorSelect_del(Reactor *reactor, Socket *socket);
static int swReactorSelect_wait(Reactor *reactor, struct timeval *timeo);
static void swReactorSelect_free(Reactor *reactor);

int swReactorSelect_create(Reactor *reactor) {
    // create reactor object
    swReactorSelect *object = new swReactorSelect();
    reactor->object = object;
    // binding method
    reactor->add = swReactorSelect_add;
    reactor->set = swReactorSelect_set;
    reactor->del = swReactorSelect_del;
    reactor->wait = swReactorSelect_wait;
    reactor->free = swReactorSelect_free;

    return SW_OK;
}

void swReactorSelect_free(Reactor *reactor) {
    swReactorSelect *object = (swReactorSelect *) reactor->object;
    delete object;
}

int swReactorSelect_add(Reactor *reactor, Socket *socket, int events) {
    int fd = socket->fd;
    if (fd > FD_SETSIZE) {
        swWarn("max fd value is FD_SETSIZE(%d).\n", FD_SETSIZE);
        return SW_ERR;
    }

    swReactorSelect *object = (swReactorSelect *) reactor->object;
    reactor->_add(socket, events);
    object->fds.emplace(fd, socket);
    if (fd > object->maxfd) {
        object->maxfd = fd;
    }

    return SW_OK;
}

int swReactorSelect_del(Reactor *reactor, Socket *socket) {
    swReactorSelect *object = (swReactorSelect *) reactor->object;
    if (socket->removed) {
        swoole_error_log(SW_LOG_WARNING, SW_ERROR_EVENT_SOCKET_REMOVED, 
            "failed to delete event[%d], it has already been removed", socket->fd);
        return SW_ERR;
    }
    int fd = socket->fd;
    if (object->fds.erase(fd) == 0) {
        swWarn("swReactorSelect: fd[%d] not found", fd);
        return SW_ERR;
    }
    SW_FD_CLR(fd, &object->rfds);
    SW_FD_CLR(fd, &object->wfds);
    SW_FD_CLR(fd, &object->efds);
    reactor->_del(socket);
    return SW_OK;
}

int swReactorSelect_set(Reactor *reactor, Socket *socket, int events) {
    swReactorSelect *object = (swReactorSelect *) reactor->object;
    auto i = object->fds.find(socket->fd);
    if (i == object->fds.end()) {
        swWarn("swReactorSelect: sock[%d] not found", socket->fd);
        return SW_ERR;
    }
    reactor->_set(socket, events);
    return SW_OK;
}

int swReactorSelect_wait(Reactor *reactor, struct timeval *timeo) {
    swReactorSelect *object = (swReactorSelect *) reactor->object;
    swEvent event;
    ReactorHandler handler;
    struct timeval timeout;
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
        FD_ZERO(&(object->rfds));
        FD_ZERO(&(object->wfds));
        FD_ZERO(&(object->efds));

        if (reactor->onBegin != nullptr) {
            reactor->onBegin(reactor);
        }

        for (auto i = object->fds.begin(); i != object->fds.end(); i++) {
            int fd = i->first;
            int events = i->second->events;
            if (Reactor::isset_read_event(events)) {
                SW_FD_SET(fd, &(object->rfds));
            }
            if (Reactor::isset_write_event(events)) {
                SW_FD_SET(fd, &(object->wfds));
            }
            if (Reactor::isset_error_event(events)) {
                SW_FD_SET(fd, &(object->efds));
            }
        }

        if (reactor->timeout_msec < 0) {
            timeout.tv_sec = UINT_MAX;
            timeout.tv_usec = 0;
        } else if (reactor->defer_tasks) {
            timeout.tv_sec = 0;
            timeout.tv_usec = 0;
        } else {
            timeout.tv_sec = reactor->timeout_msec / 1000;
            timeout.tv_usec = reactor->timeout_msec - timeout.tv_sec * 1000;
        }

        ret = select(object->maxfd + 1, &(object->rfds), &(object->wfds), &(object->efds), &timeout);
        if (ret < 0) {
            if (!reactor->catch_error()) {
                swSysWarn("select error");
                break;
            } else {
                goto _continue;
            }
        } else if (ret == 0) {
            reactor->execute_end_callbacks(true);
            SW_REACTOR_CONTINUE;
        } else {
            for (int fd = 0; fd <= object->maxfd; fd++) {
                auto i = object->fds.find(fd);
                if (i == object->fds.end()) {
                    continue;
                }
                event.socket = i->second;
                event.fd = event.socket->fd;
                event.reactor_id = reactor->id;
                event.type = event.socket->fd_type;

                // read
                if (SW_FD_ISSET(event.fd, &(object->rfds)) && !event.socket->removed) {
                    handler = reactor->get_handler(SW_EVENT_READ, event.type);
                    ret = handler(reactor, &event);
                    if (ret < 0) {
                        swSysWarn("[Reactor#%d] select event[type=READ, fd=%d] handler fail", reactor->id, event.fd);
                    }
                }
                // write
                if (SW_FD_ISSET(event.fd, &(object->wfds)) && !event.socket->removed) {
                    handler = reactor->get_handler(SW_EVENT_WRITE, event.type);
                    ret = handler(reactor, &event);
                    if (ret < 0) {
                        swSysWarn("[Reactor#%d] select event[type=WRITE, fd=%d] handler fail", reactor->id, event.fd);
                    }
                }
                // error
                if (SW_FD_ISSET(event.fd, &(object->efds)) && !event.socket->removed) {
                    handler = reactor->get_handler(SW_EVENT_ERROR, event.type);
                    ret = handler(reactor, &event);
                    if (ret < 0) {
                        swSysWarn("[Reactor#%d] select event[type=ERROR, fd=%d] handler fail", reactor->id, event.fd);
                    }
                }
                if (!event.socket->removed && (event.socket->events & SW_EVENT_ONCE)) {
                    swReactorSelect_del(reactor, event.socket);
                }
            }
        }
    _continue:
        reactor->execute_end_callbacks(false);
        SW_REACTOR_CONTINUE;
    }
    return SW_OK;
}
