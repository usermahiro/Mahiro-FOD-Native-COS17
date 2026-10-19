package mahiro.fod;

import android.content.Context;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;

import java.lang.reflect.Method;

/**
 * Main-thread side of the native FOD hook.
 *
 * This class is embedded as classes.dex inside libfodhook.so and loaded with
 * InMemoryDexClassLoader. It only uses public Android APIs.
 *
 * It replaces the smali patch:
 *   - FingerKeyReceiver show/hide branches
 *   - createHbmSurfaceControl / destroyHbmSurfaceControl
 *   - setVisibility-hook that destroys a stuck HBM layer
 */
public final class NativeUi {
    private static final String TAG = "fodhook";
    private static final String HBM_TITLE = "RianixiaHBMController";

    private static Handler sMain;
    private static View sHbmView;
    private static WindowManager sWindowManager;
    private static volatile boolean sShownByUs;

    private NativeUi() {
    }

    public static void init() {
        if (sMain == null) {
            sMain = new Handler(Looper.getMainLooper());
        }
        Log.i(TAG, "NativeUi ready");
    }

    /**
     * FINGER_DOWN branch.
     *
     * Matches OnScreenFingerprintIcon$FingerKeyReceiver:
     *   show icon if it was hidden and the screen was off,
     *   create the HBM layer,
     *   call OnScreenFingerprintUiMech.onFpTouch(true).
     */
    public static void onDown(final View icon, final Object mech, final boolean showIcon) {
        ensureHandler();
        sMain.post(new Runnable() {
            @Override
            public void run() {
                try {
                    if (showIcon && icon != null) {
                        postVisibility(icon, View.VISIBLE);
                        sShownByUs = true;
                    }
                    addHbm(icon);
                    touch(mech, true);
                } catch (Throwable t) {
                    Log.e(TAG, "onDown failed", t);
                }
            }
        });
    }

    /**
     * FINGER_UP branch.
     *
     * Matches FingerKeyReceiver:
     *   destroy HBM,
     *   call onFpTouch(false),
     *   hide the icon when the screen was off.
     */
    public static void onUp(final View icon, final Object mech, final boolean hideIcon) {
        ensureHandler();
        sMain.post(new Runnable() {
            @Override
            public void run() {
                try {
                    removeHbm();
                    touch(mech, false);
                    if (hideIcon && icon != null) {
                        postVisibility(icon, View.GONE);
                        sShownByUs = false;
                    }
                } catch (Throwable t) {
                    Log.e(TAG, "onUp failed", t);
                }
            }
        });
    }

    /** Watchdog: icon became non-visible while HBM is still on. */
    public static void destroyHbm() {
        if (sMain == null) {
            return;
        }
        sMain.post(new Runnable() {
            @Override
            public void run() {
                removeHbm();
            }
        });
    }

    /** True while the dummy HBM layer is attached. */
    public static boolean isHbmActive() {
        return sHbmView != null;
    }

    public static boolean wasShownByUs() {
        return sShownByUs;
    }

    private static void ensureHandler() {
        if (sMain == null) {
            init();
        }
    }

    /**
     * Mirrors the smali updateOpticalUI(Runnable) call. If the OPLUS helper is
     * unavailable, fall back to the main looper.
     */
    private static void postVisibility(final View icon, final int visibility) {
        Runnable r = new Runnable() {
            @Override
            public void run() {
                try {
                    icon.setVisibility(visibility);
                } catch (Throwable t) {
                    Log.e(TAG, "setVisibility(" + visibility + ") failed", t);
                }
            }
        };
        try {
            Class<?> c = Class.forName(
                    "com.oplus.systemui.biometrics.finger.KeyguardFingerprintUtils");
            Method m = c.getMethod("updateOpticalUI", Runnable.class);
            m.invoke(null, r);
            return;
        } catch (Throwable ignored) {
            // fall through
        }
        sMain.post(r);
    }

    private static void touch(Object mech, boolean down) {
        if (mech == null) {
            return;
        }
        try {
            Method m = mech.getClass().getMethod("onFpTouch", boolean.class);
            m.setAccessible(true);
            m.invoke(mech, down);
        } catch (Throwable t) {
            Log.e(TAG, "onFpTouch(" + down + ") failed", t);
        }
    }

    /**
     * Same window the smali createHbmSurfaceControl() adds: an alpha-0 View
     * whose WindowManager title makes SurfaceFlinger name the layer
     * "VRI[RianixiaHBMController]". The patched hwcomposer recognizes that
     * name and turns on FULL HBM.
     */
    private static void addHbm(View icon) {
        if (sHbmView != null) {
            Log.w(TAG, "HBM layer already active");
            return;
        }
        if (icon == null) {
            Log.w(TAG, "addHbm: no icon");
            return;
        }
        try {
            Context context = icon.getContext();
            WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
            if (wm == null) {
                Log.e(TAG, "addHbm: no WindowManager");
                return;
            }

            int[] location = new int[2];
            try {
                icon.getLocationOnScreen(location);
            } catch (Throwable ignored) {
                location[0] = 0;
                location[1] = 0;
            }

            int width = icon.getWidth();
            int height = icon.getHeight();
            if (width <= 0) {
                width = 200;
            }
            if (height <= 0) {
                height = 300;
            }

            // 0x7f7 first (the type used by the smali patch), then the public
            // TYPE_APPLICATION_OVERLAY fallback.
            int[] types = {0x7f7, 0x7f6};
            Throwable last = null;
            for (int type : types) {
                WindowManager.LayoutParams lp = new WindowManager.LayoutParams();
                lp.width = width;
                lp.height = height;
                lp.x = location[0];
                lp.y = location[1];
                lp.type = type;
                lp.flags = 0x1080028;
                lp.format = PixelFormat.TRANSLUCENT;
                lp.gravity = 0x33;
                lp.setTitle(HBM_TITLE);

                View view = new View(context);
                view.setAlpha(0.0f);
                try {
                    wm.addView(view, lp);
                    sHbmView = view;
                    sWindowManager = wm;
                    Log.i(TAG, "HBM layer added (type=0x" + Integer.toHexString(type)
                            + ") " + width + "x" + height
                            + " @" + location[0] + "," + location[1]);
                    return;
                } catch (Throwable t) {
                    last = t;
                }
            }
            throw last;
        } catch (Throwable t) {
            Log.e(TAG, "addHbm failed", t);
            sHbmView = null;
            sWindowManager = null;
        }
    }

    private static void removeHbm() {
        if (sHbmView == null) {
            return;
        }
        try {
            sWindowManager.removeView(sHbmView);
            Log.i(TAG, "HBM layer removed");
        } catch (Throwable t) {
            Log.e(TAG, "removeHbm failed", t);
        }
        sHbmView = null;
        sWindowManager = null;
    }
}
