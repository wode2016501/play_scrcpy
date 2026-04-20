#! /system/bin/sh
cp /data/local/tmp/scrcpy-server-manual.jar1 /data/local/tmp/scrcpy-server-manual.jar
CLASSPATH=/data/local/tmp/scrcpy-server-manual.jar app_process / com.genymobile.scrcpy.Server 3.3.3 tunnel_forward=true audio=false control=false cleanup=false video_bit_rate=20000000 scid=1  video_codec_options=profile=8,level=65536 max_fps=60 capture_orientation=@270


