#include <hiredis/hiredis.h>
#include <pthread.h>
#include <stdlib.h>
#include <memory.h>
#include "db.h"

typedef struct {
  char *prefix;
  pthread_mutex_t mtx;
  redisContext *rdCtx;
  redisReply *rdReply;
} redis_db_t;

#define RDB(p) ((redis_db_t*)p)

int db_init(db_t *db)
{
  db->d=malloc(sizeof(redis_db_t));
  memset(db->d, 0, sizeof(redis_db_t));
  pthread_mutex_init(&RDB(db)->mtx, 0);
  return 0;
}

void db_free(db_t *db)
{
  free(db->d);
  db->d=0;
}

int db_cmd(db_t *db, const char *fmt, ...)
{
  return 0;
}

int db_set_cmd(db_t *db, const char *fmt, ...)
{
  return 0;
}

int db_get_str_cmd(db_t *db, const char *fmt, ...)
{
  return 0;
}

int db_get_list_cmd(db_t *db, const char *fmt, ...)
{
  return 0;
}

int db_create_user(db_t *db, const char *hash, const char *receiver)
{
  return 0;
}

int db_del_user(db_t *db, const char *hash, const char *receiver)
{
  return 0;
}

int db_get_user_hash(db_t *db, const char *receiver, char *hash, uint16_t len)
{
  return 0;
}

int db_get_user_receiver(db_t *db, const char *hash, char *receiver, uint16_t len)
{
  return 0;
}

int db_set_event_data(db_t *db, const char *receiver, const char *etime, const char *data)
{
  return 0;
}

int db_get_event_data(db_t *db, const char *receiver, char *data, uint16_t len)
{
  return 0;
}


