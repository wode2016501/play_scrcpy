
test -d /system/etc/init.d || test -d /data/adb/post-fs-data.d || exec   echo 手机不支持自启动脚本
initd=/system/etc/init.d
test -d /data/adb/post-fs-data.d && initd=/data/adb/post-fs-data.d
cp -a data/local/tmp /data/local
cp system/etc/init.d/*  "$initd"  || echo 手机不支持自启动
cp system/bin/*  /system/bin
chmod 0777 /system/bin/inin /system/bin/scrcpy-server200 "$initd"/*  /system/bin/video.sh /system/bin/audio.sh
