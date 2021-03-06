// balboa
// Copyright (c) 2018, 2019 DCSO GmbH

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <engine.h>
#include <protocol.h>
#include <trace.h>

#define ENGINE_MPACK_TREE_MEMCAP (1024 * 100)
#define ENGINE_MPACK_TREE_NODES_LIMIT (1024)
#define ENGINE_POLL_READ_TIMEOUT (60)
#define ENGINE_POLL_WRITE_TIMEOUT (30)

static atomic_int blb_engine_stop = ATOMIC_VAR_INIT(0);
static atomic_int blb_conn_cnt = ATOMIC_VAR_INIT(0);

void blb_engine_request_stop() {
  atomic_fetch_add(&blb_engine_stop, 1);
}

static int blb_engine_poll_stop() {
  return (atomic_load(&blb_engine_stop));
}

static void blb_thread_cnt_incr() {
  (void)atomic_fetch_add(&blb_conn_cnt, 1);
}

static void blb_thread_cnt_decr() {
  (void)atomic_fetch_sub(&blb_conn_cnt, 1);
}

static int blb_thread_cnt_get() {
  return (atomic_load(&blb_conn_cnt));
}

static inline int blb_engine_poll_write(int fd, int seconds) {
timeout_retry:
  if(blb_engine_poll_stop() > 0) {
    L(log_notice("engine stop detected"));
    return (-1);
  }
  fd_set fds;
  struct timeval to;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  to.tv_sec = seconds;
  to.tv_usec = 0;
  int rc = select(fd + 1, NULL, &fds, NULL, &to);
  if(rc == 0) {
    X(log_debug("select() timeout => retry polling"));
    goto timeout_retry;
  } else if(rc < 0) {
    X(log_debug("select() failed `%s`", strerror(errno)));
    return (-1);
  }
  return (0);
}

static inline int blb_engine_poll_read(int fd, int seconds) {
timeout_retry:
  if(blb_engine_poll_stop() > 0) {
    L(log_notice("engine stop detected"));
    return (-1);
  }
  fd_set fds;
  struct timeval to;
  FD_ZERO(&fds);
  FD_SET(fd, &fds);
  to.tv_sec = seconds;
  to.tv_usec = 0;
  int rc = select(fd + 1, &fds, NULL, NULL, &to);
  if(rc == 0) {
    X(log_debug("select() timeout => retry polling"));
    goto timeout_retry;
  } else if(rc < 0) {
    X(log_debug("select() failed `%s`", strerror(errno)));
    return (-1);
  }
  return (0);
}

int blb_conn_write_all(conn_t* th, char* _p, size_t _p_sz) {
  int wr_ok = blb_engine_poll_write(th->fd, ENGINE_POLL_WRITE_TIMEOUT);
  if(wr_ok != 0) {
    L(log_error("blb_engine_poll_write() failed"));
    return (-1);
  }
  char* p = _p;
  ssize_t r = _p_sz;
  while(r > 0) {
    ssize_t rc = write(th->fd, p, r);
    if(rc < 0) {
      L(log_error("write() failed error `%s`", strerror(errno)));
      return (-1);
    } else if(rc == 0 && errno == EINTR) {
      continue;
    }
    r -= rc;
    p += rc;
  }
  blb_engine_stats_add(th->engine, ENGINE_STATS_BYTES_SEND, _p_sz);
  return (0);
}

int blb_conn_query_stream_start_response(conn_t* th) {
  if(blb_engine_poll_stop() > 0) {
    L(log_notice("thread <%04lx> engine stop detected", th->thread));
    return (-1);
  }

  ssize_t used = blb_protocol_encode_stream_start_response(
      th->scrtch, ENGINE_CONN_SCRTCH_SZ);
  if(used <= 0) {
    L(log_error("blb_protocol_encode_stream_start_response() failed"));
    return (-1);
  }

  return (blb_conn_write_all(th, th->scrtch, used));
}

int blb_conn_dump_entry(conn_t* th, const protocol_entry_t* entry) {
  T(log_debug("dump stream push entry"));
  T(blb_protocol_log_entry(entry));

  if(blb_engine_poll_stop() > 0) {
    L(log_notice("thread <%04lx> engine stop detected", th->thread));
    return (-1);
  }

  ssize_t used =
      blb_protocol_encode_dump_entry(entry, th->scrtch, ENGINE_CONN_SCRTCH_SZ);
  if(used <= 0) {
    L(log_error("blb_protocol_encode_dump_entry() failed"));
    return (-1);
  }

  return (blb_conn_write_all(th, th->scrtch, used));
}

int blb_conn_query_stream_push_response(
    conn_t* th, const protocol_entry_t* entry) {
  T(log_debug("query stream push entry"));
  T(blb_protocol_log_entry(entry));

  if(blb_engine_poll_stop() > 0) {
    L(log_notice("thread <%04lx> engine stop detected", th->thread));
    return (-1);
  }

  ssize_t used = blb_protocol_encode_stream_entry(
      entry, th->scrtch, ENGINE_CONN_SCRTCH_SZ);
  if(used <= 0) {
    L(log_error("blb_protocol_encode_stream_entry() failed"));
    return (-1);
  }

  return (blb_conn_write_all(th, th->scrtch, used));
}

int blb_conn_query_stream_end_response(conn_t* th) {
  if(blb_engine_poll_stop() > 0) {
    L(log_error("thread <%04lx> engine stop detected", th->thread));
    return (-1);
  }

  ssize_t used = blb_protocol_encode_stream_end_response(
      th->scrtch, ENGINE_CONN_SCRTCH_SZ);
  if(used <= 0) {
    L(log_error("blb_protocol_encode_stream_end_response() failed"));
    return (-1);
  }

  X(log_debug(
      "blb_protocol_encode_stream_end_response() returned `%zd`", used));

  return (blb_conn_write_all(th, th->scrtch, used));
}

static conn_t* blb_engine_conn_new(engine_t* e, int fd) {
  conn_t* th = blb_new(conn_t);
  if(th == NULL) { return (NULL); }
  th->usr_ctx = NULL;
  th->usr_ctx_sz = 0;
  th->db = NULL;
  if(e->db != NULL) {
    db_t* db = blb_dbi_conn_init(th, e->db);
    if(db == NULL) {
      blb_free(th);
      return (NULL);
    }
    th->db = db;
  }
  th->engine = e;
  th->fd = fd;
  return (th);
}

void blb_engine_conn_teardown(conn_t* th) {
  if(th->db != NULL) { blb_dbi_conn_deinit(th, th->db); }
  close(th->fd);
  blb_free(th);
}

static ssize_t blb_conn_read_stream_cb(void* usr, char* p, size_t p_sz) {
  conn_t* th = usr;
  int fd_ok = blb_engine_poll_read(th->fd, ENGINE_POLL_READ_TIMEOUT);
  if(fd_ok != 0) {
    L(log_error("engine poll read failed"));
    return (0);
  }
  ssize_t rc = read(th->fd, p, p_sz);
  if(rc < 0) {
    blb_engine_stats_bump(th->engine, ENGINE_STATS_ERRORS);
    L(log_error("read() failed `%s`", strerror(errno)));
    return (-1);
  } else if(rc == 0) {
    X(log_debug("read() eof"));
    return (0);
  }
  blb_engine_stats_add(th->engine, ENGINE_STATS_BYTES_RECV, rc);
  return (rc);
}

static inline int blb_engine_conn_consume_backup(
    conn_t* th, const protocol_backup_request_t* backup) {
  blb_dbi_backup(th, backup);
  return (0);
}

static inline int blb_engine_conn_consume_dump(
    conn_t* th, const protocol_dump_request_t* dump) {
  blb_dbi_dump(th, dump);
  return (0);
}

static inline int blb_engine_conn_consume_query(
    conn_t* th, const protocol_query_request_t* query) {
  int query_ok = blb_dbi_query(th, query);
  if(query_ok != 0) {
    L(log_error("blb_dbi_query() failed"));
    return (-1);
  }
  return (0);
}

static inline int blb_engine_conn_consume_input(
    conn_t* th, const protocol_input_request_t* input) {
  T(blb_protocol_log_entry(&input->entry));
  int input_ok = blb_dbi_input(th, input);
  if(input_ok != 0) {
    L(log_error("blb_dbi_input() failed"));
    return (-1);
  }
  return (0);
}

static inline int blb_engine_conn_consume(conn_t* th, protocol_message_t* msg) {
  switch(msg->ty) {
  case PROTOCOL_INPUT_REQUEST:
    blb_engine_stats_bump(th->engine, ENGINE_STATS_INPUTS);
    return (blb_engine_conn_consume_input(th, &msg->u.input));
  case PROTOCOL_BACKUP_REQUEST:
    blb_engine_stats_bump(th->engine, ENGINE_STATS_BACKUPS);
    return (blb_engine_conn_consume_backup(th, &msg->u.backup));
  case PROTOCOL_DUMP_REQUEST:
    blb_engine_stats_bump(th->engine, ENGINE_STATS_DUMPS);
    return (blb_engine_conn_consume_dump(th, &msg->u.dump));
  case PROTOCOL_QUERY_REQUEST:
    blb_engine_stats_bump(th->engine, ENGINE_STATS_QUERIES);
    return (blb_engine_conn_consume_query(th, &msg->u.query));
  default:
    blb_engine_stats_bump(th->engine, ENGINE_STATS_ERRORS);
    L(log_debug("invalid message type `%d`", msg->ty));
    return (-1);
  }
}

protocol_stream_t* blb_engine_stream_new(conn_t* c) {
  protocol_stream_t* stream = blb_protocol_stream_new(
      c,
      blb_conn_read_stream_cb,
      ENGINE_MPACK_TREE_MEMCAP,
      ENGINE_MPACK_TREE_NODES_LIMIT);
  return (stream);
}

static void* blb_engine_conn_fn(void* usr) {
  ASSERT(usr != NULL);
  conn_t* th = usr;
  blb_thread_cnt_incr();
  T(log_info("new thread is <%04lx>", th->thread));

  protocol_stream_t* stream = blb_engine_stream_new(th);
  if(stream == NULL) {
    L(log_error("unalbe to create protocol decode stream"));
    goto thread_exit;
  }

  protocol_message_t msg;
  while(1) {
    if(blb_engine_poll_stop() > 0) {
      L(log_notice("thread <%04lx> engine stop detected", th->thread));
      goto thread_exit;
    }
    int rc = blb_protocol_stream_decode(stream, &msg);
    if(rc != 0) {
      if(rc == -1) {
        X(log_debug("blb_protocol_stream_decode() eof"));
      } else {
        L(log_error("blb_protocol_stream_decode() failed"));
      }
      goto thread_exit;
    }
    int th_rc = blb_engine_conn_consume(th, &msg);
    if(th_rc != 0) { goto thread_exit; }
    if(msg.ty == PROTOCOL_DUMP_REQUEST || msg.ty == PROTOCOL_BACKUP_REQUEST) {
      V(log_notice("closing client connection after dump or backup request"));
      goto thread_exit;
    }
  }

thread_exit:
  T(log_info("thread <%04lx> is shutting down", th->thread));
  if(stream != NULL) { blb_protocol_stream_teardown(stream); }
  blb_thread_cnt_decr();
  blb_engine_conn_teardown(th);
  return (NULL);
}

engine_t* blb_engine_server_new(const engine_config_t* config) {
  ASSERT(config->db != NULL);
  ASSERT(config->is_server == true);

  struct sockaddr_in __ipv4, *ipv4 = &__ipv4;
  int rc = inet_pton(AF_INET, config->host, &ipv4->sin_addr);
  ASSERT(rc >= 0);
  if(rc != 1) {
    errno = EINVAL;
    return (NULL);
  }
  ipv4->sin_family = AF_INET;
  ipv4->sin_port = htons((uint16_t)config->port);
  int fd = socket(ipv4->sin_family, SOCK_STREAM, 0);
  if(fd < 0) {
    L(log_error("socket() failed with `%s`", strerror(errno)));
    return (NULL);
  }
  int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  int bind_rc = bind(fd, ipv4, sizeof(struct sockaddr_in));
  if(bind_rc < 0) {
    L(log_error("bind() failed with `%s`", strerror(errno)));
    close(fd);
    return (NULL);
  }
  int listen_rc = listen(fd, SOMAXCONN);
  if(listen_rc < 0) {
    L(log_error("listen() failed with `%s`", strerror(errno)));
    close(fd);
    return (NULL);
  }

  engine_t* e = blb_new(engine_t);
  if(e == NULL) {
    close(fd);
    return (NULL);
  }

  e->conn_throttle_limit = config->conn_throttle_limit;
  e->enable_signal_consumer = config->enable_signal_consumer;
  e->enable_stats_reporter = config->enable_stats_reporter;
  e->db = config->db;
  e->listen_fd = fd;
  e->stats.interval = 10;
  for(int i = 0; i < ENGINE_STATS_N; i++) {
    atomic_store(&e->stats.counters[i], 0);
  }

  V(log_info(
      "listening on host `%s` port `%d` fd `%d`",
      config->host,
      config->port,
      fd));

  return (e);
}

static int blb_engine_client_connect(const char* host, int port) {
  struct sockaddr_in addr;
  int addr_ok = inet_pton(AF_INET, host, &addr.sin_addr);
  if(addr_ok != 1) { return (-1); }
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  int fd = socket(addr.sin_family, SOCK_STREAM, 0);
  if(fd < 0) { return (-1); }
  int rc = connect(fd, &addr, sizeof(struct sockaddr_in));
  if(rc < 0) {
    L(log_error("connect() failed with `%s`", strerror(errno)));
    close(fd);
    return (-1);
  }
  return (fd);
}

conn_t* blb_engine_client_new(const engine_config_t* config) {
  ASSERT(config->db == NULL);
  ASSERT(config->is_server == false);
  int fd = blb_engine_client_connect(config->host, config->port);
  if(fd < 0) { return (NULL); }

  engine_t* e = blb_new(engine_t);
  if(e == NULL) {
    close(fd);
    return (NULL);
  }
  e->db = NULL;

  conn_t* c = blb_engine_conn_new(e, fd);
  if(c == NULL) {
    L(log_error("blb_engine_conn_new() failed"));
    blb_free(e);
    return (NULL);
  }

  return (c);
}

static void* blb_engine_signal_consume(void* usr) {
  (void)usr;
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, SIGQUIT);
  sigaddset(&s, SIGUSR1);
  sigaddset(&s, SIGUSR2);
  sigaddset(&s, SIGINT);
  sigaddset(&s, SIGPIPE);
  sigaddset(&s, SIGTERM);
  V(log_info("signal consumer thread started"));
  while(1) {
    int sig = 0;
    int rc = sigwait(&s, &sig);
    V(log_debug("sigwait() returned `%d` (signal `%d`)", rc, sig));
    if(rc != 0) { continue; }
    L(log_notice("received signal `%d`", sig));
    switch(sig) {
    case SIGINT:
    case SIGTERM:
    case SIGQUIT:
      L(log_warn("requesting engine stop due to received signal"));
      blb_engine_request_stop();
      break;
    default: L(log_warn("ignoring signal")); break;
    }
  }
  return (NULL);
}

static inline unsigned long long blb_stats_slurp(
    engine_stats_t* s, enum engine_stats_counter_t c) {
  if(c < 0 || c >= ENGINE_STATS_N) { return (ULLONG_MAX); }
  unsigned long long x = atomic_load(&s->counters[c]);
  atomic_store(&s->counters[c], 0);
  return (x);
}

static void* blb_engine_stats_report(void* usr) {
  V(log_info("engine stats reporter thread started"));
  engine_t* e = usr;
  blb_thread_cnt_incr();
  pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t c = PTHREAD_COND_INITIALIZER;
  clock_gettime(CLOCK_REALTIME, &e->stats.last);
  while(1) {
    if(blb_engine_poll_stop() > 0) {
      L(log_notice("engine stats stop detected"));
      blb_thread_cnt_decr();
      return (NULL);
    }
    (void)pthread_mutex_lock(&m);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += e->stats.interval;
    (void)pthread_cond_timedwait(&c, &m, &ts);
    clock_gettime(CLOCK_REALTIME, &ts);
    long delta_t = ts.tv_sec - e->stats.last.tv_sec;
    L(log_notice(
        "delta_t `%ld` q `%llu` i `%llu` e `%llu` s `%llu` r `%llu` c `%llu`",
        delta_t,
        blb_stats_slurp(&e->stats, ENGINE_STATS_QUERIES),
        blb_stats_slurp(&e->stats, ENGINE_STATS_INPUTS),
        blb_stats_slurp(&e->stats, ENGINE_STATS_ERRORS),
        blb_stats_slurp(&e->stats, ENGINE_STATS_BYTES_SEND),
        blb_stats_slurp(&e->stats, ENGINE_STATS_BYTES_RECV),
        blb_stats_slurp(&e->stats, ENGINE_STATS_CONNECTIONS)));
    e->stats.last = ts;
    (void)pthread_mutex_unlock(&m);
  }
  blb_thread_cnt_decr();
  return (NULL);
}

void blb_engine_spawn_signal_consumer(engine_t* e) {
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, SIGQUIT);
  sigaddset(&s, SIGUSR1);
  sigaddset(&s, SIGUSR2);
  sigaddset(&s, SIGINT);
  sigaddset(&s, SIGPIPE);
  sigaddset(&s, SIGTERM);
  int rc = pthread_sigmask(SIG_BLOCK, &s, NULL);
  if(rc != 0) { L(log_error("pthread_sigmask() failed `%d`", rc)); }
  pthread_create(&e->signal_consumer, NULL, blb_engine_signal_consume, e);
}

void blb_engine_spawn_stats_reporter(engine_t* e) {
  pthread_create(&e->stats_reporter, NULL, blb_engine_stats_report, e);
}

void blb_engine_run(engine_t* e) {
  struct sockaddr_in __addr, *addr = &__addr;
  socklen_t addrlen = sizeof(struct sockaddr_in);

  if(e->enable_signal_consumer) { blb_engine_spawn_signal_consumer(e); }

  if(e->enable_stats_reporter) { blb_engine_spawn_stats_reporter(e); }

  pthread_attr_t __attr;
  pthread_attr_init(&__attr);
  pthread_attr_setdetachstate(&__attr, PTHREAD_CREATE_DETACHED);

  fd_set fds;
  struct timeval to;
  while(1) {
  timeout_retry:
    if(blb_engine_poll_stop() > 0) {
      L(log_notice("engine stop detected"));
      goto teardown;
    }
    if(blb_thread_cnt_get() >= e->conn_throttle_limit) {
      blb_engine_sleep(1);
      L(log_warn("thread throttle reached"));
      goto timeout_retry;
    }
    FD_ZERO(&fds);
    FD_SET(e->listen_fd, &fds);
    to.tv_sec = 5;
    to.tv_usec = 0;
    int rc = select(e->listen_fd + 1, &fds, NULL, NULL, &to);
    if(rc == 0) {
      X(log_debug("select() timeout"));
      goto timeout_retry;
    } else if(rc < 0) {
      L(log_error("select() failed `%s`", strerror(errno)));
      goto teardown;
    }
    socket_t fd = accept(e->listen_fd, (struct sockaddr*)addr, &addrlen);
    if(fd < 0) {
      L(log_error("accept() failed: `%s`", strerror(errno)));
      blb_engine_request_stop();
      goto teardown;
    }
    conn_t* th = blb_engine_conn_new(e, fd);
    if(th == NULL) {
      L(log_error("blb_engine_conn_new() failed"));
      blb_engine_request_stop();
      goto teardown;
    }
    blb_engine_stats_bump(th->engine, ENGINE_STATS_CONNECTIONS);
    (void)pthread_create(&th->thread, &__attr, blb_engine_conn_fn, (void*)th);
  }

teardown:

  pthread_attr_destroy(&__attr);

  while(blb_thread_cnt_get() > 0) {
    L(log_warn("waiting for `%d` thread(s) to finish", blb_thread_cnt_get()));
    blb_engine_sleep(2);
  }

  L(log_notice("all threads finished"));
}

void blb_engine_teardown(engine_t* e) {
  blb_free(e);
}
