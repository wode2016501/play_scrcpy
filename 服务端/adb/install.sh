tar -xvf  server.tar.gz -C /data/local/tmp
ln -s /data/local/tmp/server.sh /data/adb/post-fs-data.d/scrcpy_server.sh
chmod 0777 /data/local/tmp/server.sh