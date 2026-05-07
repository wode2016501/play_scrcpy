package com.example.screenstream;

import android.os.Environment;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

public class Config {
    public static String SERVER_IP = "192.168.100.1";
    public static final int TOUCH_PORT = 9000;
    public static final int VIDEO_PORT = 9999;
    public static final int AUDIO_PORT = 9998;

    public static void loadIpFromFile() {
        File file = new File(Environment.getExternalStorageDirectory(), "scrcpy.txt");
        if (file.exists()) {
            FileInputStream fis = null;
            try {
                fis = new FileInputStream(file);
                byte[] data = new byte[20];
                int len = fis.read(data);
                if (len > 0) {
                    String ip = new String(data, 0, len).trim();
                    if (ip.matches("\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}")) {
                        SERVER_IP = ip;
                    }
                }
            } catch (Exception ignored) {
            } finally {
                if (fis != null) {
                    try { fis.close(); } catch (IOException ignored) {}
                }
            }
        }
    }
}
