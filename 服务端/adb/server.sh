#! /system/bin/sh 
export PATH=/data/local/tmp/system/bin:$PATH 
chmod -R 0777  /data/local/tmp/* 
for i in /data/local/tmp/system/etc/init.d/*;do 
test -f "$i" && ($i >/dev/null &)
done
