package com.example.screenstream;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.view.Surface;
import android.util.Log;
import java.io.InputStream;
import java.net.Socket;
import java.nio.ByteBuffer;

public class VideoPlayer implements Runnable {
    private static final String TAG = "VideoPlayer";
    private String ip;
    private int port;
    private Surface surface;
    private volatile boolean running;
    private Socket socket;
    private OnVideoSizeListener onSizeListener;

    public interface OnVideoSizeListener {
        void onSize(int width, int height);
    }

    public VideoPlayer(String ip, int port, Surface surface, OnVideoSizeListener listener) {
        this.ip = ip;
        this.port = port;
        this.surface = surface;
        this.onSizeListener = listener;
    }

    public void start() {
        running = true;
        new Thread(this).start();
    }

    public void stop() {
        running = false;
        try { if (socket != null) socket.close(); } catch (Exception e) {}
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
        MediaCodec codec = null;
        try {
            socket = new Socket(ip, port);
            socket.setTcpNoDelay(true);
            InputStream is = socket.getInputStream();

            // 1. 读取 69 字节固定头
            byte[] header = new byte[69];
            readFully(is, header, 0, 69);

            // 2. 读取视频宽高（网络序）
            byte[] wb = new byte[4];
            byte[] hb = new byte[4];
            readFully(is, wb, 0, 4);
            readFully(is, hb, 0, 4);
            int width = ((wb[0] & 0xFF) << 24) | ((wb[1] & 0xFF) << 16) |
                ((wb[2] & 0xFF) << 8)  | (wb[3] & 0xFF);
            int height = ((hb[0] & 0xFF) << 24) | ((hb[1] & 0xFF) << 16) |
                ((hb[2] & 0xFF) << 8)  | (hb[3] & 0xFF);

            // 3. 回调视频分辨率（发送端原始尺寸）
            if (onSizeListener != null) {
                onSizeListener.onSize(width, height);
            }

            // 配置 MediaCodec
            codec = MediaCodec.createDecoderByType("video/avc");
            MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
            codec.configure(format, surface, null, 0);
            codec.start();

            byte[] ptsBuf = new byte[8];
            byte[] lenBuf = new byte[4];
            byte[] dataBuf = new byte[1024 * 1024*6];
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

            while (running) {
                // 读取 PTS (8字节)
                readFully(is, ptsBuf, 0, 8);
                // 读取长度 (4字节，网络序)
                readFully(is, lenBuf, 0, 4);
                int length = ((lenBuf[0] & 0xFF) << 24) | ((lenBuf[1] & 0xFF) << 16) |
                    ((lenBuf[2] & 0xFF) << 8)  | (lenBuf[3] & 0xFF);
                if (length <= 0 || length > dataBuf.length) continue;
                readFully(is, dataBuf, 0, length);

                int inIndex = codec.dequeueInputBuffer(10000);
                if (inIndex >= 0) {
                    ByteBuffer buffer = codec.getInputBuffer(inIndex);
                    buffer.clear();
                    buffer.put(dataBuf, 0, length);
                    codec.queueInputBuffer(inIndex, 0, length, 0, 0);
                }

                int outIndex = codec.dequeueOutputBuffer(info, 10000);
                if (outIndex >= 0) {
                    codec.releaseOutputBuffer(outIndex, true);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "Video decode error", e);
        } finally {
            if (codec != null) {
                try { codec.stop(); } catch (Exception e) {}
                try { codec.release(); } catch (Exception e) {}
            }
            try { if (socket != null) socket.close(); } catch (Exception e) {}
        }
    }
}
