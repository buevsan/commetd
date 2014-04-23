#!/bin/sh


while [ ! -z $1 ]  ; do

./commet-cli -d3 -j -c '{"cmd":"SetEvent","receiver":"22","event_type":"mes","edata":"qq" }'

done
