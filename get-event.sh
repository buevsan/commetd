#!/bin/sh



if [ ! -z $1 ] ; then
  while [ ! -z $1 ] ;  do
   ./commet-cli -d3 -j -c '{"cmd":"GetEvent","receiver":"22"}'
  done
else
   ./commet-cli -d3 -j -c '{"cmd":"GetEvent","receiver":"22"}'
fi 
