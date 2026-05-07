package com.example.screenstream;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import java.io.InputStream;
import java.net.Socket;

public class AudioPlayer implements Runnable {
    private String ip;
    private int port;
    private volatile boolean running;
    private Socket socket;
    private AudioTrack audioTrack;

    public AudioPlayer(String ip, int port) {
        this.ip = ip;
        this.port = port;
    }

    public void start() {
        running = true;
        new Thread(this).start();
    }

    public void stop() {
        running = false;
        try { if (socket != null) socket.close(); } catch (Exception e) {}
        if (audioTrack != null) {
            audioTrack.stop();
            audioTrack.release();
        }
    }

    private void readFully(InputStream is, byte[] buf, int off, int len) throws java.io.IOException {
        while (len > 0) {
            int read = is.read(buf, off, len);
            if (read < 0) throw new java.io.IOException("EOF");
            off += read;
            len -= read;
        }
    }

    @Override
    public void run() {
        try {
            socket = new Socket(ip, port);
            InputStream is = socket.getInputStream();

            // 1. 读取 69 字节头
            byte[] header = new byte[69];
            readFully(is, header, 0, 69);

            // 2. 配置 AudioTrack（与 C 端 OpenSL ES 参数一致）
            int sampleRate = 48000;
            int channelConfig = AudioFormat.CHANNEL_OUT_STEREO;
            int audioFormat = AudioFormat.ENCODING_PCM_16BIT;
            int bufferSize = AudioTrack.getMinBufferSize(sampleRate, channelConfig, audioFormat) * 2;
            audioTrack = new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder()
                                    .setUsage(AudioAttributes.USAGE_MEDIA)
                                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                                    .build())
                .setAudioFormat(new AudioFormat.Builder()
                                .setEncoding(audioFormat)
                                .setSampleRate(sampleRate)
                                .setChannelMask(channelConfig)
                                .build())
                .setBufferSizeInBytes(bufferSize)
                .build();
            audioTrack.play();

            byte[] ptsBuf = new byte[8];
            byte[] lenBuf = new byte[4];
            byte[] pcmBuf = new byte[8192];

            while (running) {
                // 读取 8 字节 PTS
                readFully(is, ptsBuf, 0, 8);
                // 读取 4 字节长度
                readFully(is, lenBuf, 0, 4);
                int length = ((lenBuf[0]&0xFF)<<24)|((lenBuf[1]&0xFF)<<16)|
                    ((lenBuf[2]&0xFF)<<8)|(lenBuf[3]&0xFF);
                if (length <= 0) continue;
                if (length > pcmBuf.length) {
                    pcmBuf = new byte[length];
                }
                readFully(is, pcmBuf, 0, length);
                // 写入 AudioTrack 播放
                audioTrack.write(pcmBuf, 0, length);
            }
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
