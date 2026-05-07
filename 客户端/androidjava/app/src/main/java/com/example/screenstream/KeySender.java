package com.example.screenstream;

import android.view.KeyEvent;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import android.util.Log;

public class KeySender {
    private static final String TAG = "KeySender";
    private Socket socket;
    private OutputStream out;
    private volatile boolean running;
    private BlockingQueue<byte[]> sendQueue = new LinkedBlockingQueue<>();
    private Thread senderThread;

    private static int androidToLinuxKeyCode(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_HOME: return 102;
            case KeyEvent.KEYCODE_BACK: return 158;
            case KeyEvent.KEYCODE_DPAD_UP: return 103;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 108;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 105;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 106;
            case KeyEvent.KEYCODE_DPAD_CENTER: return 28;
            case KeyEvent.KEYCODE_ENTER: return 28;
            case KeyEvent.KEYCODE_VOLUME_UP: return 115;
            case KeyEvent.KEYCODE_VOLUME_DOWN: return 114;
            case KeyEvent.KEYCODE_POWER: return 116;
            case KeyEvent.KEYCODE_MENU: return 139;
            default: return keyCode;
        }
    }

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

    public void sendKey(int androidKeyCode, boolean down) {
        if (!running) return;
        int linuxCode = androidToLinuxKeyCode(androidKeyCode);
        int active = down ? 1 : 0;
        // 小端序打包：8字节 = 2个int
        ByteBuffer buffer = ByteBuffer.allocate(12);
        buffer.order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(8);   // size 字段（小端：08 00 00 00）
        buffer.putInt(linuxCode);
        buffer.putInt(active);
        sendQueue.offer(buffer.array());
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
