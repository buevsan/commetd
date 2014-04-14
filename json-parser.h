#ifndef JSON_PARSER_H
#define JSON_PARSER_H

#include <json/json.h>

typedef struct json_obj_check_item_s {
  char *name;
  json_type type;
  struct json_obj_check_s *next;
} json_obj_check_item_t;
 
#endif


