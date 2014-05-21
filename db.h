#ifndef DB_H
#define DB_H
#include <stdint.h>


typedef struct {
  void *d;
} db_t;

int db_init(db_t *db);
void db_free(db_t *db);

void db_setlog(db_t *db, void *log);
void db_setdbnum(db_t *db, uint16_t dbnum);
void db_set_prefix(db_t *db, char *prefix);
void db_set_event_expire_timer(db_t *db, uint32_t t);
void db_set_user_expire_timer(db_t *db, uint32_t t);

int db_connect(db_t *db, char *hostname, uint16_t port);
int db_disconnect(db_t *db);
int db_create_user(db_t *db, const char *hash, const char *receiver);
int db_del_user(db_t *db, const char *hash, const char *receiver);
int db_get_user_hash(db_t *db, const char *receiver, char *hash, uint16_t len);
int db_get_user_receiver(db_t *db, const char *hash, char *receiver, uint16_t len);

int db_set_event_data(db_t *db, const char *receiver, uint64_t etime, const char *data);
int db_get_event_data(db_t *db, const char *receiver, uint64_t etime, char *edata, uint16_t len);

int db_get_events_times_list(db_t *db, const char *receiver, uint64_t *eitem, uint16_t *cnt);



#endif
