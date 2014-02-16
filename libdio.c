#include "libdio.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#define LIBDIO_DEBUG

dbg_desc_t *libdio_dbg=0;

#define ERR(format, ...) debug_print(libdio_dbg, 0, "ERR: "format"\n", ##__VA_ARGS__)
#define MSG(format, ...) debug_print(libdio_dbg, 0, format"\n", ##__VA_ARGS__)
#define LOG(format, ...) MSG(format, ##__VA_ARGS__)

#ifdef LIBDIO_DEBUG
#define DBGL(level, format, ...) debug_print(libdio_dbg, 1+level, "DBG: %s: "format"\n", __FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(0, format, ##__VA_ARGS__);
#else
#define DBGL(level, format, ...)
#define DBG(format, ...) ;
#endif


#define LIBDIO_CMEXE_TIMEOUT 1000

void libdio_setlog(dbg_desc_t *dbg)
{
  libdio_dbg = dbg;
}

int libdio_save_read(int fd, void *buf, size_t size)
{
  size_t rcv=0;
  int r;

  while (1) {

    r=libdio_waitfd(fd, 100, 'r');

    if (r) {
      if (r>0)
        ERR("io timeout");
      return -1;
    }

    r=read(fd, &((char*)buf)[rcv], size-rcv);

    if (r<0) {
      if (r!=EINTR) {
        ERR("read %s", strerror(errno));
        return -1;
      }
    }

    rcv += r;
    if (rcv>=size)
      break ;

  }

  return 0;
}

int libdio_save_write(int fd, void *buf, size_t size)
{
  return write(fd, buf, size);
}

int libdio_read_message(int fd,  uint8_t *buf)
{
  libdio_msg_hdr_t *rhdr=(libdio_msg_hdr_t *)buf;

  if (libdio_save_read(fd, buf, sizeof(libdio_msg_hdr_t)))
    return -1;

  DBGL(4, "msg: len: %u code: %04X", ntohs(rhdr->len), ntohs(rhdr->code));

  if (libdio_save_read(fd, &buf[sizeof(libdio_msg_hdr_t)], ntohs(rhdr->len)))
    return -1;

  return 0;
}

int libdio_write_message(int fd,  uint8_t *buf)
{
  libdio_msg_hdr_t *whdr=(libdio_msg_hdr_t *)buf;
  DBGL(4, "msg: len: %i code: %u", fd, sizeof(libdio_msg_hdr_t) + ntohs(whdr->len), whdr->code);
  return libdio_save_write(fd, buf, sizeof(libdio_msg_hdr_t) + ntohs(whdr->len));
}

int libdio_strtook(char **start, char **end, char **next)
{
  char state=0, *c;
  int ch;

  if (!(*start))
    return 1;

  for (c=(*start); c!=0; ++c) {

    ch = (*c);

    switch (state) {
      case 0:
       if (isspace((int)(*c)))
         continue;
       if (ch=='"') {
         state=1;
         (*start)=(c+1);
       } else {
         state=2;
         (*start)=c;
       }
      break;
      case 1:
       if (ch=='"') {
         (*c)=0;
         (*end) = (c-1);
         (*next) = (c+1);
         if ((*start) >= (*end))
           return 1;
         return 0;
       }
      break;
      case 2:
       if (isspace((int)(*c))) {
         (*c)=0;
         (*end) = c;
         (*next) = (c+1);
         if ((*start) >= (*end))
           return 1;
         return 0;
       }
    }
  }

  if (state==2) {
    (*end) = (c-1);
    (*next) = 0;
    if ((*start) < (*end))
        return 0;
  }

  return 1;
}

int libdio_process_str_cmd(void *rbuf, libdio_str_cmd_tbl_t *cmd_tbl, uint16_t cmd_count, void *data)
{
  int i;
  char *argv[32], *p;
  int argc = 0;

  /* build argc argv */

  p = strtok(((libdio_msg_str_cmd_t *)rbuf)->cmd, " ");

  while (p && argc < 32) {
    argv[argc++] = p;
    p = strtok(0, " ");
  }

  /*  */
  DBGL(4, "argc %i\n", argc);

  if (argc < 1)
    return -1;

  for (i=0; i<argc; ++i)
    DBGL(4, "argv[%i] %s", i, argv[i]);

  /* find command */
  for (i = 0;  i < cmd_count; ++i)
    if (!strcmp(argv[0], cmd_tbl[i].cmd))
      break;

  if (i >= cmd_count)
    return -2;

  DBGL(4, "handler found %u", i);

  return cmd_tbl[i].handler(argc, argv, data);

}

int libdio_signal(int signum, void (*handler)(int))
{
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = handler;
  if (sigaction(signum, &act, NULL)==-1) {
    ERR("sigaction: %s", strerror(errno));
    return 1;
  }
  return 0;
}

int libdio_waitfd(int fd, uint16_t timer, char m)
{
  fd_set set;
  struct timeval tv;
  int r;

  FD_ZERO(&set);
  FD_SET(fd, &set);
  memset(&tv, 0 ,sizeof(tv));
  tv.tv_usec = 1000*timer;

  while (1) {

    DBGL(4, "wait '%c' timer %u", m, timer);
    r = select(fd+1, (m=='r')?&set:0, (m=='w')?&set:0, 0, &tv);

    if (r<0) {

      if ((errno==EINTR) && (timer))
         return 1;

      ERR("select %s", strerror(errno));
      return -1;
    }

    if (r==0)
      return 1;

    if (r>0)
      break;
  }

  return 0;
}
