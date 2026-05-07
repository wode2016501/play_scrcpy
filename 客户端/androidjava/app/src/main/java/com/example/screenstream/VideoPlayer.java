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
    private OnFpsListener onFpsListener;

    public interface OnVideoSizeListener {
        void onSize(int width, int height);
    }

    public interface OnFpsListener {
        void onFps(float fps);
    }

    public VideoPlayer(String ip, int port, Surface surface, OnVideoSizeListener listener) {
        this.ip = ip;
        this.port = port;
        this.surface = surface;
        this.onSizeListener = listener;
    }

    public void setOnFpsListener(OnFpsListener listener) {
        this.onFpsListener = listener;
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
        int frameCount = 0;
        long lastTime = System.nanoTime();

        try {
            socket = new Socket(ip, port);
            socket.setTcpNoDelay(true);
            InputStream is = socket.getInputStream();

            // 固定头69字节
            byte[] header = new byte[69];
            readFully(is, header, 0, 69);

            // 读取宽高
            byte[] wb = new byte[4];
            byte[] hb = new byte[4];
            readFully(is, wb, 0, 4);
            readFully(is, hb, 0, 4);
            int width = ((wb[0] & 0xFF) << 24) | ((wb[1] & 0xFF) << 16) |
                ((wb[2] & 0xFF) << 8)  | (wb[3] & 0xFF);
            int height = ((hb[0] & 0xFF) << 24) | ((hb[1] & 0xFF) << 16) |
                ((hb[2] & 0xFF) << 8)  | (hb[3] & 0xFF);

            if (onSizeListener != null) {
                onSizeListener.onSize(width, height);
            }
            CoordTransform.setReceiverSize(width,height);

            codec = MediaCodec.createDecoderByType("video/avc");
            MediaFormat format = MediaFormat.createVideoFormat("video/avc", width, height);
            codec.configure(format, surface, null, 0);
            codec.start();

            byte[] ptsBuf = new byte[8];
            byte[] lenBuf = new byte[4];
            byte[] dataBuf = new byte[1024 * 1024];
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();

            while (running) {
                readFully(is, ptsBuf, 0, 8);
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
                    // 统计帧数
                    frameCount++;
                    long now = System.nanoTime();
                    if (now - lastTime >= 1_000_000_000L) { // 1秒
                        float fps = frameCount * 1_000_000_000f / (now - lastTime);
                        if (onFpsListener != null) {
                            onFpsListener.onFps(fps);
                        }
                        frameCount = 0;
                        lastTime = now;
                    }
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
