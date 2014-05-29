#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <json-c/json.h>

typedef struct json_obj_check_item_s {
  char *name;
  json_type type;
  struct json_obj_check_item_s *next;
} json_obj_check_item_t;


json_object *json_new_string(const char *key, const char *value);
void json_add_string(json_object *o, const char *key, const char *value);
json_object *json_getobj(json_object *obj, char *key, json_type type);
const char *json_get_string(json_object *req, char *key);
int json_check(json_object *obj, json_obj_check_item_t *table);
json_object * json_parse(const char *s, json_type type);

#endif


