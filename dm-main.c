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
#include "json-parser.h"
#include "debug.h"
#include "utils.h"

#define MAXCLIENTS 32

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

typedef struct dm_thread_s
{
  pthread_t tid;
  int fd;
} dm_thread_t;

typedef struct dm_thr_array_s
{
  pthread_mutex_t mtx;
  dm_thread_t *th[MAXCLIENTS];
  void *(*th_fun) (void *);
  uint16_t cnt;
} dm_thr_array_t;

typedef struct dm_vars_s
{
  dm_prm_t prm;
  dm_state_t st;
  REDIS db;
  int fcgi_fd;
  FCGX_Request *fcgi_req;
  int cli_fd;
  dbg_desc_t dbg;

  dm_thr_array_t cli_clients;
  dm_thr_array_t fcgi_clients;
  dm_th_pipe_t thpipe;

} dm_vars_t;

void *dm_cli_thread(void *ptr);
void *dm_fcgi_thread(void *ptr);
void sig_handler(int signum);
void sigchld_handler(int signum);


dm_vars_t dm_vars;

#define ERR(format, ...) debug_print(&dm_vars.dbg, 0, "ERR: "format, ##__VA_ARGS__)
#define MSG(format, ...) debug_print(&dm_vars.dbg, 0, format, ##__VA_ARGS__)
#define LOG(format, ...) MSG(format, ##__VA_ARGS__)

#ifdef DM_DEBUG
#define DBGL(level, format, ...) debug_print(&dm_vars.dbg, 1+level, "DBG: %s "format, __FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(1, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif

void dm_init_clients(dm_thr_array_t *ta, void* (*th_fun)(void*))
{
  ta->cnt=0;
  pthread_mutex_init(&ta->mtx,0);
  ta->th_fun=th_fun;
}

void dm_init_th_pipe(dm_th_pipe_t *t)
{
  pthread_mutex_init(&t->mtx, 0);
  pipe(t->fd);
}

int dm_signal(int signum, void (*handler)(int))
{
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = handler;
  if (sigaction(signum, &act, NULL)==-1) {
    ERR("sigaction: %s\n", strerror(errno));
    return 1;
  }
  return 0;
}

int dm_init_vars(dm_vars_t *v)
{
  memset(v, 0, sizeof(dm_vars_t));
  v->prm.dlevel=1;
  v->prm.fcgiport=6666;
  v->prm.cliport=7777;
  v->prm.sleeptimer=1000;

  dm_init_clients(&v->cli_clients, dm_cli_thread);
  dm_init_clients(&v->fcgi_clients, dm_fcgi_thread);
  dm_init_th_pipe(&v->thpipe);

  return 0;
}

int dm_init(dm_vars_t *v)
{
  char *s;
  char str[32];  

  if (debug_init(&v->dbg, v->prm.dlevel))
    return -1;

  DBG("Initilization...\n");

  if (FCGX_Init()) {
    ERR("Can't init fcgi library!\n");
    return -1;
  }

  DBGL(2,"Create fcgi socket %u...\n", v->prm.fcgiport);  

  if (v->prm.fcgi_iface.s_addr) {
    sprintf(str, "%s:", inet_ntoa(v->prm.fcgi_iface));
    sprintf(&str[strlen(str)], "%u",
          v->prm.fcgiport);
  } else
    sprintf(str, "*:%u", v->prm.fcgiport);

  v->fcgi_fd = FCGX_OpenSocket(str, 5);
  if (v->fcgi_fd<0) {
    ERR("Can't create fcgi socket %s!\n", str);
    return -1;
  }

  s=0;
  if (v->prm.redisaddr.s_addr)
    s=inet_ntoa(v->prm.redisaddr);
  DBGL(2, "Connecting to REDIS '%s':%u...\n", s?s:"127.0.0.1", v->prm.redisport);
  v->db = credis_connect(0, v->prm.redisport, 20000);
  if (!v->db) {
    ERR("Can't connect to redis!\n");
    return -1;
  }  

  /* open cli socket */
  struct sockaddr_in sa;

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
    ERR("Can't bind socket '%s'\n", strerror(errno));
    return -1;
  }

  if (listen(v->cli_fd, 5)) {
    ERR("Listen error '%s'\n", strerror(errno));
    return -1;
  }

  dm_signal(SIGINT, sig_handler);
  dm_signal(SIGTERM, sig_handler);
  dm_signal(SIGHUP, sig_handler);
  dm_signal(SIGALRM, sig_handler);
  dm_signal(SIGCHLD, sigchld_handler);

  if (!v->prm.foreground) {
    DBGL(2, "Become a daemon...\n");
    daemon(0, 0);
  }

  return 0;
}

int dm_cleanup(dm_vars_t *v)
{
  debug_free(&v->dbg);

  if (v->fcgi_fd>=0) {
    shutdown(v->fcgi_fd, SHUT_RDWR);
    close(v->fcgi_fd);
  }
  if (v->cli_fd>=0) {
    shutdown(v->cli_fd, SHUT_RDWR);
    close(v->cli_fd);
  }

  return 0;
}

void sig_handler(int signum)
{
  LOG("Signal received %i\n", signum);

  if (signum == SIGHUP) {
    LOG("Internal states initialized\n");
    return;
  }

  if ((signum == SIGINT) || (signum == SIGTERM)) {
    dm_cleanup(&dm_vars);
    LOG("Exiting...\n");
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

    /*libdio_del_child(&glob_prms.ch, pid);*/

    if (WIFEXITED(sts)) {
      LOG("Child %i exited, status=%d\n", pid, WEXITSTATUS(sts));
    } else if (WIFSIGNALED(sts)) {
      LOG("Child %i killed by signal %d\n", pid, WTERMSIG(sts));
    } else if (WIFSTOPPED(sts)) {
      LOG("Child %i stopped by signal %d\n", pid, WSTOPSIG(sts));
    } else if (WIFCONTINUED(sts)) {
      LOG("Child %i continued\n", pid);
    }
 }
}

int dm_process_fcgi(dm_vars_t *v) 
{
  if (FCGX_InitRequest(v->fcgi_req, v->fcgi_fd, 0))  {
      ERR("Can't init fcgi req!\n");
      return -1;
  }
  
  if (FCGX_Accept_r(v->fcgi_req))
     LOG("request accepted\n"); 
      
  return 0;  
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
    LOG("Exit: with error %i\n", code);
  } else {
    LOG("Exit: success\n");
  }
  exit(code);
}


int dm_handle_del_client(dm_thr_array_t *clients, pthread_t tid)
{
  int i,j;
  DBGL(3, "\n");
  for (i=0; i<clients->cnt;++i)
    if (clients->th[i]->tid==tid)
      break;

  pthread_mutex_lock(&clients->mtx);

  if (i>=clients->cnt) {
    pthread_mutex_unlock(&clients->mtx);
    return -1;
  }

  free(clients->th[i]);

  for (j=i; j<(clients->cnt-1);++j)
    clients->th[j]=clients->th[j+1];
  clients->cnt--;

  pthread_mutex_unlock(&clients->mtx);

  return 0;
}

int dm_handle_new_client(dm_thr_array_t *clients, int fd)
{
  dm_thread_t *th=0;
  int newfd=-1;
  pthread_t tid;

  newfd = accept(fd, NULL, 0);

  if (newfd<0) {
    ERR("accept error %s\n",strerror(errno));
    goto error;
  }

  if (clients->cnt>=MAXCLIENTS) {
    ERR("client count too big \n");
    goto error;
  }

  th = malloc(sizeof(dm_thread_t));
  memset(th, 0, sizeof(dm_thread_t));

  th->fd = newfd;

  pthread_mutex_lock(&clients->mtx);
  clients->th[clients->cnt]=th;
  clients->cnt++;
  pthread_mutex_unlock(&clients->mtx);

  DBG("new thread this %p\n", th);
  if (pthread_create(&tid, 0, clients->th_fun, th)) {
    ERR("thread create error\n");
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

void dm_send_thread_exit_signal( dm_th_pipe_t *t , uint8_t type)
{
  pthread_t tid;
  pthread_mutex_lock(&t->mtx);
  tid=pthread_self();
  write(t->fd[1], &tid, sizeof(pthread_t));
  write(t->fd[1], &type, 1);
  pthread_mutex_unlock(&t->mtx);
}

void *dm_cli_thread(void *ptr)
{
  dm_thread_t *th = (dm_thread_t *)ptr;
  DBG("new cli started %lu\n", pthread_self());

  int cnt=10;
  while ((--cnt)>0) {
    DBG("cli thread worked\n");
    sleep(1);
  }

  /*int r = select(th->fd+1, rset, 0,0, &tv);*/


  dm_send_thread_exit_signal(&dm_vars.thpipe, 0);

  shutdown(th->fd, SHUT_RDWR);
  close(th->fd);

  return &dm_vars;
}

void *dm_fcgi_thread(void *ptr)
{
  dm_thread_t *th = (dm_thread_t *)ptr;
  DBG("new fcgi started %lu\n", pthread_self());

  int cnt=10;
  while ((--cnt)>0) {
    DBG("fcgi thread worked\n");
    sleep(1);
  }

  dm_send_thread_exit_signal(&dm_vars.thpipe, 1);
  shutdown(th->fd, SHUT_RDWR);
  close(th->fd);
  return &dm_vars;
}

int dm_handle_thread_exited(dm_vars_t *v)
{
  pthread_t tid;
  uint8_t type;
  void *p;
  read(v->thpipe.fd[0], &tid, sizeof(pthread_t));
  read(v->thpipe.fd[0], &type, 1);
  pthread_join(tid, &p);
  DBG("%s thread exited %p\n", (type)?"fcgi":"cli", p);
  dm_handle_del_client((type)?(&v->fcgi_clients):(&v->cli_clients), tid);
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

  int fds[2];
  int fdcount;
  fd_set set, rset;

  FD_ZERO(&set);
  FD_SET(dm_vars.cli_fd, &set);
  FD_SET(dm_vars.fcgi_fd, &set);
  FD_SET(dm_vars.thpipe.fd[0], &set);

  fds[0]=dm_vars.cli_fd;
  fds[1]=dm_vars.fcgi_fd;
  fds[3]=dm_vars.thpipe.fd[0];
  fdcount=3;

  struct timeval tv;

  while (1) {

    /* wait for event */
    tv.tv_sec=0;
    tv.tv_usec=1000*dm_vars.prm.sleeptimer;

    rset = set;
    int maxfd,s ;
    maxfd = (dm_vars.cli_fd > dm_vars.fcgi_fd)?dm_vars.cli_fd:dm_vars.fcgi_fd;
    maxfd += 1;
    s = select(maxfd, &rset, 0, 0, &tv);

    if (s<0) {
      if (errno != EINTR) {
        ERR("select: %s\n", strerror(errno));
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
        DBGL(3,"ADD CLI client %i!\n", fd);
        dm_handle_new_client(&dm_vars.cli_clients, fd);
        continue;
      };

      if (fd==dm_vars.fcgi_fd) {
        /* new connection */
        DBGL(3,"ADD FCGI client %i!\n", fd);
        dm_handle_new_client(&dm_vars.fcgi_clients, fd);
        continue;
      };

      if (fd==dm_vars.thpipe.fd[0]) {
        /* thread exited */
        dm_handle_thread_exited(&dm_vars);
        continue;
      };

    }

  }

#if 0

  /* JSON test */
  char * string = "{\"sitename\" : \"joys of programming\","
                     "\"categories\" : [ \"c\" , [\"c++\" , \"c\" ], \"java\", \"PHP\" ],"
                     "\"author-details\": { \"admin\": false, \"name\" : \"Joys of Programming\", \"Number of Posts\" : 10 } "
                     "}";
  printf("JSON string: %sn", string);
  json_object * jobj = json_tokener_parse(string);     
  json_parse(jobj);
#endif


  return 0;

}

