#!/bin/sh



if [ ! -z $1 ] ; then
  while true  ; do
   
  ./commet-cli -d3 -j -c '{"cmd":"SetEvent","receiver":"22","event_type":"mes","edata":"qq" }'
    #sleep 0.2
  done
else

  ./commet-cli -d3 -j -c '{"cmd":"SetEvent","receiver":"22","event_type":"mes","edata":"qq" }'
fi



