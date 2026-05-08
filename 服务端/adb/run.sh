adb push server.tar.gz /data/local/tmp 
adb shell  tar -xvf  /data/local/tmp/server.tar.gz -C /data/local/tmp
adb shell su -c  ln -s /data/local/tmp/server.sh /data/adb/post-fs-data.d/scrcpy_server.sh && echo 已经开启开机自启
adb shell chmod 0777 /data/local/tmp/server.sh 
adb shell  sh  /data/local/tmp/server.sh