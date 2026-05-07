package com.example.screenstream;

import android.util.Log;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;

public class TouchSender {
    private static final String TAG = "TouchSender";
    private Socket socket;
    private OutputStream out;
    private volatile boolean running;
    private BlockingQueue<byte[]> sendQueue = new LinkedBlockingQueue<>();
    private Thread senderThread;

    public boolean connect(String ip, int port) {
        try {
            socket = new Socket(ip, port);
            socket.setTcpNoDelay(true);
            out = socket.getOutputStream();
            running = true;
            senderThread = new Thread(new Runnable() {
                    @Override
                    public void run() {
                        while (running) {
                            try {
                                byte[] data = sendQueue.take();
                                if (data.length >= 4) {
                                    // 调试：打印前4字节（小端序下应该是 10 00 00 00）
                                    Log.d(TAG, "Sending " + data.length + " bytes, hex: " + bytesToHex(data));
                                }
                                out.write(data);
                                out.flush();
                            } catch (InterruptedException e) {
                                break;
                            } catch (Exception e) {
                                Log.e(TAG, "Send error", e);
                                close();
                                break;
                            }
                        }
                    }
                });
            senderThread.start();
            Log.i(TAG, "Connected to " + ip + ":" + port);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "Connect failed", e);
            return false;
        }
    }

    public void sendTouch(int id, int x, int y, int active) {
        if (!running) return;
        // 使用小端序 (Little Endian) 打包：总共20字节 = 5个int
        ByteBuffer buffer = ByteBuffer.allocate(20);
        buffer.order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(16);   // size 字段（小端：10 00 00 00）
        buffer.putInt(id);
        buffer.putInt(x);
        buffer.putInt(y);
        buffer.putInt(active);
        sendQueue.offer(buffer.array());
    }

    private static String bytesToHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) {
            sb.append(String.format("%02X ", b));
        }
        return sb.toString();
    }

    public void close() {
        running = false;
        if (senderThread != null) {
            senderThread.interrupt();
            try { senderThread.join(100); } catch (InterruptedException e) {}
        }
        try { if (out != null) out.close(); } catch (Exception e) {}
        try { if (socket != null) socket.close(); } catch (Exception e) {}
        Log.i(TAG, "Closed");
    }
}
