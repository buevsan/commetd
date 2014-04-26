#ifndef DB_H
#define DB_H
#include <stdint.h>

typedef struct {
  void *d;
} db_t;

int db_init(db_t *db);
void db_free(db_t *db);

int db_cmd(db_t *db, const char *, ...);
int db_set_cmd(db_t *db, const char *, ...);
int db_get_str_cmd(db_t *db, const char *, ...);
int db_get_list_cmd(db_t *db, const char *, ...);

int db_create_user(db_t *db, const char *hash, const char *receiver);
int db_del_user(db_t *db, const char *hash, const char *receiver);
int db_get_user_hash(db_t *db, const char *receiver, char *hash, uint16_t len);
int db_get_user_receiver(db_t *db, const char *hash, char *receiver, uint16_t len);

int db_set_event_data(db_t *db, const char *receiver, const char *etime, const char *data);
int db_get_event_data(db_t *db, const char *receiver, char *data, uint16_t len);

#endif
