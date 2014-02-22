#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/wait.h>

#include "credis/credis.h"
#include "fcgi_config.h"
#include "fcgiapp.h"
#include <json/json.h>
#include "debug.h"
#include "utils.h"
#include "libdio.h"

#define MAXCLIENTS 32
#define THREADBUFSIZE 2048

typedef struct dm_prm_s
{
  int dlevel;
  uint16_t sleeptimer;
  struct in_addr redisaddr;
  uint16_t redisport;
  uint16_t fcgiport;
  uint16_t cliport;
  struct in_addr cli_iface;
  struct in_addr fcgi_iface;
  uint16_t maxths;
  uint16_t minths;
  union {
   uint8_t foreground:1;
  };
} dm_prm_t;

typedef struct dm_th_pipe_s
{
  pthread_mutex_t mtx;
  int fd[2];
} dm_th_pipe_t;

typedef struct dm_state_s
{

} dm_state_t;

struct dm_thpool_s;

typedef struct dm_thread_s
{
  pthread_t tid;
  uint8_t *buf;
  uint8_t stop;
  uint8_t stopped;
  struct dm_thpool_s *pool;
  void *data;
} dm_thread_t;

typedef struct dm_fcgi_thprm_s
{
  int fd;
} dm_fcgi_thprm_t;

typedef struct dm_fcgi_thdata_s
{
  dm_fcgi_thprm_t p;
  char *buf;
  FCGX_Request req;
  int fd;
} dm_fcgi_thdata_t;

typedef struct dm_cli_thprm_s
{
  int fd;
} dm_cli_thprm_t;

typedef struct dm_cli_thdata_s
{
  dm_cli_thprm_t p;
  uint8_t *buf;
  int fd;
} dm_cli_thdata_t;

typedef struct dm_thpool_s
{
  char *name;
  pthread_mutex_t mtx;
  dm_thread_t **th;  
  pthread_mutex_t accept_mtx;
  void *(*create_f)(void);
  void (*free_f)(void *);
  void *(*th_fun) (void *);
  void *data;
  uint16_t cnt;
  uint16_t maxcnt;
  uint16_t peakcnt;
  uint16_t type;
  dm_th_pipe_t *thpipe;
} dm_thpool_t;

typedef struct dm_vars_s
{
  dm_prm_t prm;
  dm_state_t st;
  REDIS db;
  int fcgi_fd;  
  int cli_fd;
  dbg_desc_t dbg;

  dm_thpool_t clipool;
  dm_thpool_t fcgipool;
  dm_th_pipe_t thpipe;

} dm_vars_t;

void *dm_cli_thread(void *ptr);
void *dm_fcgi_thread(void *ptr);
void sig_handler(int signum);
void sigchld_handler(int signum);
int dm_thpool_stop(dm_thpool_t *ta);

dm_vars_t dm_vars;

#define ERR(format, ...) debug_print(&dm_vars.dbg, 0, "ERR: "format"\n", ##__VA_ARGS__)
#define MSG(format, ...) debug_print(&dm_vars.dbg, 0, format"\n", ##__VA_ARGS__)
#define LOG(format, ...) MSG(format, ##__VA_ARGS__)

#ifdef DM_DEBUG
#define DBGL(level, format, ...) debug_print(&dm_vars.dbg, 1+level, "DBG: %s: "format"\n", __FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(0, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif

void dm_init_thpool(dm_thpool_t *ta, void* (*th_fun)(void*), uint8_t type, dm_th_pipe_t *p, uint16_t max)
{
  ta->cnt=0;
  ta->th = malloc(sizeof(dm_thread_t)*max);
  ta->maxcnt = max;
  pthread_mutex_init(&ta->mtx,0);
  ta->th_fun=th_fun;
  ta->type=type;
  ta->thpipe=p;
}

void dm_free_thpool(dm_thpool_t *p)
{
  free(p->th);
}

void dm_init_th_pipe(dm_th_pipe_t *t)
{
  pthread_mutex_init(&t->mtx, 0);
  pipe(t->fd);
}

void *create_fcgi_thdata()
{
  dm_fcgi_thdata_t *d;
  d = malloc(sizeof(dm_fcgi_thdata_t));
  memset(d, 0, sizeof(dm_fcgi_thdata_t));
  d->buf = malloc(THREADBUFSIZE);
  d->p.fd = -1;
  return d;
}

void free_fcgi_thdata(void *d)
{
  free(((dm_fcgi_thdata_t *)d)->buf);
  free(d);
}

void *create_cli_thdata()
{
  dm_cli_thdata_t *d;
  d = malloc(sizeof(dm_fcgi_thdata_t));
  memset(d, 0, sizeof(dm_fcgi_thdata_t));
  d->buf = malloc(THREADBUFSIZE);
  d->p.fd = -1;
  return d;
}

void free_cli_thdata(void *d)
{
  free(((dm_cli_thdata_t *)d)->buf);
  free(d);
}

int dm_init_vars(dm_vars_t *v)
{
  memset(v, 0, sizeof(dm_vars_t));
  v->prm.dlevel=1;
  v->prm.fcgiport=6666;
  v->prm.cliport=7777;
  v->prm.sleeptimer=1000;
  v->prm.minths = 1;
  v->prm.maxths = MAXCLIENTS;

  dm_init_th_pipe(&v->thpipe);

  return 0;
}

int dm_init(dm_vars_t *v)
{
  uint16_t i;
  char *s;
  char str[32];  

  if (debug_init(&v->dbg, v->prm.dlevel))
    return -1;
  libdio_setlog(&v->dbg);

  DBG("Initilization...");

  dm_init_thpool(&v->clipool, dm_cli_thread, 0, &v->thpipe, v->prm.maxths);
  v->clipool.name = "cli";
  v->clipool.create_f = create_cli_thdata;
  v->clipool.free_f = free_cli_thdata;
  dm_init_thpool(&v->fcgipool, dm_fcgi_thread, 1, &v->thpipe, v->prm.maxths);
  v->fcgipool.name = "fcgi";
  v->fcgipool.create_f = create_fcgi_thdata;
  v->fcgipool.free_f = free_fcgi_thdata;

  if (FCGX_Init()) {
    ERR("Can't init fcgi library!");
    return -1;
  }

  DBGL(2,"Create fcgi socket %u...", v->prm.fcgiport);

  if (v->prm.fcgi_iface.s_addr) {
    sprintf(str, "%s:", inet_ntoa(v->prm.fcgi_iface));
    sprintf(&str[strlen(str)], "%u",
          v->prm.fcgiport);
  } else
    sprintf(str, "*:%u", v->prm.fcgiport);

  v->fcgi_fd = FCGX_OpenSocket(str, 5);
  if (v->fcgi_fd<0) {
    ERR("Can't create fcgi socket %s!", str);
    return -1;
  }

  s=0;
  if (v->prm.redisaddr.s_addr)
    s=inet_ntoa(v->prm.redisaddr);
  DBGL(2, "Connecting to REDIS '%s':%u...", s?s:"127.0.0.1", v->prm.redisport);
  v->db = credis_connect(0, v->prm.redisport, 20000);
  if (!v->db) {
    ERR("Can't connect to redis!");
    return -1;
  }  

  /* open cli socket */
  struct sockaddr_in sa;

  DBGL(2,"Create cli socket %u...", v->prm.cliport);

  v->cli_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (v->cli_fd<0) {
    ERR("Can't create socket '%s'\n", strerror(errno));
    return -1;
  }
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(v->prm.cli_iface.s_addr);
  sa.sin_port = htons(v->prm.cliport);

  if (bind(v->cli_fd, (struct sockaddr*)&sa, sizeof(sa))) {
    ERR("Can't bind socket '%s'", strerror(errno));
    return -1;
  }

  if (listen(v->cli_fd, 5)) {
    ERR("Listen error '%s'", strerror(errno));
    return -1;
  }

  libdio_signal(SIGINT, sig_handler);
  libdio_signal(SIGTERM, sig_handler);
  libdio_signal(SIGHUP, sig_handler);
  libdio_signal(SIGALRM, sig_handler);
  libdio_signal(SIGCHLD, sigchld_handler);


  for (i=0; i<v->prm.minths; ++i) {
    dm_thpool_add(&v->clipool, &v->cli_fd, sizeof(v->cli_fd));
    dm_thpool_add(&v->fcgipool, &v->fcgi_fd, sizeof(v->fcgi_fd));
  }

  if (!v->prm.foreground) {
    DBGL(2, "Become a daemon...");
    daemon(0, 0);
  }

  return 0;
}

int dm_cleanup(dm_vars_t *v)
{
  dm_thpool_stop(&v->clipool);
  dm_thpool_stop(&v->fcgipool);

  dm_free_thpool(&v->clipool);
  dm_free_thpool(&v->fcgipool);

  if (v->fcgi_fd>=0) {
    shutdown(v->fcgi_fd, SHUT_RDWR);
    close(v->fcgi_fd);
  }
  if (v->cli_fd>=0) {
    shutdown(v->cli_fd, SHUT_RDWR);
    close(v->cli_fd);
  }

  debug_free(&v->dbg);

  return 0;
}

void sig_handler(int signum)
{
  LOG("Signal %i received", signum);

  if (signum == SIGHUP) {
    LOG("Internal states initialized");
    return;
  }

  if ((signum == SIGINT) || (signum == SIGTERM)) {
    dm_cleanup(&dm_vars);
    LOG("Exiting...");
    exit(0);
  }
}

void sigchld_handler(int signum)
{
  int sts;
  pid_t pid;

  if (SIGCHLD!=signum)
    return;

  while ((pid = waitpid(-1, &sts, WNOHANG))>0) {


    if (WIFEXITED(sts)) {
      LOG("Child %i exited, status=%d", pid, WEXITSTATUS(sts));
    } else if (WIFSIGNALED(sts)) {
      LOG("Child %i killed by signal %d", pid, WTERMSIG(sts));
    } else if (WIFSTOPPED(sts)) {
      LOG("Child %i stopped by signal %d", pid, WSTOPSIG(sts));
    } else if (WIFCONTINUED(sts)) {
      LOG("Child %i continued", pid);
    }
 }
}

static struct option loptions[] = {
  {"help", 0, 0, 0},
  {"rdhost", 1, 0, 0},
  {"rdport", 1, 0, 0},
  {"fcgi-port, 1, 0, 0"},
  {"cli-port, 1, 0, 0"},
  {"fcgi-iface, 1, 0, 0"},
  {"cli-iface, 1, 0, 0"}
};

void dm_print_help(void)
{
  printf("commetd \n\n");
  printf("-h - help\n"
         "-d <level> - debug level\n"
         "-l <file> - logfile\n"
         "-f - do not daemonize\n");
}

int dm_handle_long_opt(dm_prm_t *p, int idx)
{
  switch (idx)
  {
    case 0:
      dm_print_help();
      return -1;
    case 1:
      if (!inet_aton(optarg, &p->redisaddr))
        printf("Wrong REDIS address!\n");
    break;
    case 2:
      if (ut_s2n10(optarg, &p->redisport))
        printf("Wrong port!\n");
    break;
    case 3:
      if (ut_s2n10(optarg, &p->fcgiport))
        printf("Wrong port!\n");
    break;
    case 4:
      if (ut_s2n10(optarg, &p->cliport))
        printf("Wrong port!\n");
    break;
    case 5:
      if (!inet_aton(optarg, &p->fcgi_iface))
        printf("Wrong FCGI iface!\n");
    break;
    case 6:
     if (!inet_aton(optarg, &p->cli_iface))
        printf("Wrong CLI iface!\n");
    break;
  }
  return 0;
}

int dm_handle_args(dm_prm_t * p, int argc, char **argv)
{
  int c;
  int optidx;

  while (1) {
    c = getopt_long(argc, argv, "hfl:d:", loptions,  &optidx);
    if (c==-1)
      break;
    switch (c) {
      case 0:
        return dm_handle_long_opt(p, optidx);
      break;
      case 'l':
      break;
      case 'd':
       p->dlevel =  strtoul(optarg, 0, 10);
      break;
      case 'f':
       p->foreground = 1;
      break;
      case 'h':
       dm_print_help();
       return -1;
      break;
   }
 }

 return 0;
}


int dm_exit(dm_vars_t *v, int code)
{
  dm_cleanup(v);
  if (code) {
    LOG("Exit: with error %i", code);
  } else {
    LOG("Exit: success");
  }
  exit(code);
}


int dm_thpool_del(dm_thpool_t *p, pthread_t tid)
{
  int i,j;
  DBGL(3, "");  

  pthread_mutex_lock(&p->mtx);

  for (i=0; i<p->cnt;++i)
    if (p->th[i]->tid==tid)
      break;

  if (i>=p->cnt) {
    pthread_mutex_unlock(&p->mtx);
    return -1;
  }

  /* free thread data */
  if (p->free_f)
    p->free_f(p->th[i]->data);

  free(p->th[i]);

  for (j=i; j<(p->cnt-1);++j)
    p->th[j]=p->th[j+1];
  p->cnt--;

  pthread_mutex_unlock(&p->mtx);

  return 0;
}


int dm_thpool_add(dm_thpool_t *pool, void *data, size_t dsize)
{
  dm_thread_t *th=0;
  int newfd=-1;
  pthread_t tid;

  if (pool->cnt>=pool->maxcnt) {
    ERR("too many threads in pool (%u)!", pool->cnt);
    goto error;
  }

  th = malloc(sizeof(dm_thread_t));
  memset(th, 0, sizeof(dm_thread_t));
  DBGL(3, "th: %p", th);

  th->pool = pool;

  if (pool->create_f) {
    /* create thread local data */
    th->data = pool->create_f();
    /* copy thread input parametes */
    memcpy(th->data, data, dsize);
  }
  DBGL(3, "thd: %p", th->data);

  pthread_mutex_lock(&pool->mtx);
  pool->th[pool->cnt]=th;
  pool->cnt++;
  if (pool->cnt > pool->peakcnt)
    pool->peakcnt = pool->cnt;
  pthread_mutex_unlock(&pool->mtx);


  if (pthread_create(&tid, 0, pool->th_fun, th)) {
    ERR("thread create error");
    return -1;
  }

  th->tid=tid;

  return 0;

error:

  if (newfd>=0) {
    shutdown(newfd, 3);
    close(newfd);
  }

  if (th)
    free(th);

  return -1;
}

void dm_send_thread_exit_signal(dm_thread_t *th)
{
  dm_th_pipe_t *p;

  if (!th->pool)
   return;

  p=th->pool->thpipe;

  if (!p)
    return;

  pthread_mutex_lock(&th->pool->mtx);

  write(p->fd[1], &th, sizeof(th));

  pthread_mutex_unlock(&th->pool->mtx);
  th->stopped=1;
}

void dm_json_print(json_object *jobj)
{
  enum json_type type;
  char vals[64];
  json_object_object_foreach(jobj, key, val) {

    /*Passing through every array element*/
    type = json_object_get_type(val);
    switch (type) {
      case json_type_boolean:
        snprintf(vals, sizeof(vals), "%s", json_object_get_boolean(val)? "true": "false");
      break;
      case json_type_double:
        snprintf(vals, sizeof(vals), "%lf", json_object_get_double(val));
      break;
      case json_type_int:
        snprintf(vals, sizeof(vals), "%d", json_object_get_int(val));
      break;
      case json_type_string:
        snprintf(vals, sizeof(vals), "%s", json_object_get_string(val));
      break;
      case json_type_object:
        snprintf(vals, sizeof(vals), "object");
        /*jobj = json_object_object_get(jobj, key);
        json_parse(jobj);*/
      break;
      case json_type_array:
        snprintf(vals, sizeof(vals), "array");
        /*printf("type: json_type_array, ");
        json_parse_array(jobj, key);*/
      break;
      default:;
    }
    DBGL(2, "key: %s type: %i val: %s",key,type, vals);
  }

}

char wrongJSON[]="{ \"answer\":\"wrong JSON\" }";

int dm_process_json_cmd(uint8_t *buf)
{
  libdio_msg_str_cmd_r_t *rshdr = (libdio_msg_str_cmd_r_t *)buf;
  json_object *jobj=0;

  jobj = json_tokener_parse(((libdio_msg_str_cmd_t*)buf)->cmd);

  if (!jobj) {
    ERR("Wrong json!");
    LIBDIO_FILLJSONRESPONSE(rshdr, wrongJSON, 0xF1);
    return -1;
  }

  dm_json_print(jobj);

  json_object_put(jobj);

  LIBDIO_FILLJSONRESPONSE(rshdr, "OK", 0x0);

  return 0;
}

int dm_cli_cml_info_handler(int argc, char **argv, void *data)
{
   libdio_msg_str_cmd_r_t *hdr = (libdio_msg_str_cmd_r_t *)(((dm_thread_t*)data)->buf);


   DBG("");
   sprintf(hdr->response, "cli threads %u/%u/%u\n fcgi threads %u/%u/%u\n",
           dm_vars.clipool.cnt,
           dm_vars.clipool.peakcnt,
           dm_vars.clipool.maxcnt,
           dm_vars.fcgipool.cnt,
           dm_vars.fcgipool.peakcnt,
           dm_vars.fcgipool.maxcnt
           );
   LIBDIO_FILLRESPONSE((&hdr->rhdr), strlen(hdr->response)+1, LIBDIO_MSG_STR_CMD, 0);
   return 0;
}

libdio_str_cmd_tbl_t cli_cml_table[] = {
    {"info", dm_cli_cml_info_handler}
};

void *dm_cli_thread(void *ptr)
{
  dm_thread_t *th = (dm_thread_t *)ptr;
  dm_cli_thdata_t *thd = (dm_cli_thdata_t *)th->data;
  libdio_msg_hdr_t *hdr;
  libdio_msg_response_hdr_t *rhdr;
  libdio_msg_str_cmd_r_t *rshdr;    
  uint16_t code;
  int r;

  DBG("started %p", th);

  while (!th->stop) {

    if (th->pool)
      pthread_mutex_lock(&th->pool->accept_mtx);
    thd->fd = accept(thd->p.fd, NULL, 0);
    if (th->pool)
      pthread_mutex_unlock(&th->pool->accept_mtx);

    if (thd->fd<0) {
      ERR("accept error %s", strerror(errno));
      goto exit;
    }

    hdr = (libdio_msg_hdr_t *)thd->buf;
    rhdr = (libdio_msg_response_hdr_t *)thd->buf;
    rshdr = (libdio_msg_str_cmd_r_t*)thd->buf;

    while (!th->stop) {

      r = libdio_waitfd(thd->fd, 500, 'r');

      if (r<0)
        break;

      if (r>0) {
        if (th->stop)
          break;
        continue;
      }

      if (recv(thd->fd, thd->buf, 1, MSG_PEEK)!=1) {
        DBGL(1, "connection closed %p", th);
        break;
      }

      if (libdio_read_message(thd->fd, thd->buf))
        continue;

      code = ntohs(hdr->code);
      DBGL(1,"msg: len: %u code: %04X", ntohs(hdr->len), code);
      char unknown[]="Unknown command";

      switch (code) {
        case LIBDIO_MSG_STR_CMD:
          DBGL(1,"cm: %s", ((libdio_msg_str_cmd_t*)hdr)->cmd);
          if (libdio_process_str_cmd(thd->buf, cli_cml_table, 1, th)==-2);
            LIBDIO_FILLSTRRESPONSE(rshdr, unknown, 0xF0);
        break;
        case LIBDIO_MSG_JSON_CMD:
          DBGL(1,"json: %s", ((libdio_msg_str_cmd_t*)hdr)->cmd);
          dm_process_json_cmd(thd->buf);
        break;
        default:
          LIBDIO_FILLRESPONSE(rhdr, 1, code, 0xFF);
      }

      libdio_write_message(thd->fd, thd->buf);

    }

    if (thd->fd>=0) {
      shutdown(thd->fd, SHUT_RDWR);
      close(thd->fd);
    }


  } // while

  if (th->stop)
    DBG("stopped %p", th);

exit:

  dm_send_thread_exit_signal(th);

  return &dm_vars;
}

void dm_print_fcgi_req_params(FCGX_Request *r)
{
  char **p;
  for (p = r->envp; *p; ++p)
    DBGL(3, "PRM: %s", *p);

}

int dm_process_fcgi(dm_vars_t *v, dm_fcgi_thdata_t *thd)
{
  FCGX_Request *r = &thd->req;

  if (FCGX_InitRequest(r, thd->p.fd, 0))  {
    ERR("Can't init fcgi req!");
    return -1;
  }

  while (1) {

   pthread_mutex_lock(&v->fcgipool.accept_mtx);
   if (FCGX_Accept_r(r)) {
     ERR("FCGX_Accept_r");
     goto exit;
   }

   LOG("request accepted");
   pthread_mutex_unlock(&v->fcgipool.accept_mtx);

   #ifdef DM_DEBUG
   dm_print_fcgi_req_params(r);
   #endif

   FCGX_Finish_r(&thd->req);

  }

exit:

  return 0;
}

void *dm_fcgi_thread(void *ptr)
{
  dm_thread_t *th = (dm_thread_t *)ptr;
  dm_fcgi_thdata_t *thd = (dm_fcgi_thdata_t *)th->data;
  DBG("new fcgi started %lu", pthread_self());

  dm_process_fcgi(&dm_vars, th->data);

  dm_send_thread_exit_signal(th);
  shutdown(thd->p.fd, SHUT_RDWR);
  close(thd->p.fd);
  return &dm_vars;
}


int dm_handle_thread_exited(dm_vars_t *v)
{
  dm_thread_t *th;
  void *p;
  /*
  read(v->thpipe.fd[0], &tid, sizeof(pthread_t));
  read(v->thpipe.fd[0], &type, 1);
  */
  read(v->thpipe.fd[0], &th, sizeof(th));

  DBG("thread %p", th);
  if (!th) {
    DBG("th==0");
    return -1;
  }

  pthread_join(th->tid, &p);

  if (th->pool) {
    DBG("%s thread exited %p", th->pool->name, p);
    dm_thpool_del(th->pool, th->tid);

  }
  return 0;
}

void dm_thread_abort(dm_thread_t *th)
{
  DBG("thread aborted %lu", th->tid);
  pthread_cancel(th->tid);
  pthread_join(th->tid, 0);
  dm_send_thread_exit_signal(th);
}

int dm_thread_stop(dm_thread_t *th)
{
  int timer = 10;
  th->stop = 1;
  while (timer--)
    if (!th->stopped)
      usleep(50*1000);
    else break;

  if (timer)
    return 0;

  dm_thread_abort(th);

  return 0;
}

int dm_thpool_stop(dm_thpool_t *ta)
{
  int i,cnt;
  uint16_t timer=10;

  pthread_mutex_lock(&ta->mtx);

  for (i=0;i<ta->cnt;i++)
    ta->th[i]->stop=1;

  while (timer--) {
    cnt=0;
    for (i=0;i<ta->cnt;i++)
      if (!ta->th[i]->stopped)
        cnt++;

    if (cnt)
      usleep(50000);

    else break;
  }

  if (timer)
    return 0;

  DBG("%u threads not stopped normaly!", cnt);

  for (i=0;i<ta->cnt;i++)
    if (!ta->th[i]->stopped)
      dm_thread_abort(ta->th[i]);

  pthread_mutex_unlock(&ta->mtx);

  return 0;
}

int dm_handle_timeout()
{
  /*DBG("\n")*/
  return 0;
}

int main(int argc, char **argv)
{

  dm_init_vars(&dm_vars);

  if (dm_handle_args(&dm_vars.prm, argc, argv))
      dm_exit(&dm_vars, 0);

  if (dm_init(&dm_vars))
    dm_exit(&dm_vars, 1);

  int fds[3];
  int fdcount;
  fd_set set, rset;

  FD_ZERO(&set);
  FD_SET(dm_vars.cli_fd, &set);
  FD_SET(dm_vars.fcgi_fd, &set);
  FD_SET(dm_vars.thpipe.fd[0], &set);

  fds[0]=dm_vars.thpipe.fd[0];
  /*fds[0]=dm_vars.cli_fd;
  fds[1]=dm_vars.fcgi_fd;
  fds[2]=dm_vars.thpipe.fd[0];*/
  fdcount=1;

  struct timeval tv;

  while (1) {

    /* wait for event */
    tv.tv_sec=0;
    tv.tv_usec=1000*dm_vars.prm.sleeptimer;

    rset = set;
    int maxfd,s ;

    /*maxfd = (dm_vars.cli_fd > dm_vars.fcgi_fd)?dm_vars.cli_fd:dm_vars.fcgi_fd;*/
    maxfd = dm_vars.thpipe.fd[0];
    maxfd += 1;
    s = select(maxfd, &rset, 0, 0, &tv);

    if (s<0) {
      if (errno != EINTR) {
        ERR("select: %s", strerror(errno));
        dm_exit(&dm_vars, 1);
      }
      continue;
    }

    if (s==0) {
      dm_handle_timeout(&dm_vars);
      continue;
    }

    int i, fd;
    for (i = 0, fd = fds[0]; i <= fdcount; ++i, fd=fds[i]) {

      if (!FD_ISSET(fd, &rset))
        continue;

      if (fd==dm_vars.cli_fd) {
        /* new connection */
        DBGL(3,"ADD CLI client %i!", fd);
        dm_thpool_add(&dm_vars.clipool, &fd, sizeof(fd));
        continue;
      };

      if (fd==dm_vars.fcgi_fd) {
        /* new connection */
        DBGL(3,"ADD FCGI client %i!", fd);
        dm_thpool_add(&dm_vars.fcgipool, &fd, sizeof(fd));
        continue;
      };

      if (fd==dm_vars.thpipe.fd[0]) {
        /* thread exited */
        dm_handle_thread_exited(&dm_vars);
        continue;
      };

    }

  }

  return 0;

}

