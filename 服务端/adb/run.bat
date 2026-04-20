adb push server.tar.gz /data/local/tmp 
adb shell tar -cvf /data/local/tmp/server.tar.gz -C  /data/local/tmp 
adb shell  sh  /data/local/tmp/server.sh