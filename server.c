#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include "server.h"

#define ASSERT(expr)                                      \
 do {                                                     \
  if (!(expr)) {                                          \
    fprintf(stderr,                                       \
      "Assertion failed in %s on line %d: %s\n",          \
      __FILE__,                                           \
      __LINE__,                                           \
      #expr);                                             \
    fflush(stderr);                                       \
    abort();                                              \
  }                                                       \
 } while (0)

#define FATAL(msg)                                        \
  do {                                                    \
    fprintf(stderr,                                       \
      "Fatal error in %s on line %d: %s\n",               \
      __FILE__,                                           \
      __LINE__,                                           \
      msg);                                               \
    fflush(stderr);                                       \
    abort();                                              \
  } while (0)

static uv_loop_t* loop;
static http_parser_settings parser_settings;

KHASH_MAP_INIT_STR(mime_type, const char*)
static khash_t(mime_type)* mime_type;

#if 0
#include <sys/time.h>
void
btime(int f) {
  static struct timeval s;
  struct timeval e;
  if (f == 0) {
    puts("start");
    gettimeofday(&s, NULL);
  } else {
    puts("end");
    gettimeofday(&e, NULL);
    double startTime = s.tv_sec + (double)(s.tv_usec * 1e-6);
    double endTime = e.tv_sec + (double)(e.tv_usec * 1e-6);
    printf("%f\n", endTime - startTime);
  }
}
#endif

static void on_write(uv_write_t*, int);
static void on_header_write(uv_write_t*, int);
static void on_read(uv_stream_t*, ssize_t, const uv_buf_t*);
static void on_close(uv_handle_t*);
static void on_server_close(uv_handle_t*);
static void on_connection(uv_stream_t*, int);
static void on_alloc(uv_handle_t*, size_t, uv_buf_t*);
static void on_request_complete(http_request*);
static void on_fs_read(uv_fs_t*);
static void response_error(uv_handle_t*, int, const char*, const char*);

int on_message_begin(http_parser* _);
int on_headers_complete(http_parser* _);
int on_message_complete(http_parser* _);
int on_url(http_parser* _, const char* at, size_t length);
int on_header_field(http_parser* _, const char* at, size_t length);
int on_header_value(http_parser* _, const char* at, size_t length);
int on_body(http_parser* _, const char* at, size_t length);

static void
destroy_request(http_request* request, int close_handle) {
  kl_destroy(header, request->headers);
  if (request->payload) free(request->payload);
  if (request->url) free(request->url);
  if (request->path) free(request->path);
  if (close_handle) {
    if (request->handle) uv_close((uv_handle_t*) request->handle, NULL);
  }
  free(request);
}

static void
destroy_response(http_response* response, int close_handle) {
  if (response->pbuf) free(response->pbuf);
  if (response->handle->data) free(response->handle->data);
  if (response->open_req) {
    if (close_handle) {
      if (response->request) destroy_request(response->request, 1);
    }
    uv_fs_t close_req;
    uv_fs_close(loop, &close_req, response->open_req->result, NULL);
    free(response->open_req);
  }
  free(response);
}

static void
on_write(uv_write_t* req, int status) {
  http_response* response = (http_response*) req->data;
  free(req);

  if (response->request->offset < response->request->size) {
    int r = uv_fs_read(loop, response->read_req, response->open_req->result, &response->buf, 1, response->request->offset, on_fs_read);
    if (r) {
      response_error(response->handle, 500, "Internal Server Error", NULL);
      destroy_request(response->request, 1);
    }
    return;
  }
  if (!response->keep_alive) {
    destroy_response(response, 1);
    return;
  }

  if (response->request) destroy_request(response->request, 0);
  uv_stream_t* stream = (uv_stream_t*) response->handle;
  destroy_response(response, 0);

  http_parser* parser = malloc(sizeof(http_parser));
  if (parser == NULL) {
    fprintf(stderr, "Allocate error\n");
    uv_close((uv_handle_t*) stream, NULL);
    return;
  }
  http_parser_init(parser, HTTP_REQUEST);

  http_request* request = malloc(sizeof(http_request));
  if (request == NULL) {
    fprintf(stderr, "Allocate error\n");
    uv_close((uv_handle_t*) stream, NULL);
    return;
  }
  parser->data = request;

  memset(request, 0, sizeof(http_request));
  request->handle = (uv_handle_t*) stream;
  request->on_request_complete = on_request_complete;
  stream->data = parser;
}

static void
on_shutdown(uv_shutdown_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, on_close);
  free(req);
}

static void
on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  if (nread < 0) {
    if (buf->base) {
      free(buf->base);
    }
    /*
    uv_shutdown_t* shutdown_req = (uv_shutdown_t*) malloc(sizeof(uv_shutdown_t));
    if (shutdown_req == NULL) {
      fprintf(stderr, "Allocate error\n");
      uv_close((uv_handle_t*) stream, on_close);
      return;
    }
    uv_shutdown(shutdown_req, stream, on_shutdown);
    */
    uv_close((uv_handle_t*) stream, NULL);
    return;
  }

  if (nread == 0) {
    free(buf->base);
    return;
  }

  http_parser* parser = (http_parser*) stream->data;
  size_t nparsed = http_parser_execute(parser, &parser_settings, buf->base, nread);
  free(buf->base);
}

static void on_close(uv_handle_t* peer) {
  free(peer);
}

static void
on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

static void
response_error(uv_handle_t* handle, int status_code, const char* status, const char* message) {
  char bufline[1024];
  puts("error");
  sprintf(bufline,
      "HTTP/1.0 %d %s\r\n"
      "Content-Length: 10\r\n"
      "Content-Type: text/plain; charset=UTF-8;\r\n"
      "\r\n"
      "%s", status_code, status, message ? message : status);
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  int r = uv_write(write_req, (uv_stream_t*) handle, &buf, 1, NULL);
  free(write_req);
  if (r) {
    fprintf(stderr, "Write error %s\n", uv_err_name(r));
  }
}

static void
on_fs_read(uv_fs_t *req) {
  http_response* response = (http_response*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "File read error %s\n", uv_err_name(result));
    response_error(response->handle, 500, "Internal Server Error", NULL);
    destroy_response(response, 1);
    return;
  } else if (result == 0) {
    destroy_response(response, 1);
    return;
  }

  uv_write_t *write_req = (uv_write_t *) malloc(sizeof(uv_write_t));
  if (write_req == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }
  write_req->data = response;
  uv_buf_t buf = uv_buf_init(response->pbuf, result);
  int r = uv_write(write_req, (uv_stream_t*) response->handle, &buf, 1, on_write);
  if (r) {
    destroy_response(response, 1);
    return;
  }
  response->request->offset += result;
}

static void
on_fs_open(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "Open error %s\n", uv_err_name(result));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    return;
  }

  http_response* response = malloc(sizeof(http_response));
  memset(response, 0, sizeof(http_response));
  response->open_req = req;
  response->request = request;
  response->handle = request->handle;
  response->pbuf = malloc(8192);
  response->buf = uv_buf_init(response->pbuf, 8192);
  response->keep_alive = request->keep_alive;
  int offset = -1;
  uv_fs_t* read_req = malloc(sizeof(uv_fs_t));
  read_req->data = response;
  response->read_req = read_req;
  int r = uv_fs_read(loop, read_req, result, &response->buf, 1, offset, on_fs_read);
  if (r) {
    response_error(request->handle, 500, "Internal Server Error", NULL);
    destroy_response(response, 1);
  }
  /*
  r = uv_fs_read(loop, read_req, result, &response->buf, 1, offset, NULL);
  if (r) {
    response_error(request->handle, 500, "Internal Server Error", NULL);
    destroy_response(response, 1);
    return;
  }
  */
}

static void
on_header_write(uv_write_t* req, int status) {
  http_request* request = (http_request*) req->data;
  free(req);

  uv_fs_t* open_req = malloc(sizeof(uv_fs_t));
  open_req->data = request;
  int r = uv_fs_open(loop, open_req, request->path, O_RDONLY, S_IREAD, on_fs_open);
  if (r) {
    fprintf(stderr, "Open error %s\n", uv_err_name(r));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    free(open_req);
  }
}

static void
on_fs_stat(uv_fs_t* req) {
  http_request* request = (http_request*) req->data;
  int result = req->result;

  uv_fs_req_cleanup(req);
  if (result < 0) {
    fprintf(stderr, "Stat error %s\n", uv_err_name(result));
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
    return;
  }
  request->size = req->statbuf.st_size;

  const char* ctype = "application/octet-stream";
  char* dot = request->path;
  char* ptr = dot;
  while (dot) {
    ptr = dot;
    dot = strchr(dot + 1, '.');
  }
  khint_t k = kh_get(mime_type, mime_type, ptr);
  if (k != kh_end(mime_type)) {
    ctype = kh_value(mime_type, k);
  }

  char bufline[1024];
  snprintf(bufline,
      sizeof(bufline),
      "HTTP/1.1 200 OK\r\n"
      "Content-Length: %" PRId64 "\r\n"
      "Content-Type: %s\r\n"
      "Connection: %s\r\n"
      "\r\n",
      request->size,
      ctype,
      (request->keep_alive ? "keep-alive" : "close"));
  uv_write_t* write_req = malloc(sizeof(uv_write_t));
  uv_buf_t buf = uv_buf_init(bufline, strlen(bufline));
  write_req->data = request;
  int r = uv_write(write_req, (uv_stream_t*) request->handle, &buf, 1, on_header_write);
  if (r) {
    free(write_req);
    fprintf(stderr, "Write error %s\n", uv_err_name(r));
    return;
  }
}

static void
on_request_complete(http_request* request) {
  kliter_t(header)* it;
  for (it = kl_begin(request->headers); it != kl_end(request->headers); it = kl_next(it)) {
    header_elem elem = kl_val(it);
    if (!strcasecmp(elem.key, "connection")) {
      if (!strcasecmp(elem.value, "keep-alive")) {
        request->keep_alive = 1;
      }
    }
  }

  if (!(request->url_handle.field_set & (1<<UF_PATH))) {
    request->path = strdup("./public/index.html");
  } else {
    char path[PATH_MAX];
    char* ptr = request->url + request->url_handle.field_data[UF_PATH].off;
    int len = request->url_handle.field_data[UF_PATH].len;
    snprintf(path, sizeof(path), "./public%.*s", len, ptr);
    if (strstr(path, "quit")) exit(0);
    if (*(ptr + len - 1) == '/') {
      strcat(path, "index.html");
    }
    request->path = strdup(path);
  }

  uv_fs_t* stat_req = malloc(sizeof(uv_fs_t));
  stat_req->data = request;
  //printf("%s\n", request->path);
  int r = uv_fs_stat(loop, stat_req, request->path, on_fs_stat);
  if (r) {
    fprintf(stderr, "Stat error %s\n", uv_err_name(r));
    free(stat_req);
    uv_close((uv_handle_t*) request->handle, NULL);
    response_error(request->handle, 404, "Not Found", NULL);
    destroy_request(request, 1);
  }
}

static void
on_connection(uv_stream_t* server, int status) {
  uv_stream_t* stream;
  int r;

  if (status != 0) {
    fprintf(stderr, "Connect error %s\n", uv_err_name(status));
    return;
  }

  stream = malloc(sizeof(uv_tcp_t));
  if (stream == NULL) {
    fprintf(stderr, "Allocate error\n");
    return;
  }

  r = uv_tcp_init(loop, (uv_tcp_t*) stream);
  if (r) {
    fprintf(stderr, "Socket creation error %s\n", uv_err_name(r));
    return;
  }

  r = uv_accept(server, stream);
  if (r) {
    fprintf(stderr, "Accept error %s\n", uv_err_name(r));
    return;
  }

  http_parser* parser = malloc(sizeof(http_parser));
  if (parser == NULL) {
    fprintf(stderr, "Allocate error\n");
    uv_close((uv_handle_t*) stream, NULL);
    return;
  }
  http_parser_init(parser, HTTP_REQUEST);

  http_request* request = malloc(sizeof(http_request));
  if (request == NULL) {
    fprintf(stderr, "Allocate error\n");
    uv_close((uv_handle_t*) stream, NULL);
    return;
  }
  parser->data = request;

  memset(request, 0, sizeof(http_request));
  request->handle = (uv_handle_t*) stream;
  request->on_request_complete = on_request_complete;
  stream->data = parser;

  r = uv_read_start(stream, on_alloc, on_read);
  if (r) {
    fprintf(stderr, "Read error %s\n", uv_err_name(r));
    uv_close((uv_handle_t*) stream, NULL);
  }
}

static void
usage(const char* app) {
  fprintf(stderr, "usage: %s [OPTIONS]\n", app);
  fprintf(stderr, "    -a ADDR: specify address\n");
  fprintf(stderr, "    -p PORT: specify port number\n");
  exit(1);
}

static void
add_mime_type(const char* ext, const char* value) {
  int hr;
  khint_t k = kh_put(mime_type, mime_type, ext, &hr);
  kh_value(mime_type, k) = value;
}

int
main(int argc, char* argv[]) {
  char* ipaddr = "0.0.0.0";
  int port = 7000;
  int i;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-a")) {
      if (i < argc-1) {
        ipaddr = argv[++i];
      } else {
        usage(argv[0]);
      }
    }
    if (!strcmp(argv[i], "-p")) {
      if (i < argc-1) {
        char* e = NULL;
        port = strtol(argv[++i], &e, 10);
        if (e && *e) usage(argv[0]);
      } else {
        usage(argv[0]);
      }
    }
  }

  loop = uv_default_loop();

  struct sockaddr_in addr;
  int r;

  memset(&parser_settings, 0, sizeof(parser_settings));
  parser_settings.on_message_begin = on_message_begin;
  parser_settings.on_url = on_url;
  parser_settings.on_header_field = on_header_field;
  parser_settings.on_header_value = on_header_value;
  parser_settings.on_headers_complete = on_headers_complete;
  parser_settings.on_body = on_body;
  parser_settings.on_message_complete = on_message_complete;

  mime_type = kh_init(mime_type);
  /*
  add_mime_type(".jpg", "image/jpeg");
  add_mime_type(".png", "image/png");
  add_mime_type(".gif", "image/gif");
  add_mime_type(".html", "text/html");
  add_mime_type(".txt", "text/plain");
  */

  r = uv_ip4_addr(ipaddr, 7000, &addr);
  if (r) {
    fprintf(stderr, "Address error %s\n", uv_err_name(r));
    return 1;
  }

  uv_tcp_t server;
  r = uv_tcp_init(loop, &server);
  if (r) {
    fprintf(stderr, "Socket creation error %s\n", uv_err_name(r));
    return 1;
  }

  r = uv_tcp_bind(&server, (const struct sockaddr*) &addr, 0);
  if (r) {
    fprintf(stderr, "Bind error %s\n", uv_err_name(r));
    return 1;
  }

  r = uv_listen((uv_stream_t*)&server, SOMAXCONN, on_connection);
  if (r) {
    fprintf(stderr, "Listen error %s\n", uv_err_name(r));
    return 1;
  }

  return uv_run(loop, UV_RUN_DEFAULT);
}

/* vim:set et: */
