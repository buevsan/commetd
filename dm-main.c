#include <stdio.h>
#include "fcgi_config.h"
#include "fcgiapp.h"
#include "json-parser.h"

int main(int argc, char **argv)
{
  printf("Hi! I am the daemon :)\n");


  /* JSON test */
  char * string = "{\"sitename\" : \"joys of programming\","
                     "\"categories\" : [ \"c\" , [\"c++\" , \"c\" ], \"java\", \"PHP\" ],"
                     "\"author-details\": { \"admin\": false, \"name\" : \"Joys of Programming\", \"Number of Posts\" : 10 } "
                     "}";
  printf("JSON string: %sn", string);
  json_object * jobj = json_tokener_parse(string);     
  json_parse(jobj);

 /* FCGI test */

 FCGX_Init();

}

