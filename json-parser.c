#include "json-parser.h"

json_object *json_new_string(const char *key, const char *value)
{
  json_object * obj;
  obj=json_object_new_object();
  json_object_object_add(obj, key, json_object_new_string(value));
  return obj;
}

void json_add_string(json_object *o, const char *key, const char *value)
{
  json_object_object_del(o, key);
  json_object_object_add(o, key, json_object_new_string(value));
}

json_object *json_getobj(json_object *obj, char *key, json_type type)
{
  json_object *o;
  o = json_object_object_get(obj, key);
  if (json_object_get_type(o)!=type)
    return 0;
  return o;
}

const char *json_get_string(json_object *o, char *key)
{
  json_object *obj = json_getobj(o, key, json_type_string);
  if (!obj)
    return 0;
  return json_object_get_string(obj);
}

int json_check(json_object *obj, json_obj_check_item_t *table)
{
  int i=0, r;
  json_object *o;
  while (table[i].name) {
    o = json_getobj(obj, table[i].name, table[i].type);
    if (!o) {
      /*ERR("Wrong or absent '%s' json key", table[i].name);*/
      return 1;
    }
    if (table[i].next) {
      r = json_check(o, table[i].next);
      if (r)
        return r;
    }
    i++;
  }
  return 0;
}


json_object * json_parse(const char *s, json_type type)
{
  json_object *o;
  o = json_tokener_parse(s);
  if (!o)
    return 0;

  if (json_object_get_type(o)!=type)
  {
    json_object_put(o);
    return 0;
  }

  return o;
}
