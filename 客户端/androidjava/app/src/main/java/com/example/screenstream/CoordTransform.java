package com.example.screenstream;

public class CoordTransform {
    // sender: 本地窗口分辨率（从 SurfaceView 获取）
    private static int senderWidth = 1920;
    private static int senderHeight = 1080;
    // receiver: 服务端屏幕分辨率（根据视频分辨率和 XY_SWAP_MODE 计算）
    private static int receiverWidth = 2376;
    private static int receiverHeight = 1080;
    private static int xySwapMode = 0;   // 全局模式，应与 C 端一致

    public static void setSenderSize(int w, int h) {
        senderWidth = w;
        senderHeight = h;
    }

    public static void setReceiverSize(int w, int h) {
        receiverWidth = w;
        receiverHeight = h;
    }

    public static void setSwapMode(int mode) {
        xySwapMode = mode;
    }

    public static int getSenderWidth() { return senderWidth; }
    public static int getSenderHeight() { return senderHeight; }
    public static int getReceiverWidth() { return receiverWidth; }
    public static int getReceiverHeight() { return receiverHeight; }

    public static int mapX(int x, int y) {
        switch (xySwapMode) {
            case 0:  // 不转换，直接缩放
                return x * receiverWidth / senderWidth;
            case 1:  // 竖屏转横屏：Y -> X
                return receiverWidth - y * receiverWidth / senderHeight;
            case 2:  // 只交换，不缩放
                return y;
            default:
                return x * receiverWidth / senderWidth;
        }
    }

    public static int mapY(int x, int y) {
        switch (xySwapMode) {
            case 0:
                return y * receiverHeight / senderHeight;
            case 1:
                return x * receiverHeight / senderWidth;
            case 2:
                return x;
            default:
                return y * receiverHeight / senderHeight;
        }
    }
}
