#ifndef LIBDIO_H
#define LIBDIO_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "debug.h"

typedef struct {
  uint16_t len;
  uint16_t code;
} libdio_msg_hdr_t;

typedef struct {
  libdio_msg_hdr_t hdr;
  uint8_t status;
} libdio_msg_response_hdr_t;

typedef struct {
  libdio_msg_hdr_t hdr;
  char cmd[1];
} libdio_msg_str_cmd_t;

typedef struct {
  libdio_msg_response_hdr_t rhdr;
  char response[1];
} libdio_msg_str_cmd_r_t;

typedef int (*libdio_str_cmd_handler_t)(int, char **,  void *);

typedef struct {
  char *cmd;
  libdio_str_cmd_handler_t handler;
} libdio_str_cmd_tbl_t;


/* IO */

void libdio_setlog(dbg_desc_t *dbg);
int libdio_waitfd(int fd, uint16_t timer, char mode);
int libdio_read_message(int fd,  uint8_t *buf);
int libdio_write_message(int fd,  uint8_t *buf);
int libdio_process_str_cmd(void *rbuf, libdio_str_cmd_tbl_t *cmd_tbl,
                           uint16_t cmd_count, void *data);
int libdio_execute_cm(char *sockpath, void *buf, size_t bufsize, char *command);
int libdio_signal(int signum, void (*handler)(int));

#define LIBDIO_MSG_RESPONSE    0x8000
#define LIBDIO_MSG_STR_CMD     0x0000
#define LIBDIO_MSG_JSON_CMD    0x0001

#define LIBDIO_FILLRESPONSE(rhdr, ln, cod, sts) \
   rhdr->status = sts; \
   rhdr->hdr.code = htons(cod | LIBDIO_MSG_RESPONSE); \
   rhdr->hdr.len = htons(ln);

#define LIBDIO_FILLSTRRESPONSE(rshdr, str, sts) \
   strcpy(rshdr->response, str); \
   LIBDIO_FILLRESPONSE((&(rshdr->rhdr)), strlen(str)+1, LIBDIO_MSG_STR_CMD, sts);

#define LIBDIO_FILLJSONRESPONSE(rshdr, str, sts) \
   strcpy(rshdr->response, str); \
   LIBDIO_FILLRESPONSE((&(rshdr->rhdr)), strlen(str)+1, LIBDIO_MSG_JSON_CMD, sts);

#endif
