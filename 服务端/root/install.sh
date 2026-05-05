
test -d /system/etc/init.d || test -d /data/adb/post-fs-data.d || exec   echo 手机不支持自启动脚本
initd=/system/etc/init.d
test -d /data/adb/post-fs-data.d && initd=/data/adb/post-fs-data.d
cp  data/local/tmp/*  /data/local/tmp
cp system/etc/init.d/*  "$initd"  || echo 手机不支持自启动
mount -o remount,rw / 
mount -o remount,rw /system
cp system/bin/*  /system/bin && chmod 0777 /system/bin/inin /system/bin/scrcpy-server200 "$initd"/*  /system/bin/video.sh /system/bin/audio.sh
mount -o remount,ro / 
mount -o remount,ro /system
ls /system/bin/inin ||(mkdir /data/bin && cp system/bin/* /data/bin&& chmod -R  0777 /data/bin)
