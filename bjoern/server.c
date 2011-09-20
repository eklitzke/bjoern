#include <Python.h>
#include "structmember.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#ifdef WANT_SIGINT_HANDLING
# include <sys/signal.h>
#endif
#include <sys/sendfile.h>
#include <ev.h>
#include "common.h"
#include "filewrapper.h"
#include "wsgi.h"
#include "server.h"

#define LISTEN_BACKLOG  1024
#define READ_BUFFER_SIZE 64*1024
#define Py_XCLEAR(obj) do { if(obj) { Py_DECREF(obj); obj = NULL; } } while(0)
#define GIL_LOCK(n) PyGILState_STATE _gilstate_##n = PyGILState_Ensure()
#define GIL_UNLOCK(n) PyGILState_Release(_gilstate_##n)

static const char* http_error_messages[4] = {
  NULL, /* Error codes start at 1 because 0 means "no error" */
  "HTTP/1.1 400 Bad Request\r\n\r\n",
  "HTTP/1.1 406 Length Required\r\n\r\n",
  "HTTP/1.1 500 Internal Server Error\r\n\r\n"
};

typedef void ev_io_callback(struct ev_loop*, ev_io*, const int);
#if WANT_SIGINT_HANDLING
typedef void ev_signal_callback(struct ev_loop*, ev_signal*, const int);
static ev_signal_callback ev_signal_on_sigint;
#endif
static ev_io_callback ev_io_on_request;
static ev_io_callback ev_io_on_read;
static ev_io_callback ev_io_on_write;
static bool send_chunk(Request*);
static bool do_sendfile(Request*);
static bool handle_nonzero_errno(Request*);

static PyObject *_server_fd_map;
static PyObject *_client_fd_map;

/* Get a WSGIServer object from a file descriptor. Note that it's not necessary
 * to increase the reference count of the server object here before using it
 * from arbitrary Python code, since _server_fd_map retains a hidden reference
 * to the server (and the _server_fd_map dictionary is hidden from non-module
 * Python code).
 */
static WsgiServer*
wsgi_server_from_fd(PyObject *map, int fd) {
  WsgiServer *return_val;
  PyObject *int_obj = PyInt_FromLong((long) fd);
  return_val = (WsgiServer *) PyDict_GetItem(map, int_obj);
  Py_DECREF(int_obj);
  assert(return_val != NULL);
  assert(return_val->socket == fd);
  return return_val;
}

static bool
add_fd_to_map(PyObject* map, int fd, WsgiServer* val)
{
  // XXX: add error handling
  PyObject *py_fd = PyInt_FromLong((long) fd);
  PyDict_SetItem(map, py_fd, (PyObject *) val);
  Py_DECREF(py_fd);
  return true;
}

static void
WsgiServer_dealloc(WsgiServer *self)
{
  if (self->socket >= 0) {
    close(self->socket);
  }
  if (self->sock != NULL) {
    PyDict_DelItem(_server_fd_map, self->sock);
    Py_DECREF(self->sock);
  }
  Py_DECREF(self->callbacks);
  Py_DECREF(self->wsgi_base_dict);
  self->ob_type->tp_free((PyObject*)self);
}

static int
WsgiServer_init(WsgiServer *self, PyObject *args, PyObject *kwds)
{
  static char *kwlist[] = {"application", "callbacks", NULL};
  self->socket = -1;

  if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|O", kwlist, &self->application, &self->callbacks)) {
    return -1;
  }

  if (!PyCallable_Check(self->application)) {
    PyErr_SetString(PyExc_TypeError, "application must be callable");
    return -1;
  }

  if (!self->callbacks) {
    if ((self->callbacks = PyDict_New()) == NULL)
      return -1;
  } else if (!PyDict_Check(self->callbacks)) {
    PyErr_SetString(PyExc_TypeError, "callbacks must be a dictionary");
    return -1;
  } else {
    Py_INCREF(self->callbacks);
  }

  Py_INCREF(Py_None);
  self->sock = Py_None;

  Py_INCREF(Py_None);
  self->wsgi_base_dict = Py_None;

  return 0;
}

static PyObject*
WsgiServer_listen(WsgiServer *self, PyObject*args) {
  char *host;
  int port;
  struct sockaddr_in sockaddr;

  if(!PyArg_ParseTuple(args, "si:listen", &host, &port))
    return NULL;

  if (port < 0 || port >= 65536) {
    PyErr_SetString(PyExc_ValueError, "port not in range [0, 65535]");
    return NULL;
  }

  /* If we already listened() for this server, we need to close the existing
   * socket
   */
  if (self->socket >= 0) {
    PyDict_DelItem(_server_fd_map, self->sock);
    Py_DECREF(self->sock);
    close(self->socket);
    self->socket = -1;
  }

  if ((self->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    PyErr_SetString(PyExc_IOError, "failed to create socket");
    return NULL;
  }

  /* expose the python version of the socket fd */
  if ((self->sock = PyInt_FromLong((long) self->socket)) == NULL) {
    close(self->socket);
    self->socket = -1;
    return NULL;
  }

  /* update the global socket map */
  if (PyDict_SetItem(_server_fd_map, self->sock, (PyObject *) self) < 0) {
    close(self->socket);
    self->socket = -1;
    Py_DECREF(self->sock);
    return NULL;
  }

  sockaddr.sin_family = AF_INET;
  inet_pton(AF_INET, host, &sockaddr.sin_addr);
  sockaddr.sin_port = htons((uint16_t) port);

  /* Set SO_REUSEADDR t make the IP address available for reuse */
  int optval = 1;
  setsockopt(self->socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  if (bind(self->socket, (struct sockaddr*)&sockaddr, sizeof(sockaddr)) < 0) {
    PyErr_SetString(PyExc_IOError, "failed to bind()");
    goto listen_err;
  }

  if (listen(self->socket, LISTEN_BACKLOG) < 0) {
    PyErr_SetString(PyExc_IOError, "failed to listen()");
    goto listen_err;
  }

  DBG("Listening on %s:%d...", host, port);

  Py_DECREF(self->wsgi_base_dict);
  self->wsgi_base_dict = PyDict_New();
  
  /* dct['wsgi.file_wrapper'] = FileWrapper */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.file_wrapper",
    (PyObject*)&FileWrapper_Type
    );
  
  /* dct['SCRIPT_NAME'] = '' */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "SCRIPT_NAME",
    _empty_string
    );
  
  /* dct['wsgi.version'] = (1, 0) */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.version",
    PyTuple_Pack(2, PyInt_FromLong(1), PyInt_FromLong(0))
    );
  
  /* dct['wsgi.url_scheme'] = 'http'
   * (This can be hard-coded as there is no TLS support in bjoern.) */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.url_scheme",
    PyString_FromString("http")
    );
  
  /* dct['wsgi.errors'] = sys.stderr */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.errors",
    PySys_GetObject("stderr")
    );
  
  /* dct['wsgi.multithread'] = True
   * If I correctly interpret the WSGI specs, this means
   * "Can the server be ran in a thread?" */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.multithread",
    Py_True
    );

  /* dct['wsgi.multiprocess'] = True
   * ... and this one "Can the server process be forked?" */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.multiprocess",
    Py_True
    );
  
  /* dct['wsgi.run_once'] = False (bjoern is no CGI gateway) */
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "wsgi.run_once",
    Py_False
    );

  PyDict_SetItemString(
    self->wsgi_base_dict,
    "SERVER_NAME",
    PyString_FromString(host)
    );
  
  PyDict_SetItemString(
    self->wsgi_base_dict,
    "SERVER_PORT",
    PyString_FromFormat("%d", port)
    );
  
  Py_RETURN_NONE;

listen_err:
  PyDict_DelItem(_server_fd_map, self->sock);
  Py_DECREF(self->sock);
  close(self->socket);
  self->socket = -1;
  return NULL;
}

static void
WsgiServer_run_callback(WsgiServer *server, const char *callback_name) {
  PyObject *cb;
  cb = PyDict_GetItemString(server->callbacks, callback_name);
  if (cb && PyCallable_Check(cb)) {
    Py_INCREF(cb);
    PyObject_CallObject(cb, NULL);
    Py_DECREF(cb);
  }
}

static PyMemberDef WsgiServer_members[] = {
  {"application", T_OBJECT_EX, offsetof(WsgiServer, application), 0, "the WSGI application"},
  {"_callbacks", T_OBJECT_EX, offsetof(WsgiServer, callbacks), 0, "callbacks to run"},
  {"_environ_template", T_OBJECT_EX, offsetof(WsgiServer, wsgi_base_dict), 0, "the WSGI environ template"},
  {"_sockfd", T_OBJECT_EX, offsetof(WsgiServer, sock), 0, "the fd of the socket"},
  {NULL}
};

static PyMethodDef WsgiServer_methods[] = {
  {"listen", (PyCFunction) WsgiServer_listen, METH_VARARGS, "listen"},
  {NULL, NULL, 0, NULL}
};

PyTypeObject WsgiServer_Type = {
  PyVarObject_HEAD_INIT(NULL, 0)
  "WSGIServer",                     /* tp_name (__name__)                     */
  sizeof(WsgiServer),               /* tp_basicsize                           */
  0,                                /* tp_itemsize                            */
  (destructor)WsgiServer_dealloc,   /* tp_dealloc                             */
};


bool server_run(void)
{
  struct ev_loop* mainloop = ev_default_loop(0);
  ev_io accept_watcher;
  
  /* watch each file descriptor; TODO: need a way to add file descriptors into
   * the loop later on (i.e. after bjoern is already running)
   */
  PyObject *iterator = PyObject_GetIter(_server_fd_map);
  PyObject *item;
  if (iterator == NULL) {
    return false;
  }
  while ((item = PyIter_Next(iterator))) {
    int sockfd = (int) PyInt_AS_LONG(item);
    Py_DECREF(item);
    DBG("watching socket for client connections %d", sockfd);
    ev_io_init(&accept_watcher, ev_io_on_request, sockfd, EV_READ);
  }
  Py_DECREF(iterator);

  if (PyErr_Occurred()) {
    return false;
  }

  ev_io_start(mainloop, &accept_watcher);

#if WANT_SIGINT_HANDLING
  ev_signal signal_watcher;
  ev_signal_init(&signal_watcher, ev_signal_on_sigint, SIGINT);
  ev_signal_start(mainloop, &signal_watcher);
#endif

  /* This is the program main loop */
  Py_BEGIN_ALLOW_THREADS
  ev_loop(mainloop, 0);
  Py_END_ALLOW_THREADS
  return true;
}

#if WANT_SIGINT_HANDLING
static void
ev_signal_on_sigint(struct ev_loop* mainloop, ev_signal* watcher, const int events)
{
  /* Clean up and shut down this thread.
   * (Shuts down the Python interpreter if this is the main thread) */
  ev_unloop(mainloop, EVUNLOOP_ALL);
  PyErr_SetInterrupt();
}
#endif

bool server_init(const char* hostaddr, const int port)
{
  return true;
}

static void
ev_io_on_request(struct ev_loop* mainloop, ev_io* watcher, const int events)
{
  int client_fd;
  struct sockaddr_in sockaddr;
  socklen_t addrlen;

  addrlen = sizeof(struct sockaddr_in);
  client_fd = accept(watcher->fd, (struct sockaddr*)&sockaddr, &addrlen);
  if(client_fd < 0) {
    DBG("Could not accept() client: errno %d", errno);
    return;
  }

  int flags = fcntl(client_fd, F_GETFL, 0);
  if(fcntl(client_fd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK) == -1) {
    DBG("Could not set_nonblocking() client %d: errno %d", client_fd, errno);
    return;
  }

  GIL_LOCK(0);
  WsgiServer *server = wsgi_server_from_fd(_server_fd_map, watcher->fd);
  WsgiServer_run_callback(server, "connect_callback");
  add_fd_to_map(_client_fd_map, client_fd, server);
  Request* request = Request_new(client_fd, inet_ntoa(sockaddr.sin_addr), server);
  GIL_UNLOCK(0);

  DBG_REQ(request, "Accepted client %s:%d on fd %d",
          inet_ntoa(sockaddr.sin_addr), ntohs(sockaddr.sin_port), client_fd);

  ev_io_init(&request->ev_watcher, &ev_io_on_read,
             client_fd, EV_READ);
  ev_io_start(mainloop, &request->ev_watcher);
}

static void
ev_io_on_read(struct ev_loop* mainloop, ev_io* watcher, const int events)
{
  static char read_buf[READ_BUFFER_SIZE];

  Request* request = REQUEST_FROM_WATCHER(watcher);

  ssize_t read_bytes = read(
    request->client_fd,
    read_buf,
    READ_BUFFER_SIZE
  );

  GIL_LOCK(0);

  if(read_bytes <= 0) {
    if(errno != EAGAIN && errno != EWOULDBLOCK) {
      if(read_bytes == 0)
        DBG_REQ(request, "Client disconnected");
      else
        DBG_REQ(request, "Hit errno %d while read()ing", errno);

      // XXX _run_fd_callback("close_callback", request->client_fd);
      close(request->client_fd);
      Request_free(request);
      ev_io_stop(mainloop, &request->ev_watcher);
    }
    goto out;
  }

  Request_parse(request, read_buf, (size_t)read_bytes);

  if(request->state.error_code) {
    DBG_REQ(request, "Parse error");
    request->current_chunk = PyString_FromString(
      http_error_messages[request->state.error_code]);
    assert(request->iterator == NULL);
  }
  else if(request->state.parse_finished) {
    WsgiServer *server = wsgi_server_from_fd(_client_fd_map, request->client_fd);
    if(!wsgi_call_application(request, server->application)) {
      assert(PyErr_Occurred());
      PyErr_Print();
      assert(!request->state.chunked_response);
      Py_XCLEAR(request->iterator);
      request->current_chunk = PyString_FromString(
        http_error_messages[HTTP_SERVER_ERROR]);
    }
  } else {
    /* Wait for more data */
    goto out;
  }

  ev_io_stop(mainloop, &request->ev_watcher);
  ev_io_init(&request->ev_watcher, &ev_io_on_write,
             request->client_fd, EV_WRITE);
  ev_io_start(mainloop, &request->ev_watcher);

out:
  GIL_UNLOCK(0);
  return;
}

/* XXX too much gotos */
static void
ev_io_on_write(struct ev_loop* mainloop, ev_io* watcher, const int events)
{
  Request* request = REQUEST_FROM_WATCHER(watcher);

  GIL_LOCK(0);

  if(request->state.use_sendfile) {
    /* sendfile */
    if(request->current_chunk) {
      /* current_chunk contains the HTTP headers */
      if(send_chunk(request))
        goto out;
      assert(!request->current_chunk_p);
      /* abuse current_chunk_p to store the file fd */
      request->current_chunk_p = PyObject_AsFileDescriptor(request->iterable);
      goto out;
    }
    if(do_sendfile(request))
      goto out;
  } else {
    /* iterable */
    if(send_chunk(request))
      goto out;

    if(request->iterator) {
      PyObject* next_chunk;
      next_chunk = wsgi_iterable_get_next_chunk(request);
      if(next_chunk) {
        if(request->state.chunked_response) {
          request->current_chunk = wrap_http_chunk_cruft_around(next_chunk);
          Py_DECREF(next_chunk);
        } else {
          request->current_chunk = next_chunk;
        }
        assert(request->current_chunk_p == 0);
        goto out;
      } else {
        if(PyErr_Occurred()) {
          PyErr_Print();
          /* We can't do anything graceful here because at least one
           * chunk is already sent... just close the connection */
          DBG_REQ(request, "Exception in iterator, can not recover");
          ev_io_stop(mainloop, &request->ev_watcher);
          // XXX _run_fd_callback("close_callback", request->client_fd);
          close(request->client_fd);
          Request_free(request);
          goto out;
        }
        Py_CLEAR(request->iterator);
      }
    }

    if(request->state.chunked_response) {
      /* We have to send a terminating empty chunk + \r\n */
      request->current_chunk = PyString_FromString("0\r\n\r\n");
      assert(request->current_chunk_p == 0);
      request->state.chunked_response = false;
      goto out;
    }
  }

  ev_io_stop(mainloop, &request->ev_watcher);
  if(request->state.keep_alive) {
    DBG_REQ(request, "done, keep-alive");
    Request_clean(request);
    Request_reset(request);
    ev_io_init(&request->ev_watcher, &ev_io_on_read,
               request->client_fd, EV_READ);
    ev_io_start(mainloop, &request->ev_watcher);
  } else {
    DBG_REQ(request, "done, close");
    // XXX _run_fd_callback("close_callback", request->client_fd);
    close(request->client_fd);
    Request_free(request);
  }

out:
  GIL_UNLOCK(0);
}

static bool
send_chunk(Request* request)
{
  Py_ssize_t bytes_sent;

  assert(request->current_chunk != NULL);
  assert(!(request->current_chunk_p == PyString_GET_SIZE(request->current_chunk)
         && PyString_GET_SIZE(request->current_chunk) != 0));

  bytes_sent = write(
    request->client_fd,
    PyString_AS_STRING(request->current_chunk) + request->current_chunk_p,
    PyString_GET_SIZE(request->current_chunk) - request->current_chunk_p
  );

  if(bytes_sent == -1)
    return handle_nonzero_errno(request);

  request->current_chunk_p += bytes_sent;
  if(request->current_chunk_p == PyString_GET_SIZE(request->current_chunk)) {
    Py_CLEAR(request->current_chunk);
    request->current_chunk_p = 0;
    return false;
  }
  return true;
}

#define SENDFILE_CHUNK_SIZE 16*1024

static bool
do_sendfile(Request* request)
{
  Py_ssize_t bytes_sent = sendfile(
    request->client_fd,
    request->current_chunk_p, /* current_chunk_p stores the file fd */
    NULL, SENDFILE_CHUNK_SIZE
  );
  if(bytes_sent == -1)
    return handle_nonzero_errno(request);
  return bytes_sent != 0;
}

static bool
handle_nonzero_errno(Request* request)
{
  if(errno == EAGAIN || errno == EWOULDBLOCK) {
    /* Try again later */
    return true;
  } else {
    /* Serious transmission failure. Hang up. */
    fprintf(stderr, "Client %d hit errno %d\n", request->client_fd, errno);
    Py_XDECREF(request->current_chunk);
    Py_XCLEAR(request->iterator);
    request->state.keep_alive = false;
    return false;
  }
}

bool _init_server(void)
{
    WsgiServer_Type.tp_new = PyType_GenericNew;
    WsgiServer_Type.tp_init = (initproc) WsgiServer_init;
    WsgiServer_Type.tp_flags |= Py_TPFLAGS_DEFAULT;
    WsgiServer_Type.tp_members = WsgiServer_members;
    WsgiServer_Type.tp_methods = WsgiServer_methods;

    _server_fd_map = PyDict_New();
    if (_server_fd_map == NULL)
      return false;
    _client_fd_map = PyDict_New();
    if (_client_fd_map == NULL)
      return false;
    return true;
}
