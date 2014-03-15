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

#include "credis/credis.h"
#include <fcgi_config.h>
#include <fcgiapp.h>
#include <json/json.h>
#include "debug.h"
#include "utils.h"
#include "libdio.h"

#define MAXCLIENTS 32
#define THREADBUFSIZE 2048
#define THREADSTACKSIZE 20

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
  int redisdb;
  char bdprefix[33];
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
  uint8_t stop;
  uint8_t stopped;
  void *stack;
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
  pthread_attr_t th_attr;
  uint16_t stacksize;
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
#define LOG(format, ...) MSG("Commetd: "format, ##__VA_ARGS__)

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
  v->prm.redisdb = 3;
  strncpy(v->prm.bdprefix, "prefix", sizeof(v->prm.bdprefix));

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
  libdio_setnonblock(v->fcgi_fd, 1);


  s=0;
  if (v->prm.redisaddr.s_addr)
    s=inet_ntoa(v->prm.redisaddr);
  DBGL(2, "Connecting to REDIS '%s':%u...", s?s:"127.0.0.1", v->prm.redisport);
  v->db = credis_connect(0, v->prm.redisport, 20000);
  if (!v->db) {
    ERR("Can't connect to redis!");
    /*return -1;*/
  }  
  if (credis_select(v->db, v->prm.redisdb)) {
    ERR("Can't connect to db '%i'!", v->prm.redisdb);
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
  libdio_setnonblock(v->cli_fd, 1);


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
  {"fcgi-port, 1, 0, 0"},
  {"cli-port, 1, 0, 0"},
  {"fcgi-iface, 1, 0, 0"},
  {"cli-iface, 1, 0, 0"},
  {"rd-prefix, 1, 0, 0"},
  {0, 0, 0, 0}
};

void dm_print_help(void)
{
  printf("commetd \n\n");
  printf("-h - help\n"
         "-d <level> - debug level\n"
         "-m <num> - maximum number of threads\n"
         "-s <num> - started number of threads\n"
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
    case 7:
      strncpy(p->bdprefix, sizeof(p->bdprefix), optarg);
    break;
  }
  return 0;
}

int dm_handle_args(dm_prm_t * p, int argc, char **argv)
{
  int c;
  int optidx;

  while (1) {
    c = getopt_long(argc, argv, "hfl:d:s:m:", loptions,  &optidx);
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
      case 's':
        ut_s2n10(optarg, &p->minths);
      break;
      case 'm':
        ut_s2n10(optarg, &p->maxths);
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

  posix_memalign(&th->stack,1024, 1024*pool->stacksize);
  pthread_attr_setstack(&pool->th_attr, th->stack, pool->stacksize);

  DBGL(3, "thd: %p", th->data);

  pthread_mutex_lock(&pool->mtx);
  pool->th[pool->cnt]=th;
  pool->cnt++;
  if (pool->cnt > pool->peakcnt)
    pool->peakcnt = pool->cnt;
  pthread_mutex_unlock(&pool->mtx);


  if (pthread_create(&tid, &pool->th_attr, pool->th_fun, th)) {
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

dm_json_obj_t msg_edata_items[]= {
  { "type", json_type_string, 0 },
  { "message", json_type_string, 0 },
  { "type_id", json_type_int, 0 },
  { "unique", json_type_string, 0 },
  { 0, 0, 0 }
};

dm_json_obj_t msg_items[]= {
  { "cmd", json_type_string, 0 },
  { "receiver", json_type_string, 0 },
  { "edata", json_type_string, 0 },
  { 0, 0, 0 }
};

json_object *dm_json_getobj(json_object *obj, char *key, json_type type)
{
  json_object *o;
  o = json_object_object_get(obj, key);
  if (json_object_get_type(o)!=type)
    return 0;
  return o;
}

int dm_json_check(json_object *obj, dm_json_obj_t *table)
{
  int i=0;
  json_object *o;
  while (table[i].name) {
    o = dm_json_getobj(obj, table[i].name, table[i].type);
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

json_object * dm_json_mkanswer(uint16_t code, char *text)
{
  json_object * answer_o = json_object_new_object();
  json_object * code_o = json_object_new_int(code);
  json_object_object_add(answer_o, "code", code_o);
  
  if ((text) && (text[0])) {
   json_object * text_o = json_object_new_string(text);
   json_object_object_add(answer_o, "text", text_o);
  } 
  return answer_o;
}

const char *dm_get_json_str_value(json_object *req, char *name)
{
  json_object *obj = dm_json_getobj(req, name, json_type_string);
  if (!obj)
    return 0;
  return json_object_get_string(obj);
}

int dm_do_set_event(REDIS db, json_object *req, json_object **ans)
{
  int r;
  const char *hash, *receiver, *prefix, *dbid, *edata;
  char *msg;
  char s[6];
  char key[64];
  time_t t;

  DBG("");

  ctime(&t);

  hash = dm_get_json_str_value(req, "hash");
  receiver = dm_get_json_str_value(req, "receiver");
  edata = dm_get_json_str_value(req, "edata");

  if ((!hash)||(!receiver)) {
    r=666;
    msg="Mandatory item absent";
    goto exit;
  }

  prefix = dm_get_json_str_value(req, "prefix");
  if (!prefix)
    prefix = dm_vars.prm.bdprefix;

  dbid = dm_get_json_str_value(req, "select");
  if (!dbid) {
    snprintf(s, sizeof(s), "%u", dm_vars.prm.redisdb);
    dbid = s;
  }

  snprintf(key, sizeof(key), "%s:%s:%u", prefix, hash, (uint16_t)t);
  DBG("key:%s receiver:%s ", key, receiver);

  if (credis_hset(db, key, "receiver", receiver)) {
    ERR("Can't set redis key '%s' to '%s'", key, receiver);
    return 1;
  }

exit:

  (*ans)=dm_json_mkanswer(r, msg);

  return r;
}

int dm_do_get_event(REDIS db, json_object *req, json_object **ans)
{
  int r;
  const char *receiver, *event_type;
  const char *min_event_time, *not_modified, *hash, *prefix;
  char *msg;
  char s[6];
  char key[64];

  DBG("");

  hash = dm_get_json_str_value(req, "hash");
  receiver = dm_get_json_str_value(req, "receiver");

  if ((!hash)||(!receiver)) {
    r=666;
    msg="Mandatory item absent";
    goto exit;
  }

  min_event_time = dm_get_json_str_value(req, "min_event_time");
  not_modified = dm_get_json_str_value(req, "not_modified");

  prefix = dm_get_json_str_value(req, "prefix");
  if (!prefix)
    prefix = dm_vars.prm.bdprefix;

  snprintf(key, sizeof(key), "%s:%s", prefix, hash);
  /*snprintf(field, sizeof(key), "%s:%s", prefix, hash);*/


/*
  if (credis_hget(db, key, "", &edata)) {
    ERR("Can't set redis key '%s'");
    return 1;
  }
*/
exit:

  (*ans)=dm_json_mkanswer(r, msg);

  return 0;
}

typedef struct str_table_item_s {
  char *name;
  uint8_t id;
} str_table_item_t;

typedef enum
{EV_SETEVENT, EV_GETEVENT}
event_id_t;

str_table_item_t cm_str_table[]={
  { "SetEvent", EV_SETEVENT },
  { "GetEvent", EV_GETEVENT },
  { 0, 0 }
};

int dm_find_str_table(str_table_item_t *t, char *cm)
{
  int i = 0;
  while (t[i].name) {
    if (!strcmp(cm, t[i].name))
      return i;
    i++;
  }
  return -1;
}


int dm_process_json_cmd(json_object *req, json_object **ans)
{
  json_object *cmd_obj=0;
  char *cmd, *msg = "OK";
  int r=0, receiver;

  if (!req) {
    msg = "Wrong JSON syntax!";
    r = 666;
    goto exit;
  }

  dm_json_print("", req);

  if (dm_json_check(req, msg_items)) {
    msg="Wrong JSON message!";
    r = 999;
    goto exit;
  }

  cmd_obj = dm_json_getobj(req, "cmd", json_type_string);
  cmd = (char *)json_object_get_string(cmd_obj);

  DBG("cmd: '%s'", cmd);

  int cmid = dm_find_str_table(cm_str_table, cmd);  

  switch (cmid) {
    case EV_SETEVENT:
      dm_do_set_event(dm_vars.db, req, ans);
    break;
    case EV_GETEVENT:
      dm_do_get_event(dm_vars.db, req, ans);
    break;
  }    

exit:

  if (!(*ans))
    (*ans)=dm_json_mkanswer(r, msg);
  DBG("ans %s", json_object_to_json_string(*ans));

  if (cmd_obj)
    json_object_put(cmd_obj);

  return r;
}

int dm_process_json_cmd_buf(uint8_t *buf)
{
  libdio_msg_str_cmd_r_t *rshdr = (libdio_msg_str_cmd_r_t *)buf;
  json_object *req=0, *ans=0;
  int r;
  req = json_tokener_parse(((libdio_msg_str_cmd_t*)buf)->cmd);
  r = dm_process_json_cmd(req, &ans);
  LIBDIO_FILLJSONRESPONSE(rshdr, json_object_to_json_string(ans), r?0xF1:0);
  json_object_put(ans);
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
          DBGL(1,"json: %s", ((libdio_msg_str_cmd_t*)hdr)->cmd);
          dm_process_json_cmd_buf(thd->buf);
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
  if (FCGX_Accept_r(&thd->req)) {
    ERR("FCGX_Accept_r");
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

  switch (dm_getjsontype(&str[m+1])) {
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

  value[0]=0;

  while ((cookie = strtok_r(cookstr, ";", &ptr1))) {
    cookstr=0;
    DBGL(4, "cookie:'%s'",cookie);
    n = strtok_r(cookie, "=", &ptr2);
    if (!strcmp(n, name)) {
      strcpy(value, strtok_r(0, "=", &ptr2));
      return 0;
    }
  }

  return 1;
}

int dm_get_receiver_id(dm_vars_t *v, char *cookies, char **receiver)
{
  uint32_t u32;
  char key[128];
  char hash[64];
  char *prefix=0;

  DBG("cookies:'%s'",cookies);

  if (dm_get_cookie_value(cookies, "PHPSSEID", hash))
    return 1;

  if (!hash[0])
    return 1;

  DBG("cookie hash: %s ", hash);

  if (!prefix)
    prefix=v->prm.bdprefix;

  snprintf(key, sizeof(key), "%s:%s", prefix, hash);

  if (credis_hget(v->db, key, "receiver", receiver)) {
    ERR("Can't get key '%s' from redis", key);
    return 1;
  }

  DBG("receiver: %s", *receiver);

  if (ut_s2nl10(*receiver, &u32))
    return 1;


  return 0;
}

int dm_process_fcgi(dm_vars_t *v, dm_thread_t *th, dm_fcgi_thdata_t *thd)
{
  FCGX_Request *r = &thd->req;
  char *server_name=0, *query_string=0, *cookie=0, *receiver=0;
  char q_string[256];
  json_object *req=0, *ans=0, *o;

  #ifdef DM_DEBUG
  dm_print_fcgi_req_params(r);
  #endif

  server_name = FCGX_GetParam("SERVER_NAME", r->envp);
  query_string = FCGX_GetParam("QUERY_STRING", r->envp);  
  cookie = FCGX_GetParam("HTTP_COOKIE", r->envp);

  /*  get receiver from DB */
  if (dm_get_receiver_id(v, cookie, &receiver)) {
    ERR("Wrong or absent cookie!");
    goto exit;
  }

  /* parse URL and convert to JSON */
  url2s(q_string, query_string);
  req=dm_qs2json(q_string);
  if (!req) {
    ERR("Wrong QUERY_STRING syntax!");
    goto exit;
  }

  /* add cmd if not present */
  if (!dm_get_json_str_value(req, "cmd")) {
    o = json_object_new_string("GetEvent");
    json_object_object_add(req, "cmd", o);
  }
  /* add receiver */
  o = json_object_new_string(receiver);
  json_object_object_add(req, "receiver", o);

  dm_process_json_cmd(req, &ans);

exit:

  FCGX_GetStr(thd->buf, THREADBUFSIZE, r->in);

  dm_fcgi_out_strs(r, fcgi_answer);
  FCGX_PutS(server_name, r->out);
  dm_fcgi_out_strs(r, &fcgi_answer[10]);
  dm_fcgi_out_strs_n(r, r->envp);
  FCGX_PutS("<p> POST: ", r->out);
  FCGX_PutS(thd->buf, r->out);
  FCGX_PutS("</p>", r->out);

 /* FCGX_PutS("<p> ANSWER: ", r->out);
  FCGX_PutS(json_object_to_json_string(ans), r->out);
  FCGX_PutS("</p>", r->out);*/
  dm_fcgi_out_strs_n(r, &fcgi_answer[12]);

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

  LOG("daemon started...");

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

