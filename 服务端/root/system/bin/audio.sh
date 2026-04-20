#! /system/bin/sh
cp /data/local/tmp/scrcpy-server-manual.jar1 /data/local/tmp/scrcpy-server-manual.jar
 CLASSPATH=/data/local/tmp/scrcpy-server-manual.jar app_process / com.genymobile.scrcpy.Server 3.3.3 tunnel_forward=true video=false control=false cleanup=false audio_codec=raw scid=2 


