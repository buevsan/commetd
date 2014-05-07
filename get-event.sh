#!/bin/sh



cm='{"cmd":"GetEvent","receiver":"22"}'
cookie='PHPSESSID=h123456'
urlprms='receiver=22'  

cm()
{
  if [ -z $2 ] ; then
    ./commet-cli -d3 -j -c $1
  else
    curl -b $cookie http://localhost/?$urlprms 
  fi
}


if [ $1 -eq 1 ] ; then
  while [ true ] ;  do
   cm $cm $2
  done
else
   cm $cm $2
fi 
