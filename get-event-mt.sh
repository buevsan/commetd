#!/bin/sh


while [ ! -z $1 ] ; do
./commet-cli -d3 -j -c '{"cmd":"GetEvent","receiver":"22","min_event_time":"123"}'
done
