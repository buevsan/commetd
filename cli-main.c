#include <stdio.h>
#include <stdlib.h>
#include <debug.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>

#include "utils.h"
#include "libdio.h"

#define CMBUFSIZE 2048

typedef struct cli_prm_s
{
  int dlevel;
  char *cm;
  uint8_t json;
  struct in_addr dm_addr;
  uint16_t dm_port;
} cli_prm_t;

typedef struct cli_vars_s
{
  int fd;
  uint8_t *buf;
  cli_prm_t prm;
  dbg_desc_t dbg;
} cli_vars_t;

cli_vars_t cli_vars;

#define ERR(format, ...) debug_print(&cli_vars.dbg, 0, "ERR: "format"\n", ##__VA_ARGS__)
#define MSG(format, ...) debug_print(&cli_vars.dbg, 0, format"\n", ##__VA_ARGS__)

#ifdef CLI_DEBUG
#define DBGL(level, format, ...) debug_print(&cli_vars.dbg, 1+level, "DBG: %s: "format"\n", __FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(0, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif

void sig_handler(int signum);

int cli_init_vars(cli_vars_t *v)
{
  memset(v, 0, sizeof(cli_vars_t));
  v->prm.dlevel=0;
  v->prm.dm_port=7777;
  v->prm.dm_addr.s_addr = inet_addr("127.0.0.1");
  return 0;
}

int cli_init(cli_vars_t *v)
{  
  if (debug_init(&v->dbg, v->prm.dlevel))
      return -1;
  libdio_setlog(&v->dbg);

  DBG("");

  v->buf = malloc(CMBUFSIZE);
  if (!v->buf)
    return -1;

  v->fd = socket(AF_INET,SOCK_STREAM, 0);
  if (v->fd<0)
    return -1;

  libdio_signal(SIGINT, sig_handler);
  libdio_signal(SIGTERM, sig_handler);
  libdio_signal(SIGHUP, sig_handler);

  /* set nonblock */
  int flags = fcntl(v->fd, F_GETFL, 0);
  if (flags==-1)
    return -1;
  if (fcntl(v->fd, F_SETFL, flags | O_NONBLOCK))
    return -1;

  return 0;
}

int cli_connect(cli_vars_t *v)
{
  struct sockaddr_in sa;
  int r;

  DBG("%s:%u", inet_ntoa(v->prm.dm_addr), v->prm.dm_port);

  memset(&sa, 0, sizeof(sa));
  sa.sin_family=AF_INET;
  sa.sin_addr = v->prm.dm_addr;
  sa.sin_port = htons(v->prm.dm_port);

  r = connect(v->fd, (struct sockaddr *)&sa, sizeof(sa));
  DBGL(2, "conr= %i", r);
  if (r) {
    if (errno!=EINPROGRESS) {
      ERR("connect %s", strerror(errno));
      return -1;
    }
  }

  r = libdio_waitfd(v->fd, 500, 'w');

  DBG("r:%i", r);

  if (r!=0)
    return -1;

  return 0;
}

int cli_cleanup(cli_vars_t *v)
{
  DBG("");
  debug_free(&v->dbg);

  if (v->fd>=0) {
    shutdown(v->fd, SHUT_RDWR);
    close(v->fd);
  }

  if (v->buf)
    free(v->buf);

  return 0;
}

void sig_handler(int signum)
{
  MSG("Signal %i received", signum);

  if ((signum == SIGINT) || (signum == SIGTERM)) {
    cli_cleanup(&cli_vars);
    MSG("Exiting...");
    exit(0);
  }
}

static struct option loptions[] = {
  {"help", 0, 0, 0},
  {"host", 1, 0, 0},
  {"port", 1, 0, 0},
  {"cm", 1, 0, 0},
};

void cli_print_help(void)
{
  printf("cli [host] [port] \n\n");
  printf("-h - help\n"
         "-d <level> - debug level\n"
         "-l <file> - logfile\n"
         "-c - command\n");
}

int cli_handle_long_opt(cli_prm_t *p, int idx)
{
  switch (idx)
  {
    case 0:
      cli_print_help();
      return 1;
    case 1:
      if (!inet_aton(optarg, &p->dm_addr))
        printf("Wrong REDIS address!\n");
    break;
    case 2:
      if (ut_s2n10(optarg, &p->dm_port))
        printf("Wrong port!\n");
    break;
    case 3:
      if (!optarg)
        return 1;
      p->cm=optarg;
    break;
    default:;
  }
  return 0;
}

int cli_handle_args(cli_prm_t * p, int argc, char **argv)
{
  int c;
  int optidx;

  while (1) {
    c = getopt_long(argc, argv, "jhp:d:c:", loptions,  &optidx);
    if (c==-1)
      break;
    switch (c) {
      case 0:
        if (cli_handle_long_opt(p, optidx))
          return 1;
      break;
      case 'j':
        p->json=1;
      break;
      case 'd':
        p->dlevel=strtoul(optarg, 0, 10);
      break;
      case 'c':
        if (!optarg)
          return 1;
        p->cm=optarg;
      break;
      case 'h':
       cli_print_help();
       return 1;
      break;
   }
 }

 if (optind < argc)
   if (!inet_aton(argv[optind], &p->dm_addr)){
     ERR("Wrong host!");
     return -1;
   };

 if ((optind+1) < argc)
   if (ut_s2n10(argv[optind+1], &p->dm_port)) {
     ERR("Wrong port!");
     return -1;
   }

 if ((optind+2) < argc)
   p->cm = argv[optind+2]; 

 if ((!p->cm) || (!strlen(p->cm)))
   return 1;

 return 0;
}

int cli_execute_cm(int fd, void *buf, size_t bufsize, char *command, char json)
{
  int r;
  int timeout=500;

  DBG("%s", command);

  if ( !((command) && (buf)))
    return -1;

  libdio_msg_str_cmd_t *hdr=(libdio_msg_str_cmd_t *)buf;
  hdr->hdr.code = htons((!json)?LIBDIO_MSG_STR_CMD:LIBDIO_MSG_JSON_CMD);
  hdr->hdr.len = htons(strlen(command)+1);
  strncpy(hdr->cmd, command, 64);

  libdio_write_message(fd, buf);

  r = libdio_waitfd(fd, timeout, 'r');

  if (r) {
    if (r>0)
      ERR("timeout");
    goto error;
  }

  libdio_read_message(fd, buf);
  libdio_msg_str_cmd_r_t *hdr_r = (libdio_msg_str_cmd_r_t *)buf;

  DBG("response status %02X", hdr_r->rhdr.status);

  if (hdr_r->rhdr.status!=0)
    goto error;

  shutdown(fd, SHUT_RDWR);
  close(fd);

  fprintf(stdout, "%s\n", hdr_r->response);

  return 0;

error:

  if (fd >= 0) {
   shutdown(fd, SHUT_RDWR);
   close(fd);
  }

  return 1;
}

int cli_mk_command(cli_vars_t *v)
{
  return cli_execute_cm(v->fd, v->buf, CMBUFSIZE, v->prm.cm, v->prm.json);
}

int main(int argc, char **argv)
{
  int r;
  cli_init_vars(&cli_vars);

  if ((r = cli_handle_args(&cli_vars.prm, argc, argv)))
    return (r>0)?0:1;

  if (cli_init(&cli_vars))
    return -1;

  if (cli_connect(&cli_vars))
    return -1;

  cli_mk_command(&cli_vars);

  cli_cleanup(&cli_vars);

  return 0;
}

