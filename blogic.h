#ifndef BLOGIC_H
#define BLOGIC_H

#include <stdint.h>
#include <json-parser.h>



typedef struct {
  void *d;
} bl_t;

int bl_init(bl_t *db);
void bl_free(bl_t *db);


int dm_process_json_cmd(dm_business_prm_t *bp, json_object *req, json_object **ans);




#endif
