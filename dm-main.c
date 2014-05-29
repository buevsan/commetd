#include <stdio.h>
#include <string.h>
#include <ctype.h>

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
#include <time.h>
#include <inttypes.h>

#include <hiredis/hiredis.h>
#include <fcgi_config.h>
#include <fcgiapp.h>
#include <json-c/json.h>
#include "json-parser.h"
#include "debug.h"
#include "utils.h"
#include "libdio.h"
#include "db.h"

#define MAXEVENTS_PERRECEIVER 200
#define MAXTHREADS 1024
#define THREADBUFSIZE 20000
#define THREADSTACKSIZE 40

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

  uint16_t minfcgi_ths;
  uint16_t mincli_ths;

  uint16_t maxths;
  uint16_t minths;
  uint16_t wait_event_timer;
  uint32_t event_expire_timer;
  uint32_t user_expire_timer;
  uint16_t redisdb;
  uint16_t get_event_sleep;
  char bdprefix[33];
  char logfilename[128];
  char cookiename[33];
  struct {
   uint8_t foreground:1;
   uint8_t fcgi_http_debug:1;
   uint8_t no_check_iface:1;
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
  uint8_t stop;
  uint8_t stopped;
  void *stack;
  struct dm_thpool_s *pool;
  void *data;
} dm_thread_t;

typedef struct dm_fcgi_thprm_s
{
  int fd;
  db_t *db;
  uint8_t fcgi_http_debug;
} dm_fcgi_thprm_t;

typedef struct dm_fcgi_thdata_s
{
  dm_fcgi_thprm_t p;
  uint8_t *buf;
  uint64_t *etimes;
  int fd;
  FCGX_Request req;
} dm_fcgi_thdata_t;

typedef struct dm_cli_thprm_s
{
  int fd;
  db_t *db;
} dm_cli_thprm_t;

typedef struct dm_cli_thdata_s
{
  dm_cli_thprm_t p;
  uint8_t *buf;
  uint64_t *etimes;
  int fd;
} dm_cli_thdata_t;

typedef struct dm_thpool_s
{
  char *name;
  pthread_mutex_t mtx;
  dm_thread_t **th;  
  pthread_mutex_t accept_mtx;
  pthread_attr_t th_attr;
  uint16_t stacksize;
  void *(*create_f)(void);
  void (*free_f)(void *);
  void *(*th_fun) (void *);
  void *data;
  uint16_t cnt;
  uint16_t runcnt;
  uint16_t maxcnt;
  uint16_t peakcnt;
  uint16_t type;
  dm_th_pipe_t *thpipe;
} dm_thpool_t;

typedef struct dm_vars_s
{
  dm_prm_t prm;
  dm_state_t st;
  int fcgi_fd;  
  int cli_fd;
  dbg_desc_t dbg;
  db_t db;

  dm_thpool_t clipool;
  dm_thpool_t fcgipool;
  dm_th_pipe_t thpipe;

} dm_vars_t;


typedef struct {
  uint8_t *buf;
  uint64_t *etimes;
  db_t *db;
  uint8_t *stop;
  uint32_t wait_event_timer;
  char *cookiename;
} dm_business_prm_t;

void *dm_cli_thread(void *ptr);
void *dm_fcgi_thread(void *ptr);
void sig_handler(int signum);
void sigchld_handler(int signum);
int dm_thpool_stop(dm_thpool_t *ta);
int dm_thpool_add(dm_thpool_t *pool, void *data, size_t dsize);

dm_vars_t dm_vars;

#define ERR(format, ...) debug_print(&dm_vars.dbg, 0, "ERR: "format"\n", ##__VA_ARGS__)
#define MSG(format, ...) debug_print(&dm_vars.dbg, 0, format"\n", ##__VA_ARGS__)
#define LOG(format, ...) MSG("Commetd: "format, ##__VA_ARGS__)

#ifdef DM_DEBUG
#define DBGL(level, format, ...) debug_print(&dm_vars.dbg, 1+level, "DBG: %lu: %s: "format"\n", pthread_self(),__FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(1, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif

#define ERR_OK 0
#define ERR_WRONG_SYNTAX  1
#define ERR_WRONG_MANDATORY_ITEM 2
#define ERR_DATABASE  3
#define ERR_UNKNOWN_COMMAND  4
#define ERR_TIMEOUT  5
#define ERR_ACCESS   6
#define ERR_INTERNAL  7

#define ANS_OK 200
#define ANS_ERROR  500
#define ANS_TIMEOUT  408
#define ANS_ACCESS   403

typedef struct {
  int code;
  int anscode;
  char *msg;
} err_table_item_t;

err_table_item_t dm_err_table[] = {
  { ERR_OK, ANS_OK, "OK" },
  { ERR_WRONG_SYNTAX, ANS_ERROR, "Wrong syntax"},
  { ERR_WRONG_MANDATORY_ITEM, ANS_ERROR, "Wrong or absent mandatory item"},
  { ERR_DATABASE, ANS_ERROR, "Database operation failed"},
  { ERR_UNKNOWN_COMMAND, ANS_ERROR, "Unknown command"},
  { ERR_TIMEOUT, ANS_TIMEOUT, "Operation timeout"},
  { ERR_ACCESS, ANS_ACCESS, "Access violation"},
  { ERR_INTERNAL, ANS_ERROR, "Internal error"}
};

err_table_item_t *dm_get_err_msg(int code)
{
  uint8_t i;
  for (i=0; i<sizeof(dm_err_table)/sizeof(err_table_item_t);++i)
    if (dm_err_table[i].code == code)
      return &dm_err_table[i];
  return 0;
}

void dm_init_thpool(dm_thpool_t *ta, void* (*th_fun)(void*), uint8_t type, dm_th_pipe_t *p, uint16_t max)
{
  ta->cnt=0;
  ta->th = malloc(sizeof(dm_thread_t)*max);
  ta->maxcnt = max;
  pthread_mutex_init(&ta->mtx,0);
  pthread_mutex_init(&ta->accept_mtx, 0);
  pthread_attr_init(&ta->th_attr);
  ta->stacksize=THREADSTACKSIZE;
  ta->th_fun=th_fun;
  ta->type=type;
  ta->thpipe=p;  
}

void dm_free_thpool(dm_thpool_t *p)
{
  pthread_attr_destroy(&p->th_attr);
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
  d->etimes = malloc(sizeof(uint64_t)*MAXEVENTS_PERRECEIVER);
  d->p.fd = -1;
  return d;
}

void free_fcgi_thdata(void *d)
{
  free(((dm_fcgi_thdata_t *)d)->buf);
  free(((dm_fcgi_thdata_t *)d)->etimes);
  free(d);
}

void *create_cli_thdata()
{
  dm_cli_thdata_t *d;
  d = malloc(sizeof(dm_cli_thdata_t));
  memset(d, 0, sizeof(dm_cli_thdata_t));
  d->buf = malloc(THREADBUFSIZE);
  d->etimes = malloc(sizeof(uint64_t)*MAXEVENTS_PERRECEIVER);
  d->p.fd = -1;
  return d;
}

void free_cli_thdata(void *d)
{
  free(((dm_cli_thdata_t *)d)->buf);
  free(((dm_cli_thdata_t *)d)->etimes);
  free(d);
}

int dm_init_vars(dm_vars_t *v)
{
  memset(v, 0, sizeof(dm_vars_t));
  v->prm.dlevel=0;
  v->prm.fcgiport=6666;
  v->prm.cliport=7777;
  v->prm.sleeptimer=1000;
  v->prm.minths = 300;
  v->prm.maxths = MAXTHREADS;
  v->prm.redisdb = 3;
  v->prm.redisport = 6379;  
  v->prm.wait_event_timer = 5;
  v->prm.event_expire_timer = 60;
  v->prm.user_expire_timer = 86400;
  v->prm.get_event_sleep = 100;
  strncpy(v->prm.bdprefix, "prefix", sizeof(v->prm.bdprefix));
  strncpy(v->prm.logfilename, "/var/log/commetd.log", sizeof(v->prm.logfilename));
  strncpy(v->prm.cookiename, "PHPSESSID", sizeof(v->prm.cookiename));

  dm_init_th_pipe(&v->thpipe);

  return 0;
}

int dm_init(dm_vars_t *v)
{
  uint16_t i;
  char *s;
  char str[32];  
  char *log;

  log = 0;
  if (v->prm.logfilename[0])
   if (strcmp(v->prm.logfilename, "std"))
     log=v->prm.logfilename;

  if (debug_init(&v->dbg, v->prm.dlevel, log))
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
  libdio_setnonblock(v->fcgi_fd, 1);

  db_init(&v->db);
  db_setdbnum(&v->db, v->prm.redisdb);
  db_setlog(&v->db, &v->dbg);
  db_set_prefix(&v->db, v->prm.bdprefix);
  db_set_event_expire_timer(&v->db, v->prm.event_expire_timer);
  db_set_user_expire_timer(&v->db, v->prm.user_expire_timer);

  s=0;
  if (v->prm.redisaddr.s_addr)
    s=inet_ntoa(v->prm.redisaddr);
  DBGL(2, "Connecting to REDIS '%s':%u...", s?s:"127.0.0.1", v->prm.redisport);

  if (db_connect(&v->db, s?s:"127.0.0.1", v->prm.redisport))
    return -1;

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
  sa.sin_addr.s_addr = v->prm.cli_iface.s_addr;
  sa.sin_port = htons(v->prm.cliport);

  int yes =1;
  if ( setsockopt(v->cli_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1 ){
      ERR("Can't set socket options'%s'", strerror(errno));
      return -1;
  }

  if (bind(v->cli_fd, (struct sockaddr*)&sa, sizeof(sa))) {
    ERR("Can't bind socket '%s'", strerror(errno));
    return -1;
  }

  if (listen(v->cli_fd, 5)) {
    ERR("Listen error '%s'", strerror(errno));
    return -1;
  }
  libdio_setnonblock(v->cli_fd, 1);

  libdio_signal(SIGINT, sig_handler);
  libdio_signal(SIGTERM, sig_handler);
  libdio_signal(SIGHUP, sig_handler);
  libdio_signal(SIGALRM, sig_handler);
  libdio_signal(SIGCHLD, sigchld_handler);

  /* assign parameters to threads pools */
  dm_fcgi_thprm_t fcgi_prm;
  fcgi_prm.fd = v->fcgi_fd;
  fcgi_prm.db = &v->db;
  fcgi_prm.fcgi_http_debug = v->prm.fcgi_http_debug;

  dm_cli_thprm_t cli_prm;
  cli_prm.fd = v->cli_fd;
  cli_prm.db = &v->db;

  if (!v->prm.foreground) {
    DBGL(2, "Become a daemon...");
    daemon(0, 0);
  }

  /* start threads */
  DBG("clipool %s fcgipool %s",v->clipool.name, v->fcgipool.name);

  for (i=0; i<v->prm.minths; ++i) {      
    dm_thpool_add(&v->clipool, &cli_prm, sizeof(cli_prm));
    dm_thpool_add(&v->fcgipool, &fcgi_prm, sizeof(fcgi_prm));
  }


  return 0;
}

int dm_cleanup(dm_vars_t *v)
{
  DBG("");

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

  db_free(&v->db);

  debug_free(&v->dbg);

  return 0;
}

void sig_handler(int signum)
{
  LOG("Signal %i received", signum);

  if (signum == SIGHUP) {    
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
  {"rd-host", 1, 0, 0},
  {"rd-port", 1, 0, 0},
  {"fcgi-port", 1, 0, 0},
  {"cli-port", 1, 0, 0},
  {"fcgi-iface", 1, 0, 0},
  {"cli-iface", 1, 0, 0},
  {"rd-prefix", 1, 0, 0},
  {"event-expire", 1, 0, 0},
  {"event-wait", 1, 0, 0},
  {"log", 1, 0, 0},
  {"fcgi-http-debug", 0, 0, 0},
  {"no-check-iface", 0, 0, 0},
  {"rd-db", 1, 0, 0},
  {"cookie-name", 1, 0, 0},
  {0, 0, 0, 0}
};

static char * loptdesc[] = {
  "Print help ",
  "REDIS server hostname",
  "REDIS server port",
  "TCP port for FCGI access",
  "TCP port for CLI access",
  "IP interface for FCGI",
  "IP interface for CLI",
  "REDIS database key prefix",
  "REDIS key expire timer",
  "Event waiting timer",
  "Log file name",
  "Send HTTP debug page via FCGI without simple answer",
  "Do not check iface allowed for command",
  "Redis database index",
  "Cookie variable name"
};

void dm_print_help(void)
{
  uint8_t i;
  printf("\ncommetd daemon - by Alexander V. Buev (buevsan at yandex.ru)\n"
         "compiled at "__DATE__"\n\n");
  printf("-h - help\n"
         "-d <level> - debug level\n"
         "-s <num> - number of threads\n"
         "-f - do not daemonize\n\n");

  for (i=0; i<sizeof(loptions)/sizeof(struct option)-1;++i)
    printf("--%s %s- %s\n", loptions[i].name, (loptions[i].has_arg)?"<arg> ":"", loptdesc[i]);
  printf("\n");
}

int dm_handle_long_opt(dm_prm_t *p, int idx)
{
  switch (idx)
  {
    case 0:
      dm_print_help();
      return -1;
    case 1:
      if (!inet_aton(optarg, &p->redisaddr)) {
        printf("Wrong REDIS address!\n");
        return 1;
      }
    break;
    case 2:
      if (ut_s2n10(optarg, &p->redisport)) {
        printf("Wrong port!\n");
        return 1;
      }
    break;
    case 3:
      if (ut_s2n10(optarg, &p->fcgiport)) {
        printf("Wrong port!\n");
        return 1;
      }
    break;
    case 4:
      if (ut_s2n10(optarg, &p->cliport)) {
        printf("Wrong port!\n");
        return 1;
      }
    break;
    case 5:
      if (!inet_aton(optarg, &p->fcgi_iface)) {
        printf("Wrong FCGI iface!\n");
        return 1;
      }
    break;
    case 6:
      if (!inet_aton(optarg, &p->cli_iface)) {
        printf("Wrong CLI iface!\n");
        return 1;
      }
    break;
    case 7:
      strncpy(p->bdprefix, optarg, sizeof(p->bdprefix));
    break;
    case 8:
      if (ut_s2nl10(optarg, &p->event_expire_timer)) {
        printf("Wrong timer!\n");
        return 1;
      }
    break;
    case 9:
      if (ut_s2n10(optarg, &p->wait_event_timer)) {
        printf("Wrong timer!\n");
        return 1;
      }
    break;
    case 10:
      strncpy(p->logfilename, optarg, sizeof(p->logfilename));
    break;
    case 11:
      p->fcgi_http_debug=1;
    break;
    case 12:
      p->no_check_iface=1;
    break;
    case 13:
      if (ut_s2n10(optarg, &p->redisdb)) {
        printf("Wrong redis db index!\n");
        return 1;
      }
    break;
    case 14:
      strncpy(p->cookiename, optarg, sizeof(p->cookiename));
    break;
  }
  return 0;
}

int dm_handle_args(dm_prm_t * p, int argc, char **argv)
{
  int c;
  int optidx;

  while (1) {
    c = getopt_long(argc, argv, "hfd:s:m:", loptions,  &optidx);
    if (c==-1)
      break;
    switch (c) {
      case 0:
        if (dm_handle_long_opt(p, optidx))
          return 1;
      break;
      case 'd':
       p->dlevel =  strtoul(optarg, 0, 10);
      break;
      case 'f':
       p->foreground = 1;
      break;
      case 's':
        if (ut_s2n10(optarg, &p->minths)) {
          printf("Wrong threads number!\n");
          return 1;
        }
      break;
      case 'm':
        if (ut_s2n10(optarg, &p->maxths)) {
          printf("Wrong threads number!\n");
          return 1;
        }
      break;
      case '?':
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
  int newfd=-1, r;
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

  posix_memalign(&th->stack,1024, 1024*pool->stacksize);
  pthread_attr_setstack(&pool->th_attr, th->stack, pool->stacksize);

  DBGL(3, "thd: %p", th->data);

  pthread_mutex_lock(&pool->mtx);
  pool->th[pool->cnt]=th;
  pool->cnt++;
  if (pool->cnt > pool->peakcnt)
    pool->peakcnt = pool->cnt;
  pthread_mutex_unlock(&pool->mtx);

  r = pthread_create(&tid, &pool->th_attr, pool->th_fun, th);
  /*r = pthread_create(&tid, 0, pool->th_fun, th);*/
  if (r) {
    ERR("thread create error '%s'", strerror(r));
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


char wrongJSON[]="{ \"answer\":\"wrong JSON\" }";


void dm_json_print(char *objname, json_object *jobj)
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
        vals[0]=0;
      break;
      case json_type_array:
        vals[0]=0;
        /*printf("type: json_type_array, ");
        json_parse_array(jobj, key);*/
      break;
      default:;
    }
    DBGL(2, "'%s': key:'%s' val:'%s' type:%i", objname, key, vals, type);
    if (type == json_type_object)
      dm_json_print(key, json_object_object_get(jobj, key));
  }

}

typedef struct dm_json_obj_s {
  char *name;
  json_type type;
  struct dm_json_obj_s *next;
} dm_json_obj_t;

dm_json_obj_t gen_items[]= {
  { "cmd", json_type_string, 0 },
  { "interface", json_type_string, 0 },
  { 0, 0, 0 }
};

dm_json_obj_t create_user_items[]= {
  { "hash", json_type_string, 0 },
  { "receiver", json_type_string, 0 },
  { 0, 0, 0 }
};

dm_json_obj_t delete_user_items[]= {
  { "hash", json_type_string, 0 },
  { "receiver", json_type_string, 0 },
  { 0, 0, 0 }
};

dm_json_obj_t set_event_items[]= {
  { "receiver", json_type_string, 0 },
  { "event_type", json_type_string, 0 },
  { "edata", json_type_string, 0 },
  { 0, 0, 0 }
};

dm_json_obj_t get_event_items[]= {
  { "receiver", json_type_string, 0 },
  { 0, 0, 0 }
};

int dm_json_check(json_object *obj, dm_json_obj_t *table)
{
  int i=0;
  json_object *o;
  while (table[i].name) {
    o = json_getobj(obj, table[i].name, table[i].type);
    if (!o) {
      ERR("Wrong or absent '%s' json key", table[i].name);
      return 1;
    }
    if (table[i].next)
      if (dm_json_check(o, table[i].next))
        return 1;
    i++;
  }
  return 0;
}

json_object * dm_mk_jsonanswer_text(json_object *toobj, int code, char *text)
{    
  json_object * answer_o;

  if (toobj) answer_o=toobj;
  else answer_o=json_object_new_object();

  json_object * code_o = json_object_new_int(code);
  json_object_object_add(answer_o, "code", code_o);
  
  if ((text) && (text[0])) {
   json_object * text_o = json_object_new_string(text);
   json_object_object_add(answer_o, "text", text_o);
  } 
  return answer_o;
}

json_object * dm_mk_jsonanswer(int code)
{
  err_table_item_t *i = dm_get_err_msg(code);
  if (i)
    return dm_mk_jsonanswer_text(0, i->anscode, i->msg);
  else
    return dm_mk_jsonanswer_text(0, ANS_ERROR, "Unknown error");
}

void dm_add_jsonanswer(json_object *toobj, int code)
{
  err_table_item_t *i = dm_get_err_msg(code);
  if (i)
    dm_mk_jsonanswer_text(toobj, i->anscode, i->msg);
  else
    dm_mk_jsonanswer_text(toobj, ANS_ERROR, "Unknown error");
}


int dm_do_create_user(dm_business_prm_t *bp, json_object *req, json_object **ans)
{
  int r=0;
  const char *hash, *receiver;

  DBG("");

  if (dm_json_check(req, create_user_items)) {
    r=ERR_WRONG_MANDATORY_ITEM;
    goto exit;
  }

  hash = json_get_string(req, "hash");
  receiver = json_get_string(req, "receiver");

  if (db_create_user(bp->db, hash, receiver))
    r = ERR_DATABASE;

exit:

  (*ans)=dm_mk_jsonanswer(r);

  return r;
}

int dm_do_delete_user(dm_business_prm_t *bp, json_object *req, json_object **ans)
{
  int r=0;
  const char *hash, *receiver;

  DBG("");

  if (dm_json_check(req, delete_user_items)) {
    r=ERR_WRONG_MANDATORY_ITEM;
    goto exit;
  }

  hash = json_get_string(req, "hash");
  receiver = json_get_string(req, "receiver");

  if (db_del_user(bp->db, hash, receiver))
    r = ERR_DATABASE;

exit:


  (*ans)=dm_mk_jsonanswer(r);

  return r;
}

int dm_check_receiver(dm_business_prm_t *bp, const char *receiver)
{
  return db_get_user_hash(bp->db, receiver, 0, 0);
}

json_object *dm_mk_event_record(uint64_t etime, const char *event_type,
                                const char *receiver, const char *received, const char *edata)
{
  json_object *o = json_object_new_object();
  json_add_string_u64(o, "event_time", etime);
  json_add_string(o, "event_type", event_type);
  json_add_string(o, "receiver", receiver);
  json_add_string(o, "received", received);
  json_add_string(o, "edata", edata);
  return o;
}


void dm_getcurtime(uint64_t *t)
{
  struct timeval tv;
  memset(&tv, 0, sizeof(tv));
  gettimeofday(&tv, 0);
  (*t) = tv.tv_sec*1000+tv.tv_usec/1000;
}

int dm_do_getnow(dm_business_prm_t *bp, json_object *req, json_object **ans)
{
  uint64_t t;
  char times[32];
  json_object *obj;
  DBG("");

  dm_getcurtime(&t);
  snprintf(times, sizeof(times), "%"PRIu64, t);
  obj = json_new_string("now", times);
  dm_add_jsonanswer(obj, 0);

  (*ans)=obj;

  return 0;
}

int dm_do_set_event(dm_business_prm_t *bp, json_object *req, json_object **ans)
{
  int r=0;
  const char *receiver, *edata, *event_type, *iface;
  json_object *o=0;
  char key[64];
  uint64_t t;

  DBG("");

  dm_getcurtime(&t);

  if (dm_json_check(req, set_event_items)) {
    r=ERR_WRONG_MANDATORY_ITEM;
    goto exit;
  }

  event_type = json_get_string(req, "event_type");
  receiver = json_get_string(req, "receiver");
  edata = json_get_string(req, "edata");

  /* Check receiver only if from cli iface */
  iface = json_get_string(req, "interface");
  if (strcmp(iface, "cli"))
    if (dm_check_receiver(bp, receiver)) {
      ERR("Receiver '%s' not exist", receiver);
      r=ERR_ACCESS;
      goto exit;
    }

  o = dm_mk_event_record(t, event_type, receiver, "0", edata);
  if (!o) {
    r=ERR_INTERNAL;
    goto exit;
  }

  /*snprintf(value, sizeof(value), "%s", json_object_to_json_string(o));*/
  DBG("key:'%s'' val:'%s'", key, json_object_to_json_string(o));

  if (db_set_event_data(bp->db, receiver, t, json_object_to_json_string(o)))
    r = ERR_DATABASE;

exit:

  if (o)
    json_object_put(o);

  (*ans)=dm_mk_jsonanswer(r);

  return r;
}


int dm_compare_event_keys(const void *i1, const void *i2)
{
  uint64_t u1, u2;
  u1=*((uint64_t *)i1);
  u2=*((uint64_t *)i2);

  if (u1<u2)
    return -1;

  if (u1>u2)
    return 1;

  return 0;
}

json_object * dm_get_event_db_data(dm_business_prm_t *bp, const char *receiver, uint64_t utime)
{
  json_object *o=0;

  if (db_get_event_data(bp->db, receiver, utime, (char *)bp->buf, THREADBUFSIZE))
    goto exit;

  o = json_parse((const char *)bp->buf, json_type_object);
  if (!o) {
    ERR("Database has record with wrong syntax: '%s'", bp->buf);
    goto exit;
  }

exit:

  return o;
}

int dm_get_event_db_received_flag(dm_business_prm_t *bp, const char *receiver,
                                   uint64_t utime, uint16_t *received)
{
  int r = ERR_DATABASE;
  json_object *o;
  const char *s;

  o = dm_get_event_db_data(bp, receiver, utime);
  if (!o)
    goto exit;

  s = json_get_string(o, "received");
  if (!s)
    goto exit;

  if (ut_s2n10(s, received))
    goto exit;

  r=0;

exit:

  if (o)
   json_object_put(o);

  return r;
}


int dm_get_all_events(dm_business_prm_t *bp, const char *receiver,
                      char checkreceived, uint64_t *item, uint16_t *cnt)
{
  int r=0;
  uint16_t i, rcnt, received=1;

  if (db_get_events_times_list(bp->db, receiver, item, cnt)) {
    r = ERR_DATABASE;
    goto exit;
  }

  DBGL(2, "db count: %u", *cnt);

  for (i=0; i<(*cnt); ++i) {

    DBGL(3,"%i: %"PRIu64, i, item[i]);

    /* mark already received */
    if (checkreceived) {
      if (dm_get_event_db_received_flag(bp, receiver, item[i], &received)) {
        item[i]=0;
        continue;
      }
      if (received) {
        item[i]=0;
        continue;
      }
    }

  }

  /* sort from low -> high */
  if ((*cnt)>1)
    qsort(item, *cnt, sizeof(item[0]), dm_compare_event_keys);

  /* skip already received */
  rcnt = 0;
  for (i=0; i<(*cnt); ++i)
    if (item[i] != 0) {
      rcnt = (*cnt)-i;
      if (i)
        memcpy(&item[0], &item[i], sizeof(item[0])*(rcnt));
      break;
    }

  for (i=0; i<rcnt; ++i)
    DBGL(2, "event_time: %u %"PRIu64, i, item[i]);

exit:


  (*cnt)=rcnt;

  return r;
}

uint64_t *dm_find_event(uint64_t *item, uint16_t cnt, uint64_t min)
{    
  uint16_t i;
  if (!cnt)
    return 0;

  if (!min)
    return &item[0];

  for (i=0;i<cnt;++i)
    if (item[i]>min)
      return &item[i];

  return 0;
}


int dm_do_get_event(dm_business_prm_t *bp, json_object *req, json_object **ans)
{
  int r=0;
  const char *receiver;
  const char *min_event_time, *not_modified;
  json_object *event_data_o=0;
  uint64_t *timep, mintime;
  uint16_t cnt;
  uint64_t st,t, lasttime=0;

  DBG("");

  if (dm_json_check(req, get_event_items)) {
    r=ERR_WRONG_MANDATORY_ITEM;
    goto exit;
  }

  receiver = json_get_string(req, "receiver");

  if (dm_check_receiver(bp, receiver)) {
    ERR("Receiver '%s' not exist", receiver);
    r=ERR_ACCESS;
    goto exit;
  }

  min_event_time = json_get_string(req, "event_last_time");
  not_modified = json_get_string(req, "not_modified");

  dm_getcurtime(&st);

  do {

    dm_getcurtime(&lasttime);
    /*dm_set_event_lasttime(bp->db, receiver, lasttime);*/

    cnt = MAXEVENTS_PERRECEIVER;
    r = dm_get_all_events(bp, receiver, (min_event_time)?0:1, bp->etimes, &cnt);
    if (r) {
      goto exit;
    }

    mintime=0;
    if (min_event_time) {
      if (ut_s2nll10(min_event_time, &mintime)) {
        ERR("Wrong min_event_time!");
        goto exit;
      }
    }

    timep = dm_find_event(bp->etimes, cnt, mintime);

    if (!timep)
      usleep(100000);

    dm_getcurtime(&t);

  } while ((!timep) && ((t-st) < (bp->wait_event_timer*1000)) && (!(*bp->stop)));

  if (!timep) {
    r = ERR_TIMEOUT;
    goto exit;
  }

  lasttime = (*timep);

  DBG("found event_time %"PRIu64, *timep);

  event_data_o = dm_get_event_db_data(bp, receiver, *timep);
  if (!event_data_o) {
    r=ERR_DATABASE;
    goto exit;
  }

  /*DBG("%p '%s'", event_data_o, v->rdReply->str);*/

  dm_json_print("", event_data_o);

  if (!not_modified) {
    DBG("modify 'received' field");
    json_object_object_del(event_data_o, "received");
    json_add_string(event_data_o, "received", "1");

    if (db_set_event_data(bp->db, receiver, *timep, json_object_to_json_string(event_data_o))) {
      r= ERR_DATABASE;
      goto exit;
    }
  }

exit:

  if ((!r) && (event_data_o)) {
    dm_add_jsonanswer(event_data_o, r);
    (*ans) = event_data_o;
    /*json_object_object_add((*ans), "event_data", event_data_o);*/
  } else {
    (*ans)=dm_mk_jsonanswer(r);
  }

  if (lasttime)
    json_add_string_u64(*ans, "event_last_time", lasttime);

  return r;
}

typedef int (*do_cmd_funt_t)(dm_business_prm_t *, json_object *, json_object**);

typedef struct {
  char *name;
  do_cmd_funt_t do_cmd;
  uint32_t flags;
} json_cmd_table_item_t;

typedef enum
{CMFL_IFACEFCGI=1, CMFL_IFACECLI=2}
event_id_t;

json_cmd_table_item_t cm_str_table[]={
  { "SetEvent", dm_do_set_event, CMFL_IFACECLI },
  { "GetEvent", dm_do_get_event, CMFL_IFACECLI | CMFL_IFACEFCGI },
  { "CreateUser", dm_do_create_user, CMFL_IFACECLI },
  { "DeleteUser", dm_do_delete_user, CMFL_IFACECLI },
  { "GetNow", dm_do_getnow, CMFL_IFACEFCGI },
  { 0, 0, 0 }
};

json_cmd_table_item_t *dm_find_json_cmd(json_cmd_table_item_t *t, const char *cm)
{
  int i = 0;
  while (t[i].name) {
    if (!strcasecmp(cm, t[i].name))
      return &t[i];
    i++;
  }
  return 0;
}

int dm_process_json_cmd(dm_business_prm_t *bp, json_object *req, json_object **ans)
{
  json_object *cmd_obj=0;
  const char *cmd, *iface;
  uint32_t mask;
  int r=0;

  if (!req) {
    r = ERR_WRONG_SYNTAX;
    goto exit;
  }

  dm_json_print("", req);

  if (dm_json_check(req, gen_items)) {
    r = ERR_WRONG_MANDATORY_ITEM;
    goto exit;
  }

  cmd = json_get_string(req, "cmd");

  DBG("cmd: '%s'", cmd);

  json_cmd_table_item_t *cm = dm_find_json_cmd(cm_str_table, cmd);

  if (!cm) {
    r=ERR_UNKNOWN_COMMAND;
    goto exit;
  }

  /* check iface allowed for command */
  if (!dm_vars.prm.no_check_iface) {
    iface = json_get_string(req, "interface");
    mask = 0;
    if (strcmp(iface, "cli")==0)
      mask |= CMFL_IFACECLI;
    if (strcmp(iface, "fcgi")==0)
      mask |= CMFL_IFACEFCGI;

    if (!(cm->flags & mask)) {
      DBG("wrong iface '%s' for command '%s'", iface, cmd);
      r= ERR_ACCESS;
      goto exit;
    }
  }

  /* execute command */
  cm->do_cmd(bp, req, ans);

exit:

  if (!(*ans))
    (*ans)=dm_mk_jsonanswer(r);
  DBG("ans %s", json_object_to_json_string(*ans));

  if (cmd_obj)
    json_object_put(cmd_obj);

  return r;
}

int dm_process_json_cmd_buf(dm_business_prm_t *bp)
{    
  libdio_msg_str_cmd_r_t *rshdr = (libdio_msg_str_cmd_r_t *)bp->buf;
  json_object *req=0, *ans=0, *o;
  int r=0;

  DBG("");

  req = json_parse(((libdio_msg_str_cmd_t*)bp->buf)->cmd, json_type_object);
  if (!req) {
    r = ERR_WRONG_SYNTAX;
    ans = dm_mk_jsonanswer(r);
    goto exit;
  }

  /* add interface */
  o = json_object_new_string("cli");
  json_object_object_add(req, "interface", o);

  r = dm_process_json_cmd(bp, req, &ans);

exit:

  DBG("ans: %s", json_object_to_json_string(ans));

  LIBDIO_FILLJSONRESPONSE(rshdr, json_object_to_json_string(ans), 0);

  if (ans)
    json_object_put(ans);
  if (req)
    json_object_put(req);

  return 0;
}


int dm_cli_cml_info_handler(int argc, char **argv, void *data)
{
   dm_cli_thdata_t * thd = (dm_cli_thdata_t*)data;
   libdio_msg_str_cmd_r_t *hdr = (libdio_msg_str_cmd_r_t *)thd->buf;

   DBG("");

   sprintf(hdr->response, "cli threads %u/%u/%u\nfcgi threads %u/%u/%u\n",
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

int dm_cli_thread_accept(void *thdata)
{
  dm_cli_thdata_t *thd = (dm_cli_thdata_t *)thdata;

  thd->fd = accept(thd->p.fd, NULL, 0);

  if (thd->fd<0) {
    ERR("accept: %s", strerror(errno));
    return -1;
  }

  return 0;
}

int dm_thread_accept(dm_thread_t *th, int (*accept_fun)(void *))
{
  int r;
  dm_cli_thdata_t *thd = (dm_cli_thdata_t *)th->data;
  do {
      pthread_mutex_lock(&th->pool->accept_mtx);

      if (th->stop) {
        pthread_mutex_unlock(&th->pool->accept_mtx);
        return 1;
      };

      r=libdio_waitfd(thd->p.fd, 100, 'r');

      if (r>0) {
        pthread_mutex_unlock(&th->pool->accept_mtx);
        if (th->stop)
          return 1;
        continue;
      }

      if (r<0) {
        pthread_mutex_unlock(&th->pool->accept_mtx);
        return -1;
      }

      if (accept_fun(thd)) {
        pthread_mutex_unlock(&th->pool->accept_mtx);
        continue;
      }

      pthread_mutex_unlock(&th->pool->accept_mtx);
      break;

  } while (1);

  return 0;
}

void *dm_cli_thread(void *ptr)
{
  dm_thread_t *th = (dm_thread_t *)ptr;
  dm_cli_thdata_t *thd = (dm_cli_thdata_t *)th->data;
  libdio_msg_hdr_t *hdr;
  libdio_msg_response_hdr_t *rhdr;
  libdio_msg_str_cmd_r_t *rshdr;    
  dm_business_prm_t bp;

  uint16_t code;
  int r;

  DBG("started %p", th);

  while (!th->stop) {

    if (dm_thread_accept(th, dm_cli_thread_accept))
      goto exit;

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
        DBGL(2, "connection closed %p", th);
        break;
      }

      if (libdio_read_message(thd->fd, thd->buf))
        continue;

      code = ntohs(hdr->code);
      DBGL(2,"msg: len: %u code: %04X", ntohs(hdr->len), code);
      char unknown[]="Unknown command";

      switch (code) {
        case LIBDIO_MSG_STR_CMD:
          DBGL(2,"cm: %s", ((libdio_msg_str_cmd_t*)hdr)->cmd);
          r = libdio_process_str_cmd(thd->buf, cli_cml_table, 1, thd);
          if (r) {
           if (r==-2) {
             LIBDIO_FILLSTRRESPONSE(rshdr, unknown, 0xF0);
           } else {
              LIBDIO_FILLSTRRESPONSE(rshdr, "", 0xF1);
           }
          }
        break;
        case LIBDIO_MSG_JSON_CMD:
          DBGL(2,"json: %s", ((libdio_msg_str_cmd_t*)hdr)->cmd);
          bp.buf = thd->buf;
          bp.etimes = thd->etimes;
          bp.db = thd->p.db;
          bp.stop = &th->stop;
          bp.wait_event_timer = dm_vars.prm.wait_event_timer;
          dm_process_json_cmd_buf(&bp);
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


exit:

  if (th->stop)
    DBG("stopped %p", th);

  dm_send_thread_exit_signal(th);

  return &dm_vars;
}

void dm_print_fcgi_req_params(FCGX_Request *r)
{
  char **p;
  for (p = r->envp; *p; ++p)
    DBGL(3, "PRM: %s", *p);

}

char *fcgi_answer[] =
{
  "Content-type: text/html\r\n",
  "\r\n",
  "<html>\r\n",
  "<head>\r\n",
  "<title>Hello !!!</title>\r\n",
  "</head>\r\n",
  "<body>\r\n",
  "<h1>Hello! This is commetd server </h1>\r\n",
  "<p>Request accepted from host <i>",
  0,
  "</i></p>\r\n",
  0,
  "</body>\r\n",
  "</html>\r\n",
  0
};

char *fcgi_answer_prefix="Content-type: application/json\r\n\r\n";
char *fcgi_answer_suffix="";

void dm_fcgi_out_strs(FCGX_Request *r, char **s)
{
  while (*s) {
   FCGX_PutS(*s, r->out);
   s++;
  }
}

void dm_fcgi_out_strs_n(FCGX_Request *r, char **s)
{
  while (*s) {
   FCGX_PutS("<p>", r->out);
   FCGX_PutS(*s, r->out);
   FCGX_PutS("</p>\r\n", r->out);
   s++;
  }
}

void dm_fcgi_out_str_n(FCGX_Request *r, char *s)
{
  FCGX_PutS("<p>", r->out);
  FCGX_PutS(s, r->out);
  FCGX_PutS("</p>\r\n", r->out);
}

int dm_fcgi_thread_accept(void *thdata)
{
  dm_fcgi_thdata_t *thd = (dm_fcgi_thdata_t *)thdata;
  int r = FCGX_Accept_r(&thd->req);
  if (r) {
    ERR("FCGX_Accept_r: %i", r);
    return -1;
  }
  return 0;
}

void url2s(char *dst, const char *src)
{
  char a, b;
  while (*src) {
    if ((*src == '%') &&
       ((a = src[1]) && (b = src[2])) &&
       (isxdigit(a) && isxdigit(b))) {
       if (a >= 'a')
         a -= 'a'-'A';
       if (a >= 'A')
         a -= ('A' - 10);
       else
         a -= '0';
       if (b >= 'a')
         b -= 'a'-'A';
       if (b >= 'A')
         b -= ('A' - 10);
       else
         b -= '0';
         *dst++ = 16*a+b;
         src+=3;
       } else {
         *dst++ = *src++;
       }
  }
  *dst++ = '\0';
}

json_type dm_getjsontype(char *s)
{
  int i, l, d = 1;
  l=strlen(s);

  for (i=0;i<l;++i) {
    if (s[i]=='{')
       return json_type_object;
    if (!isdigit(s[i]))
      d=0;
  }

  if (d)
     return json_type_int;

  return json_type_string;
}

int dm_fcgi2json(char *str, int s, int e, json_object *obj)
{
  int m;
  char *ch, c;
  json_object *o;

  if (s>=e)
    return -1;

  ch = strchr(&str[s], '=');

  if (!ch)
    return -1;

  m = ch - str;
  str[m]=0;
  c = str[e+1];
  str[e+1]=0;
  DBG("'%s':'%s'", &str[s], &str[m+1]);

  switch (66) {
/*  switch (dm_getjsontype(&str[m+1])) {*/
    case json_type_int:
      o = json_object_new_int(strtoul(&str[m+1],0,10));
    break;
    case json_type_object:
      o = json_tokener_parse(&str[m+1]);
    break;
    default:
      o = json_object_new_string(&str[m+1]);
  }

  if (o)
   json_object_object_add(obj, &str[s], o);

  str[m]='=';
  str[e+1]=c;

  if (!o)
    return -1;

  return 0;
}

json_object *dm_qs2json(char *qs)
{    
  char *pc;
  json_object *obj;
  int l = strlen(qs);
  int s=0,e=0;

  obj = json_object_new_object();

  while (s<l) {

    pc = strchr(&qs[s], '&');

    if (pc==qs) {
      json_object_put(obj);
      return 0;
    }
    e = (pc)?((pc-qs)-1):(l-1);

    if (dm_fcgi2json(qs, s, e, obj)) {
      json_object_put(obj);
      return 0;
    }
/*
    if (e<(l-1))
      qs[e+1]=0;
    DBG("fcgiprm: '%s'", &qs[s]);
    if (e<(l-1))
      qs[e+1]='&';
*/
    s=(e+2);

  }

  return obj;
}

int dm_get_cookie_value(char *cookstr, char *name, char *value)
{
  char *ptr1, *cookie, *n, *ptr2;
  int l,i;

  value[0]=0;

  while ((cookie = strtok_r(cookstr, ";", &ptr1))) {
    cookstr=0;
    DBGL(3, "cookie:'%s'",cookie);
    n = strtok_r(cookie, "=", &ptr2);

    /* delete space */
    l=strlen(n);
    for (i=0;i<l;++i)
      if (n[0]==' ')
        n++;

    if (!strcmp(n, name)) {
      strcpy(value, strtok_r(0, "=", &ptr2));
      return 0;
    }
  }

  return 1;
}

int dm_get_receiver_id(dm_business_prm_t *bp, char *cookies, uint32_t *receiverid)
{
  char hash[64];
  char receiver[64];
  int r=0, ret;

  if (!cookies)
    return 1;

  DBG("cookies:'%s'",cookies);


  if (dm_get_cookie_value(cookies, bp->cookiename, hash)) {
    ERR("Wrong or absent cookie!");
    r = ERR_ACCESS;
    goto exit;
  }

  if (!hash[0]) {
    ERR("Empty hash value!");
    r = ERR_ACCESS;
    goto exit;
  }

  DBG("cookie hash: %s ", hash);

  ret = db_get_user_receiver(bp->db, hash, receiver, sizeof(receiver));

  if (ret) {
    r = (ret==DB_ERR_NOTFOUND)?ERR_ACCESS:ERR_DATABASE;
    goto exit;
  }

  if (ut_s2nl10(receiver, receiverid)) {
    ERR("Wrong receiver id");
    r=ERR_DATABASE;
    goto exit;
  }

exit:

  return r;
}


int dm_process_fcgi(dm_vars_t *v, dm_thread_t *th, dm_fcgi_thdata_t *thd)
{
  FCGX_Request *r = &thd->req;
  char *server_name=0, *query_string=0, *cookie=0;
  uint32_t receiverid;
  char q_string[256];
  char receiver[32];
  int ret;
  json_object *req=0, *ans=0, *o;
  dm_business_prm_t bp;

  #ifdef DM_DEBUG
  dm_print_fcgi_req_params(r);
  #endif

  server_name = FCGX_GetParam("SERVER_NAME", r->envp);
  query_string = FCGX_GetParam("QUERY_STRING", r->envp);  
  cookie = FCGX_GetParam("HTTP_COOKIE", r->envp);

  /*  get receiver from DB */

  bp.buf = thd->buf;
  bp.db = thd->p.db;
  bp.etimes = thd->etimes;
  bp.stop = &th->stop;
  bp.wait_event_timer = v->prm.wait_event_timer;
  bp.cookiename = v->prm.cookiename;

  ret = dm_get_receiver_id(&bp, cookie, &receiverid);
  if (ret) {
    ans = dm_mk_jsonanswer(ret);
    goto exit;
  }  
  snprintf(receiver, sizeof(receiver), "%u", receiverid);

  /* parse URL and convert to JSON */
  url2s(q_string, query_string);
  req=dm_qs2json(q_string);
  if (!req) {
    ERR("Wrong QUERY_STRING syntax!");
    ans = dm_mk_jsonanswer(ERR_WRONG_SYNTAX);
    goto exit;
  }

  /* add cmd if not present */
  if (!json_get_string(req, "cmd")) {
    o = json_object_new_string("GetEvent");
    json_object_object_add(req, "cmd", o);
  }

  /* add receiver */
  o = json_object_new_string(receiver);
  json_object_object_add(req, "receiver", o);

  /* add interface */
  o = json_object_new_string("fcgi");
  json_object_object_add(req, "interface", o);

  dm_process_json_cmd(&bp, req, &ans);

exit:

  FCGX_GetStr(thd->buf, THREADBUFSIZE, r->in);

  if (thd->p.fcgi_http_debug) {

    dm_fcgi_out_strs(r, fcgi_answer);
    FCGX_PutS(server_name, r->out);
    dm_fcgi_out_strs(r, &fcgi_answer[10]);

    dm_fcgi_out_strs_n(r, r->envp);

    FCGX_PutS("<p> QUERY: ", r->out);
    FCGX_PutS(query_string, r->out);
    FCGX_PutS("</p>", r->out);

    FCGX_PutS("<p> POST: ", r->out);
    FCGX_PutS(thd->buf, r->out);
    FCGX_PutS("</p>", r->out);

    FCGX_PutS("<p> ANSWER: ", r->out);
    FCGX_PutS(json_object_to_json_string(ans), r->out);
    FCGX_PutS("</p>", r->out);
    dm_fcgi_out_strs_n(r, &fcgi_answer[12]);

  } else {
    FCGX_PutS(fcgi_answer_prefix, r->out);
    FCGX_PutS(json_object_to_json_string(ans), r->out);
    FCGX_PutS(fcgi_answer_suffix, r->out);
  }

  json_object_put(req);
  json_object_put(ans);

  return 0;
}

void *dm_fcgi_thread(void *ptr)
{
  dm_thread_t *th = (dm_thread_t *)ptr;
  dm_fcgi_thdata_t *thd = (dm_fcgi_thdata_t *)th->data;
  FCGX_Request *req = &thd->req;

  DBG("started %p", th);

  if (FCGX_InitRequest(req, thd->p.fd, 0))  {
    ERR("Can't init fcgi req!");
    goto exit;
  }

  while (!th->stop) {

    /* wait for req */
    if (dm_thread_accept(th, dm_fcgi_thread_accept))
      goto exit;

    DBGL(2, "request accepted");

    dm_process_fcgi(&dm_vars, th, th->data);

    /*shutdown(r->ipcFd, SHUT_RDWR);*/
    FCGX_Finish_r(req);
  }

exit:

  if (th->stop)
    DBG("stopped %p", th);

  dm_send_thread_exit_signal(th);
  shutdown(thd->p.fd, SHUT_RDWR);
  close(thd->p.fd);
  return &dm_vars;
}

int dm_handle_thread_exited(dm_vars_t *v)
{
  dm_thread_t *th;
  void *p;

  while (1) {

   if (libdio_waitfd(v->thpipe.fd[0], 100, 'r'))
     return -1;

   read(v->thpipe.fd[0], &th, sizeof(th));

   if (!th) {
     DBG("th==0");
     return -1;
   }

   pthread_join(th->tid, &p);

   if (th->pool) {
     DBG("%s thread exited %p", th->pool->name, p);
     dm_thpool_del(th->pool, th->tid);
   }

  }
  return 0;
}

void dm_thread_abort(dm_thread_t *th)
{
  DBG("thread aborted %p", th);
  pthread_cancel(th->tid);
  /*pthread_join(th->tid, 0);*/
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
  uint16_t timer;


  pthread_mutex_lock(&ta->mtx);

  if (!ta->cnt) {
    pthread_mutex_unlock(&ta->mtx);
    return 0;
  }

  for (i=0;i<ta->cnt;i++)
     ta->th[i]->stop=1;
  timer = (ta->cnt+1);
  pthread_mutex_unlock(&ta->mtx);


  while (timer!=0) {

    cnt=0;
    pthread_mutex_lock(&ta->mtx);
    for (i=0;i<ta->cnt;i++)
      if (!((ta->th[i])->stopped))
        cnt++;

    pthread_mutex_unlock(&ta->mtx);

    if (cnt) {
      usleep(110000);
      timer--;
    }
    else break;
  }

  if (timer) {
    DBG("pool %s stopped normal %i", ta->name, timer);
    return 0;
  }

  DBG("%u %s threads not stopped normaly!", cnt, ta->name);
/*
  for (i=0;i<ta->cnt;i++)
    if (!ta->th[i]->stopped)
      dm_thread_abort(ta->th[i]); */


  return 0;
}

int dm_handle_timeout(dm_vars_t*v)
{
  /*DBG("\n")*/
  return 0;
}

void set_timeval(struct timeval *tv, uint16_t timer)
{
  memset(tv, 0 ,sizeof(struct timeval));
  tv->tv_sec = timer/1000;
  tv->tv_usec = (timer - (tv->tv_sec)*1000)*1000;
}

int main(int argc, char **argv)
{
  dm_init_vars(&dm_vars);

  if (dm_handle_args(&dm_vars.prm, argc, argv))
    dm_exit(&dm_vars, 1);

  if (dm_init(&dm_vars))
    dm_exit(&dm_vars, 1);

  LOG("daemon started...");

  uint8_t i;
  int fds[3];
  int fdcount;
  fd_set set, rset;

  FD_ZERO(&set);
  /*FD_SET(dm_vars.cli_fd, &set);
  FD_SET(dm_vars.fcgi_fd, &set);*/
  FD_SET(dm_vars.thpipe.fd[0], &set);

  fds[0]=dm_vars.thpipe.fd[0];
  /*fds[0]=dm_vars.cli_fd;
  fds[1]=dm_vars.fcgi_fd;
  fds[2]=dm_vars.thpipe.fd[0];*/
  fdcount=1;

  struct timeval tv;

  while (1) {

    /* wait for event */
    set_timeval(&tv, dm_vars.prm.sleeptimer);

    rset = set;
    int maxfd,s;

    maxfd=-1;
    for (i=0; i<fdcount; ++i)
      if (fds[i]>maxfd)
        maxfd = fds[i];

    maxfd += 1;
    s = select(maxfd, &rset, 0, 0, &tv);

    if (s<0) {
      if (errno != EINTR) {
        ERR("select(): %s", strerror(errno));
        dm_exit(&dm_vars, 1);
      }
      continue;
    }

    if (s==0) {
      dm_handle_timeout(&dm_vars);
      continue;
    }

    int i, fd;
    for (i = 0, fd = fds[0]; i < fdcount; ++i, fd=fds[i]) {

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

