package com.example.screenstream;

import android.app.Activity;
import android.content.res.Configuration;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.view.WindowManager;
import android.os.Build;
import android.view.ViewGroup;
import android.view.Window;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static final int XY_SWAP_MODE = 0;   // 必须与 C 端一致

    private RelativeLayout rootLayout;
    private SurfaceView surfaceView;
    private TextView statusTextView;
    private TouchSender touchSender;
    private KeySender keySender;
    private VideoPlayer videoPlayer;
    private AudioPlayer audioPlayer;
    private String serverIp;
    private boolean isConnected = false;
    
    private float currentFps = 0;
    private int currentVideoWidth = 0, currentVideoHeight = 0;

    private void updateStatusWithFps() {
        String text = String.format("Video: %dx%d → Screen: %dx%d  FPS: %.1f",
                                    CoordTransform.getSenderHeight(), CoordTransform.getSenderWidth(),
                                    CoordTransform.getReceiverWidth(),
                                    CoordTransform.getReceiverHeight(),
                                    currentFps);
        statusTextView.setText(text);
    }
    
    

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        requestWindowFeature(Window.FEATURE_NO_TITLE);
        super.onCreate(savedInstanceState);

        // 全屏与常亮
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN
                             | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }

        // 读取 IP
        Config.loadIpFromFile();
        serverIp = Config.SERVER_IP;

        // 动态创建布局
        rootLayout = new RelativeLayout(this);
        rootLayout.setLayoutParams(new ViewGroup.LayoutParams(
                                       ViewGroup.LayoutParams.MATCH_PARENT,
                                       ViewGroup.LayoutParams.MATCH_PARENT));

        surfaceView = new SurfaceView(this);
        RelativeLayout.LayoutParams surfParams = new RelativeLayout.LayoutParams(
            RelativeLayout.LayoutParams.MATCH_PARENT,
            RelativeLayout.LayoutParams.MATCH_PARENT);
        surfaceView.setLayoutParams(surfParams);
        rootLayout.addView(surfaceView);

        statusTextView = new TextView(this);
        RelativeLayout.LayoutParams textParams = new RelativeLayout.LayoutParams(
            RelativeLayout.LayoutParams.WRAP_CONTENT,
            RelativeLayout.LayoutParams.WRAP_CONTENT);
        textParams.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        textParams.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        textParams.setMargins(16, 16, 0, 0);
        statusTextView.setLayoutParams(textParams);
        statusTextView.setPadding(8, 8, 8, 8);
        statusTextView.setTextColor(0xFFFFFFFF);
        statusTextView.setTextSize(16);
        statusTextView.setBackgroundColor(0x88000000);
        statusTextView.setText("连接到 " + serverIp + "...");
        rootLayout.addView(statusTextView);

        // 隐藏标题栏（必须在 setContentView 之前）
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        // 全屏与常亮
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN
                             | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
        setContentView(rootLayout);
        

        surfaceView.getHolder().addCallback(this);
        setupTouchListener();
    }

    private void setupTouchListener() {
        surfaceView.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View v, MotionEvent event) {
                    if (touchSender == null) return true;
                    int receiverW = CoordTransform.getReceiverWidth();
                    int receiverH = CoordTransform.getReceiverHeight();
                    if (receiverW == 0 || receiverH == 0) return true;

                    int action = event.getActionMasked();
                    int pointerIndex = event.getActionIndex();
                    int id = event.getPointerId(pointerIndex);
                    int rawX = (int) event.getX(pointerIndex);
                    int rawY = (int) event.getY(pointerIndex);

                    // 坐标转换（内部使用 sender=窗口分辨率，receiver=服务端屏幕尺寸）
                    int mappedX = CoordTransform.mapX(rawX, rawY);
                    int mappedY = CoordTransform.mapY(rawX, rawY);

                    // 边界裁剪
                    if (mappedX < 0) mappedX = 0;
                    if (mappedX >= receiverW) mappedX = receiverW - 1;
                    if (mappedY < 0) mappedY = 0;
                    if (mappedY >= receiverH) mappedY = receiverH - 1;

                    switch (action) {
                        case MotionEvent.ACTION_DOWN:
                        case MotionEvent.ACTION_POINTER_DOWN:
                            touchSender.sendTouch(id, mappedX, mappedY, 0);
                            break;
                        case MotionEvent.ACTION_MOVE:
                            for (int i = 0; i < event.getPointerCount(); i++) {
                                id = event.getPointerId(i);
                                int x = (int) event.getX(i);
                                int y = (int) event.getY(i);
                                int mx = CoordTransform.mapX(x, y);
                                int my = CoordTransform.mapY(x, y);
                                if (mx < 0) mx = 0;
                                if (mx >= receiverW) mx = receiverW - 1;
                                if (my < 0) my = 0;
                                if (my >= receiverH) my = receiverH - 1;
                                touchSender.sendTouch(id, mx, my, 1);
                            }
                            break;
                        case MotionEvent.ACTION_UP:
                        case MotionEvent.ACTION_POINTER_UP:
                            touchSender.sendTouch(id, mappedX, mappedY, 2);
                            break;
                    }
                    return true;
                }
            });
    }

    @Override
    public void surfaceCreated(final SurfaceHolder holder) {
        if (isConnected) return;
        new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        touchSender = new TouchSender();
                        touchSender.connect(serverIp, Config.TOUCH_PORT);
                        runOnUiThread(new Runnable() {
                                @Override
                                public void run() {
                                    statusTextView.setText("Touch connected, waiting video...");
                                }
                            });

                        keySender = new KeySender();
                        keySender.connect(serverIp, Config.TOUCH_PORT);

                        audioPlayer = new AudioPlayer(serverIp, Config.AUDIO_PORT);
                        audioPlayer.start();

                        videoPlayer = new VideoPlayer(serverIp, Config.VIDEO_PORT,
                            holder.getSurface(), new VideoPlayer.OnVideoSizeListener() {
                                @Override
                                public void onSize(final int videoWidth, final int videoHeight) {
                                    runOnUiThread(new Runnable() {
                                            @Override
                                            public void run() {
                                                // 根据 XY_SWAP_MODE 计算接收端分辨率（服务端屏幕尺寸）
                                                if (XY_SWAP_MODE == 1) {
                                                    CoordTransform.setReceiverSize(videoHeight, videoWidth);
                                                } else {
                                                    CoordTransform.setReceiverSize(videoWidth, videoHeight);
                                                }
                                                statusTextView.setText(String.format("Video: %dx%d → Screen: %dx%d",
                                                                                     videoWidth, videoHeight,
                                                                                     CoordTransform.getSenderWidth(),
                                                                                     CoordTransform.getSenderHeight()));
                                            }
                                        });
                                }
                            });
                            
                            
                            
                        // 设置 FPS 监听
                        videoPlayer.setOnFpsListener(new VideoPlayer.OnFpsListener() {
                                @Override
                                public void onFps(final float fps) {
                                    runOnUiThread(new Runnable() {
                                            @Override
                                            public void run() {
                                                currentFps = fps;
                                                updateStatusWithFps();
                                            }
                                        });
                                }
                            });
                        videoPlayer.start();
                        isConnected = true;
                    } catch (final Exception e) {
                        e.printStackTrace();
                        runOnUiThread(new Runnable() {
                                @Override
                                public void run() {
                                    statusTextView.setText("Connection failed: " + e.getMessage());
                                }
                            });
                    }
                }
            }).start();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // 更新本地窗口分辨率（sender）
        CoordTransform.setSenderSize(width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        disconnectAll();
    }

    private void disconnectAll() {
        isConnected = false;
        if (videoPlayer != null) videoPlayer.stop();
        if (audioPlayer != null) audioPlayer.stop();
        if (touchSender != null) touchSender.close();
        if (keySender != null) keySender.close();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keySender != null) keySender.sendKey(keyCode, true);
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keySender != null) keySender.sendKey(keyCode, false);
        return super.onKeyUp(keyCode, event);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        disconnectAll();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // 重新应用全屏标志
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.KITKAT) {
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
}
