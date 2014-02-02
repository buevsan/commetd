#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "credis/credis.h"
#include "fcgi_config.h"
#include "fcgiapp.h"
#include "json-parser.h"
#include "debug.h"
#include "utils.h"

typedef struct dm_prm_s
{
  int dlevel;
  struct in_addr redisaddr;
  uint16_t redisport;
  uint16_t fcgiport;
  union {
   uint8_t foreground:1;
  };
} dm_prm_t;

typedef struct dm_state_s
{

} dm_state_t;

typedef struct dm_vars_s
{
  dm_prm_t prm;
  dm_state_t st;
  REDIS db;
  int fcgi_fd;
  FCGX_Request *fcgi_req;
  dbg_desc_t dbg;
} dm_vars_t;

dm_vars_t dm_vars;

#define ERR(format, ...) debug_print(&dm_vars.dbg, 0, "ERR: "format, ##__VA_ARGS__)
#define MSG(format, ...) debug_print(&dm_vars.dbg, 0, format, ##__VA_ARGS__)
#define LOG(format, ...) MSG(format, ##__VA_ARGS__)

#ifdef DM_DEBUG
#define DBGL(level, format, ...) debug_print(&dm_vars.dbg, 1+level, "DBG: "format, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(1, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif

int dm_init_vars(dm_vars_t *v)
{
  memset(v, 0, sizeof(dm_vars_t));
  v->prm.dlevel=1;
  v->prm.fcgiport=6666;

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
  sprintf(str, "*:%u", v->prm.fcgiport);
  v->fcgi_fd = FCGX_OpenSocket(str, 5);
  if (v->fcgi_fd<0) {
    ERR("Can't create fcgi socket %s!\n", str);
    return -1;
  }

  if (FCGX_InitRequest(v->fcgi_req, v->fcgi_fd, 0))  {
    ERR("Can't init fcgi req!\n");
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

  if (!v->prm.foreground) {
    DBGL(2, "Become a daemon...\n");
    daemon(0, 0);
  }

  return 0;
}

int dm_cleanup(dm_vars_t *v)
{
  debug_free(&v->dbg);

  if (v->fcgi_fd>=0)
    close(v->fcgi_fd);

  return 0;
}


static struct option loptions[] = {
  {"help", 0, 0, 0},
  {"rdhost", 1, 0, 0},
  {"rdport", 1, 0, 0},
  {"fcgi-port, 1, 0, 0"},
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

int main(int argc, char **argv)
{
  dm_init_vars(&dm_vars);

  if (dm_handle_args(&dm_vars.prm, argc, argv))
      dm_exit(&dm_vars, 0);

  if (dm_init(&dm_vars))
    dm_exit(&dm_vars, 1);


  /* JSON test */
  char * string = "{\"sitename\" : \"joys of programming\","
                     "\"categories\" : [ \"c\" , [\"c++\" , \"c\" ], \"java\", \"PHP\" ],"
                     "\"author-details\": { \"admin\": false, \"name\" : \"Joys of Programming\", \"Number of Posts\" : 10 } "
                     "}";
  printf("JSON string: %sn", string);
  json_object * jobj = json_tokener_parse(string);     
  json_parse(jobj);

  return 0;

}

