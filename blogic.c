#include "blogic.h"
#include "debug.h"


#define BLD(db_ptr) ((bl_vars_t*)db_ptr->d)

#define ERR(format, ...) debug_print(BLD(bl)->log, 0, "ERR: "format"\n", ##__VA_ARGS__)
#define MSG(format, ...) debug_print(BLD(bl)->log, 0, format"\n", ##__VA_ARGS__)
#define LOG(format, ...) MSG("Commetd: "format, ##__VA_ARGS__)

#ifdef BL_DEBUG
#define DBGL(level, format, ...) debug_print(BLD(bl)->log, 1+level, "DBG: %lu: %s: "format"\n", pthread_self(),__FUNCTION__, ##__VA_ARGS__)
#define DBG(format, ...) DBGL(1, format, ##__VA_ARGS__);
#else
#define DBG(format, ...)
#define DBGL(l, ...)
#endif


typedef int (*do_cmd_fun_t)(bl_t *, json_object *, json_object**);

typedef struct {
  char *name;
  do_cmd_fun_t do_cmd;
  uint32_t flags;
} bl_cmd_table_item_t;

typedef enum {CMFL_IFACEFCGI=1, CMFL_IFACECLI=2} event_flags_t;

bl_cmd_table_item_t cm_str_table[]={
  { "SetEvent", dm_do_set_event, CMFL_IFACECLI },
  { "GetEvent", dm_do_get_event, CMFL_IFACECLI | CMFL_IFACEFCGI },
  { "CreateUser", dm_do_create_user, CMFL_IFACECLI },
  { "DeleteUser", dm_do_delete_user, CMFL_IFACECLI },
  { "GetNow", dm_do_getnow, CMFL_IFACEFCGI },
  { 0, 0, 0 }
};

typedef struct {
  uint8_t *buf;
  uint64_t *etimes;
  db_t *db;
  uint8_t *stop;
  uint32_t wait_event_timer;
  char *cookiename;
  dbg_desc_t *log;
  bl_cmd_table_item_t *cm_table;
} bl_vars_t;


bl_cmd_table_item_t *dm_find_json_cmd(bl_cmd_table_item_t *t, const char *cm)
{
  int i = 0;
  while (t[i].name) {
    if (!strcasecmp(cm, t[i].name))
      return &t[i];
    i++;
  }
  return 0;
}

int dm_process_json_cmd(bl_t *bl, json_object *req, json_object **ans)
{
  json_object *cmd_obj=0;
  const char *cmd, *iface;
  uint32_t mask;
  int r=0;

  if (!req) {
    r = ERR_WRONG_SYNTAX;
    goto exit;
  }

  dm_json_print("", req);

  if (dm_json_check(req, gen_items)) {
    r = ERR_WRONG_MANDATORY_ITEM;
    goto exit;
  }

  cmd = json_get_string(req, "cmd");

  DBG("cmd: '%s'", cmd);

  json_cmd_table_item_t *cm = dm_find_json_cmd(cm_str_table, cmd);

  if (!cm) {
    r=ERR_UNKNOWN_COMMAND;
    goto exit;
  }

  /* check iface allowed for command */
  if (!dm_vars.prm.no_check_iface) {
    iface = json_get_string(req, "interface");
    mask = 0;
    if (strcmp(iface, "cli")==0)
      mask |= CMFL_IFACECLI;
    if (strcmp(iface, "fcgi")==0)
      mask |= CMFL_IFACEFCGI;

    if (!(cm->flags & mask)) {
      DBG("wrong iface '%s' for command '%s'", iface, cmd);
      r= ERR_ACCESS;
      goto exit;
    }
  }

  /* execute command */
  cm->do_cmd(bp, req, ans);

exit:

  if (!(*ans))
    (*ans)=dm_mk_jsonanswer(r);
  DBG("ans %s", json_object_to_json_string(*ans));

  if (cmd_obj)
    json_object_put(cmd_obj);

  return r;
}
