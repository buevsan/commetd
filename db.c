#include <hiredis/hiredis.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <stdarg.h>
#include <inttypes.h>
#include "db.h"
#include "debug.h"
#include "utils.h"

#define CMBUFSIZE 20000

typedef struct {
  char *prefix;
  pthread_mutex_t mtx;
  redisContext *rdCtx;
  redisReply *rdReply;
  char *buf;
  char key[64];
  dbg_desc_t *log;
  uint32_t event_expire_timer;
  uint32_t user_expire_timer;
  uint16_t dbnum;
} redis_db_t;

#define RDB(db_ptr) ((redis_db_t*)db_ptr->d)

#define ERR(format, ...) debug_print(RDB(db)->log, 0, "ERR: "format"\n", ##__VA_ARGS__)
#define MSG(format, ...) debug_print(RDB(db)->log, 0, format"\n", ##__VA_ARGS__)
#define LOG(format, ...) MSG("Commetd: "format, ##__VA_ARGS__)

#ifdef DB_DEBUG
#define DBGL(level, format, ...) debug_print(RDB(db)->log, 1+level, "DBG: %lu: %s: "format"\n", pthread_self(),__FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(1, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif


int db_init(db_t *db)
{
  db->d=malloc(sizeof(redis_db_t));
  memset(db->d, 0, sizeof(redis_db_t));
  RDB(db)->buf = malloc(CMBUFSIZE);
  RDB(db)->event_expire_timer = 100;
  RDB(db)->user_expire_timer = 86400;
  RDB(db)->prefix="prefix";

  pthread_mutex_init(&(RDB(db)->mtx), 0);
  return 0;
}

void db_free(db_t *db)
{
  if (!db)
    return;
    
  if (!RDB(db))
    return; 
  
  db_disconnect(db);
  free(RDB(db)->buf);
  free(db->d);
  db->d=0;
}


void db_setlog(db_t *db, void *log)
{
  RDB(db)->log = (dbg_desc_t*)log;
}

void db_setdbnum(db_t *db, uint16_t dbnum)
{
  RDB(db)->dbnum = dbnum;
}

void db_set_prefix(db_t *db, char *prefix)
{
  RDB(db)->prefix = prefix;
}

void db_set_event_expire_timer(db_t *db, uint32_t t)
{
  RDB(db)->event_expire_timer = t;
}

void db_set_user_expire_timer(db_t *db, uint32_t t)
{
  RDB(db)->user_expire_timer = t;
}


int db_free_reply(redisReply **reply)
{
  if (*reply) {
    freeReplyObject(*reply);
    (*reply) = 0;
  }
  return 0;
}

int db_free_lastreply(db_t *db)
{
  if (RDB(db)->rdReply) {
    freeReplyObject(RDB(db)->rdReply);
    RDB(db)->rdReply = 0;
  }
  return 0;
}


int db_lock(db_t *db)
{
  pthread_mutex_lock(&RDB(db)->mtx);
  return 0;
}

int db_unlock(db_t *db)
{
  pthread_mutex_unlock(&RDB(db)->mtx);
  return 0;
}

int db_check_reply(db_t *db, redisReply *rdReply, int code)
{
  if (!rdReply) {
    DBG("Null redis answer received!");
    return 1;
  }  

  if (code) {
    if (rdReply->type!=code) {
      DBG("Expected %u redis answer received %u", code, rdReply->type);
      return 1;
    }
  } else {
    if (rdReply->type==REDIS_REPLY_ERROR) {
      DBG("Redis error '%s'!", rdReply->str);
      return 1;
    }
  }

  return 0;
}

int db_cmd(db_t *db, int code, const char *fmt, ...)
{
  int r=0;
  va_list args;

  db_free_reply(&RDB(db)->rdReply);

  va_start(args, fmt);
  vsnprintf(RDB(db)->buf, CMBUFSIZE, fmt, args);
  va_end(args);

  RDB(db)->rdReply = redisCommand(RDB(db)->rdCtx, RDB(db)->buf);

  if (db_check_reply(db, RDB(db)->rdReply, code)) {
    ERR("Wrong answer for redis command '%s'", RDB(db)->buf);
    r = 1;
  }

  return r;
}

int db_cmd_arg(db_t *db, int code, int argc, const char **argv, const size_t *arglen)
{
  int r=0;

  db_free_reply(&RDB(db)->rdReply);

  RDB(db)->rdReply = redisCommandArgv(RDB(db)->rdCtx, argc, argv, arglen);

  if (db_check_reply(db, RDB(db)->rdReply, code)) {
    ERR("Wrong answer for redis command '%s'", RDB(db)->buf);
    r = 1;
  }

  return r;
}


int db_connect(db_t *db, char *hostname, uint16_t port)
{
  int r = 0;
  struct timeval timeout = { 1, 500000 };

  db_lock(db);

  RDB(db)->rdCtx = redisConnectWithTimeout(hostname, port, timeout);
  if ((!RDB(db)->rdCtx) || (RDB(db)->rdCtx->err)) {
    ERR("Can't connect to Redis server '%s'!", hostname);
    r=1;
    goto exit;
  }

  if (db_cmd(db, 0, "select %u", RDB(db)->dbnum)) {
    redisFree(RDB(db)->rdCtx);
    RDB(db)->rdCtx=0;
    r=1;
    goto exit;
  }

  if (db_cmd(db, 0, "flushdb")) {
    redisFree(RDB(db)->rdCtx);
    RDB(db)->rdCtx=0;
    r=1;
  }

exit:

  db_unlock(db);

  return r;
}

int db_disconnect(db_t *db)
{
  if (RDB(db)->rdCtx) {
    redisFree(RDB(db)->rdCtx);
    RDB(db)->rdCtx=0;
  }
  return 0;
}

int db_create_user(db_t *db, const char *hash, const char *receiver)
{
  int r=0;
  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key), "%s:%s", RDB(db)->prefix, hash);
  DBG("key:'%s' hash:'%s'", RDB(db)->key, hash);

  r = db_cmd(db, 0, "hset %s receiver %s", RDB(db)->key, receiver);
  if (r)
    goto exit;

  r = db_cmd(db, 0, "expire %s %"PRIu32, RDB(db)->key, RDB(db)->user_expire_timer);
  if (r)
    goto exit;

  snprintf(RDB(db)->key, sizeof(RDB(db)->key), "%s:r%s", RDB(db)->prefix, receiver);
  DBG("key:'%s' receiver:'%s' ", RDB(db)->key, receiver);

  r = db_cmd(db, 0, "hset %s hash %s", RDB(db)->key, hash);
  if (r)
    goto exit;

  r = db_cmd(db, 0, "expire %s %"PRIu32, RDB(db)->key, RDB(db)->user_expire_timer);
  if (r)
    goto exit;


exit:

  db_free_lastreply(db);

  db_unlock(db);

  return r;
}

int db_del_user(db_t *db, const char *hash, const char *receiver)
{
  int r=0;

  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key), "%s:%s", RDB(db)->prefix, hash);
  r = db_cmd(db, 0, "hdel %s receiver", RDB(db)->key);
  if (r)
    goto exit;

  snprintf(RDB(db)->key, sizeof(RDB(db)->key), "%s:r%s", RDB(db)->prefix, receiver);
  r = db_cmd(db, 0, "hdel %s hash", RDB(db)->key);
  if (r)
    goto exit;

exit:

  db_free_lastreply(db);

  db_unlock(db);

  return r;
}

int db_get_user_hash(db_t *db, const char *receiver, char *hash, uint16_t len)
{
  int r = 0;
  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key), "%s:r%s", RDB(db)->prefix, receiver);

  r = db_cmd(db, 0, "hget %s hash", RDB(db)->key);

  if (r)
    goto exit;

  if (RDB(db)->rdReply->type != REDIS_REPLY_STRING) {
    r = 1;
    if (RDB(db)->rdReply->type == REDIS_REPLY_NIL) {
      r = DB_ERR_NOTFOUND;
      DBG("User not found!");
    } else {
      ERR("Not string hash id %i", RDB(db)->rdReply->type);
    }
  }

exit:

  if ((!r)&&(hash))
    strncpy(hash, RDB(db)->rdReply->str, len);

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}

int db_get_user_receiver(db_t *db, const char *hash, char *receiver, uint16_t len)
{
  int r = 0;

  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key), "%s:%s", RDB(db)->prefix, hash);

  r = db_cmd(db, 0, "hget %s receiver", RDB(db)->key);
  if (r)
    goto exit;

  if (RDB(db)->rdReply->type != REDIS_REPLY_STRING) {
    r = DB_ERR_ERROR;
    if (RDB(db)->rdReply->type == REDIS_REPLY_NIL) {
      DBG("User not found!");
      r = DB_ERR_NOTFOUND;
    } else {
      ERR("Not string receiver id %i", RDB(db)->rdReply->type);
    }
  }

exit:

  if ((!r)&&(receiver))
    strncpy(receiver, RDB(db)->rdReply->str, len);

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}

int db_set_event_data(db_t *db, const char *receiver, uint64_t etime, const char *edata)
{
  int r = 0;
  const char *argv[3];
  size_t arglen[3];

  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key),
           "%s:%s:%"PRIu64, RDB(db)->prefix, receiver, etime);

  argv[0]="set";
  argv[1]=RDB(db)->key;
  argv[2]=edata;

  arglen[0]=3;
  arglen[1]=strlen(RDB(db)->key);
  arglen[2]=strlen(edata);

  r = db_cmd_arg(db, 0, 3, argv, arglen);
  if (r)
    goto exit;

  r = db_cmd(db, 0, "expire %s %"PRIu32, RDB(db)->key, RDB(db)->event_expire_timer);
  if (r)
    goto exit;

exit:

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}

int db_get_event_data(db_t *db, const char *receiver, uint64_t etime, char *edata, uint16_t len)
{
  int r = 0;
  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key),
           "%s:%s:%"PRIu64, RDB(db)->prefix, receiver, etime);

  r = db_cmd(db, 0, "get %s", RDB(db)->key);

  if (r)
    goto exit;

  if (RDB(db)->rdReply->type != REDIS_REPLY_STRING) {
    r = 1;
    if (RDB(db)->rdReply->type == REDIS_REPLY_NIL) {
      r = DB_ERR_NOTFOUND;
      DBG("Event not found!");
    } else {
      ERR("Not string event data %i", RDB(db)->rdReply->type);
    }
  }

exit:

  if ((!r)&&(edata))
    strncpy(edata, RDB(db)->rdReply->str, len);

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}

int db_get_events_times_list(db_t *db, const char *receiver, uint64_t *eitem, uint16_t *cnt)
{
  int r = 0;
  uint16_t i, rcnt=0;
  char *c=0;

  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key),
           "%s:%s:*", RDB(db)->prefix, receiver);

  r = db_cmd(db, REDIS_REPLY_ARRAY, "keys %s", RDB(db)->key);
  if (r)
    goto exit;

  DBGL(3,"reply items: %i", RDB(db)->rdReply->elements);

  /* get minimum size */
  if ((*cnt) < RDB(db)->rdReply->elements) {
    ERR("To many events > %u", (*cnt));
  } else
    (*cnt) = RDB(db)->rdReply->elements;

  for (i=0; i<(*cnt); ++i) {
    c = strrchr(RDB(db)->rdReply->element[i]->str, ':');
    if (!c)
      continue;
    if (ut_s2nll10(c+1, &eitem[rcnt]))
      continue;
    rcnt++;
  }

exit:

  (*cnt) = rcnt;

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}


int dm_set_event_lasttime(db_t *db, const char *receiver, uint64_t utime)
{
  int r=0;

  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key),
           "%s:%s:lasttime", RDB(db)->prefix, receiver);

  r = db_cmd(db, 0, "set %s %"PRIu64, RDB(db)->key, utime);

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}

int dm_get_event_lasttime(db_t *db, const char *receiver, uint64_t *utime)
{
  int r=0;

  db_lock(db);

  snprintf(RDB(db)->key, sizeof(RDB(db)->key),
           "%s:%s:lasttime", RDB(db)->prefix, receiver);

  r = db_cmd(db, REDIS_REPLY_STRING, "get %s", RDB(db)->key);
  if (r)
    goto exit;

  if (ut_s2nll10(RDB(db)->rdReply->str, utime)) {
    r = 1;
    ERR("Wrong last event time '%s'", RDB(db)->rdReply->str);
  }

exit:

  db_free_lastreply(db);
  db_unlock(db);

  return r;
}
