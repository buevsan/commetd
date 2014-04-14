#include "json-parser.h"

void json_add_string(json_object *o, const char *key, const char *value)
{
  json_object_object_del(o, key);
  json_object *no = json_object_new_string(value);
  json_object_object_add(o, key, no);
}

json_object *json_getobj(json_object *obj, char *key, json_type type)
{
  json_object *o;
  o = json_object_object_get(obj, key);
  if (json_object_get_type(o)!=type)
    return 0;
  return o;
}

const char *json_get_string(json_object *req, char *key)
{
  json_object *obj = dm_json_getobj(req, key, json_type_string);
  if (!obj)
    return 0;
  return json_object_get_string(obj);
}

int json_check(json_object *obj, json_obj_check_item_t *table)
{
  int i=0;
  json_object *o;
  while (table[i].name) {
    o = dm_json_getobj(obj, table[i].name, table[i].type);
    if (!o) {
      /*ERR("Wrong or absent '%s' json key", table[i].name);*/
      return 1;
    }
    if (table[i].next)
      if (dm_json_check(o, table[i].next))
        return 1;
    i++;
  }
  return 0;
}




