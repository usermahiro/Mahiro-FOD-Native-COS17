// libfodhook.so - native replacement for the COS17 FOD smali patch.
//
// Loaded into com.android.systemui by fodinject. It:
//   1. finds the live OnScreenFingerprintIcon + OnScreenFingerprintUiMech
//   2. listens on the abstract unix socket @fodhook (fed by fodbridged)
//   3. on finger down: shows the icon (screen-off), creates the
//      "RianixiaHBMController" window (patched hwcomposer turns on HBM) and
//      calls OnScreenFingerprintUiMech.onFpTouch(true)
//   4. on finger up: removes the HBM window, calls onFpTouch(false), hides the
//      icon when the screen was off
//   5. destroys HBM if the system hides the icon while HBM is still active
//   6. forces KeyguardFeatureOption.isDisableAppDimLayer = true
//   7. optional: limits peak/min refresh rate to 120Hz while FOD is active
//
// The UI work runs in a tiny embedded classes.dex (mahiro.fod.NativeUi) so it
// is executed on SystemUI's main thread, exactly like the smali receiver did.

#include <jni.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <errno.h>
#include <atomic>
#include <string>
#include <android/log.h>
#include <sys/system_properties.h>

#include "helper_dex.h"

#define LOG_TAG "fodhook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char* ICON_TYPE =
    "com.oplus.systemui.biometrics.finger.udfps.OnScreenFingerprintIcon";
static const char* MECH_TYPE =
    "com.oplus.systemui.biometrics.finger.udfps.OnScreenFingerprintUiMech";
static const char* KFU_TYPE =
    "com.oplus.systemui.biometrics.finger.KeyguardFingerprintUtils";
static const char* OPT_TYPE =
    "com.oplusos.systemui.common.feature.KeyguardFeatureOption";
static const char* HBM_TITLE = "RianixiaHBMController";

static JavaVM* gVm = nullptr;
static jobject gApp = nullptr;
static jobject gCL = nullptr;
static jmethodID gLoadClass = nullptr;

static jclass gHelperCls = nullptr;
static jmethodID mHelperDown = nullptr;
static jmethodID mHelperUp = nullptr;
static jmethodID mHelperDestroy = nullptr;

static jclass gIconCls = nullptr;
static jobject gIcon = nullptr;
static jobject gMech = nullptr;
static jmethodID mGetVisibility = nullptr;
static jmethodID mIsAttached = nullptr;
static jmethodID mScreenOff = nullptr;

static jclass gSysCls = nullptr;
static jmethodID mSysGetFloat = nullptr;
static jmethodID mSysPutFloat = nullptr;
static jmethodID mGetContentResolver = nullptr;

static std::atomic<bool> gHbmOn{false};
static bool gSawVisible = false;
static bool gRateSaved = false;
static float gSavedPeak = 144.0f;
static float gSavedMin = 0.0f;
static bool gDimForced = false;

// ---------------------------------------------------------------- JNI utils

static bool clr(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

static std::string classNameOf(JNIEnv* env, jobject obj) {
    if (!obj) return "";
    jclass cClass = env->FindClass("java/lang/Class");
    jmethodID mGetName = env->GetMethodID(cClass, "getName", "()Ljava/lang/String;");
    jobject c = env->GetObjectClass(obj);
    jstring s = (jstring)env->CallObjectMethod(c, mGetName);
    env->DeleteLocalRef(c);
    if (clr(env) || !s) return "";
    const char* u = env->GetStringUTFChars(s, nullptr);
    std::string out = u ? u : "";
    if (u) env->ReleaseStringUTFChars(s, u);
    env->DeleteLocalRef(s);
    return out;
}

static jclass loadClass(JNIEnv* env, const char* name) {
    if (!gCL || !gLoadClass) return nullptr;
    jstring s = env->NewStringUTF(name);
    jobject c = env->CallObjectMethod(gCL, gLoadClass, s);
    env->DeleteLocalRef(s);
    if (clr(env) || !c) return nullptr;
    return (jclass)c;
}

static jobject callNoArg(JNIEnv* env, jobject obj, const char* name) {
    jclass cClass = env->FindClass("java/lang/Class");
    jclass cMethod = env->FindClass("java/lang/reflect/Method");
    jmethodID mGetMethod = env->GetMethodID(cClass, "getMethod",
        "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    jmethodID mInvoke = env->GetMethodID(cMethod, "invoke",
        "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    jclass cObj = env->GetObjectClass(obj);
    jstring s = env->NewStringUTF(name);
    jobject m = env->CallObjectMethod(cObj, mGetMethod, s, (jobjectArray)nullptr);
    env->DeleteLocalRef(s);
    if (clr(env) || !m) return nullptr;
    jobject r = env->CallObjectMethod(m, mInvoke, obj, (jobjectArray)nullptr);
    if (clr(env)) return nullptr;
    return r;
}

// ------------------------------------------------------- field discovery

static jobject fieldOfType(JNIEnv* env, jobject obj, const char* typeName) {
    jclass cClass = env->FindClass("java/lang/Class");
    jclass cField = env->FindClass("java/lang/reflect/Field");
    jmethodID mGetDF = env->GetMethodID(cClass, "getDeclaredFields",
        "()[Ljava/lang/reflect/Field;");
    jmethodID mSuper = env->GetMethodID(cClass, "getSuperclass", "()Ljava/lang/Class;");
    jmethodID mCName = env->GetMethodID(cClass, "getName", "()Ljava/lang/String;");
    jmethodID mType = env->GetMethodID(cField, "getType", "()Ljava/lang/Class;");
    jmethodID mAcc = env->GetMethodID(cField, "setAccessible", "(Z)V");
    jmethodID mGet = env->GetMethodID(cField, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    jclass c = env->GetObjectClass(obj);
    for (int depth = 0; c && depth < 8; ++depth) {
        jobjectArray fs = (jobjectArray)env->CallObjectMethod(c, mGetDF);
        if (clr(env)) return nullptr;
        jsize n = fs ? env->GetArrayLength(fs) : 0;
        for (jsize i = 0; i < n; ++i) {
            jobject f = env->GetObjectArrayElement(fs, i);
            jobject t = env->CallObjectMethod(f, mType);
            jstring nm = t ? (jstring)env->CallObjectMethod(t, mCName) : nullptr;
            bool match = false;
            if (nm) {
                const char* u = env->GetStringUTFChars(nm, nullptr);
                match = u && strcmp(u, typeName) == 0;
                if (u) env->ReleaseStringUTFChars(nm, u);
                env->DeleteLocalRef(nm);
            }
            if (t) env->DeleteLocalRef(t);
            if (match) {
                env->CallVoidMethod(f, mAcc, JNI_TRUE);
                jobject v = env->CallObjectMethod(f, mGet, obj);
                if (!clr(env) && v) {
                    env->DeleteLocalRef(f);
                    return v;
                }
            }
            clr(env);
            env->DeleteLocalRef(f);
        }
        if (fs) env->DeleteLocalRef(fs);
        jobject parent = env->CallObjectMethod(c, mSuper);
        if (clr(env)) break;
        c = (jclass)parent;
    }
    return nullptr;
}

static jobject staticFieldOfType(JNIEnv* env, jclass cls, const char* typeName) {
    jclass cClass = env->FindClass("java/lang/Class");
    jclass cField = env->FindClass("java/lang/reflect/Field");
    jmethodID mGetDF = env->GetMethodID(cClass, "getDeclaredFields",
        "()[Ljava/lang/reflect/Field;");
    jmethodID mSuper = env->GetMethodID(cClass, "getSuperclass", "()Ljava/lang/Class;");
    jmethodID mCName = env->GetMethodID(cClass, "getName", "()Ljava/lang/String;");
    jmethodID mType = env->GetMethodID(cField, "getType", "()Ljava/lang/Class;");
    jmethodID mAcc = env->GetMethodID(cField, "setAccessible", "(Z)V");
    jmethodID mGet = env->GetMethodID(cField, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    jclass c = cls;
    for (int depth = 0; c && depth < 8; ++depth) {
        jobjectArray fs = (jobjectArray)env->CallObjectMethod(c, mGetDF);
        if (clr(env)) return nullptr;
        jsize n = fs ? env->GetArrayLength(fs) : 0;
        for (jsize i = 0; i < n; ++i) {
            jobject f = env->GetObjectArrayElement(fs, i);
            jobject t = env->CallObjectMethod(f, mType);
            jstring nm = t ? (jstring)env->CallObjectMethod(t, mCName) : nullptr;
            bool match = false;
            if (nm) {
                const char* u = env->GetStringUTFChars(nm, nullptr);
                match = u && strcmp(u, typeName) == 0;
                if (u) env->ReleaseStringUTFChars(nm, u);
                env->DeleteLocalRef(nm);
            }
            if (t) env->DeleteLocalRef(t);
            if (match) {
                env->CallVoidMethod(f, mAcc, JNI_TRUE);
                jobject v = env->CallObjectMethod(f, mGet, (jobject)nullptr);
                if (!clr(env) && v) {
                    env->DeleteLocalRef(f);
                    return v;
                }
            }
            clr(env);
            env->DeleteLocalRef(f);
        }
        if (fs) env->DeleteLocalRef(fs);
        jobject parent = env->CallObjectMethod(c, mSuper);
        if (clr(env)) break;
        c = (jclass)parent;
    }
    return nullptr;
}

// --------------------------------------------------------- view discovery

static jobject findIconInTree(JNIEnv* env, jobject view, int depth) {
    if (!view || depth < 0) return nullptr;
    if (classNameOf(env, view) == ICON_TYPE) return view;

    jclass cVG = env->FindClass("android/view/ViewGroup");
    if (!cVG || !env->IsInstanceOf(view, cVG)) return nullptr;
    jmethodID mCount = env->GetMethodID(cVG, "getChildCount", "()I");
    jmethodID mChild = env->GetMethodID(cVG, "getChildAt", "(I)Landroid/view/View;");
    if (!mCount || !mChild) return nullptr;

    jint n = env->CallIntMethod(view, mCount);
    if (clr(env)) return nullptr;
    for (jint i = 0; i < n; ++i) {
        jobject ch = env->CallObjectMethod(view, mChild, i);
        if (clr(env) || !ch) continue;
        jobject r = findIconInTree(env, ch, depth - 1);
        env->DeleteLocalRef(ch);
        if (r) return r;
    }
    return nullptr;
}

static jobject findIconFromRoots(JNIEnv* env) {
    jclass cWMG = loadClass(env, "android.view.WindowManagerGlobal");
    if (!cWMG) return nullptr;

    jobject inst = nullptr;
    jmethodID mInst = env->GetStaticMethodID(cWMG, "getInstance",
        "()Landroid/view/WindowManagerGlobal;");
    if (!clr(env) && mInst) {
        inst = env->CallStaticObjectMethod(cWMG, mInst);
        if (clr(env)) inst = nullptr;
    }

    jobject list = nullptr;
    if (inst) {
        jmethodID mRoots = env->GetMethodID(cWMG, "getRootViews", "()Ljava/util/ArrayList;");
        if (!clr(env) && mRoots) {
            list = env->CallObjectMethod(inst, mRoots);
            if (clr(env)) list = nullptr;
        }
    }
    if (!list) {
        jfieldID fViews = env->GetStaticFieldID(cWMG, "mViews", "Ljava/util/ArrayList;");
        if (!clr(env) && fViews) {
            list = env->GetStaticObjectField(cWMG, fViews);
            if (clr(env)) list = nullptr;
        }
    }
    if (!list) return nullptr;

    jclass cList = env->FindClass("java/util/ArrayList");
    jmethodID mSize = env->GetMethodID(cList, "size", "()I");
    jmethodID mGet = env->GetMethodID(cList, "get", "(I)Ljava/lang/Object;");
    jint n = env->CallIntMethod(list, mSize);
    if (clr(env)) return nullptr;

    for (jint i = 0; i < n; ++i) {
        jobject root = env->CallObjectMethod(list, mGet, i);
        if (clr(env) || !root) continue;
        env->PushLocalFrame(1024);
        jobject r = findIconInTree(env, root, 24);
        if (r) {
            r = env->PopLocalFrame(r);
            LOGI("icon found in root view #%d", i);
            return r;
        }
        env->PopLocalFrame(nullptr);
        env->DeleteLocalRef(root);
    }
    return nullptr;
}

static jobject findCtrl(JNIEnv* env) {
    jclass cDep = loadClass(env, "com.android.systemui.DependencyEx");
    if (!cDep) return nullptr;
    jfieldID fS = env->GetStaticFieldID(cDep, "sDependency",
        "Lcom/android/systemui/DependencyEx;");
    if (clr(env) || !fS) return nullptr;
    jobject dep = env->GetStaticObjectField(cDep, fS);
    if (!dep) return nullptr;
    jmethodID mGetDep = env->GetMethodID(cDep, "getDependency",
        "(Ljava/lang/Class;)Ljava/lang/Object;");
    jclass cOk = loadClass(env, "com.android.keyguard.OplusKeyguardDependencyEx");
    if (!mGetDep || !cOk) return nullptr;
    jobject ok = env->CallObjectMethod(dep, mGetDep, cOk);
    if (clr(env) || !ok) return nullptr;
    return callNoArg(env, ok, "getOplusBiometricAuthController");
}

static jobject findIconFallback(JNIEnv* env) {
    jclass cKFU = loadClass(env, KFU_TYPE);
    if (cKFU) {
        jobject r = staticFieldOfType(env, cKFU, ICON_TYPE);
        if (r) return r;
    }
    jclass cMech = loadClass(env, MECH_TYPE);
    if (cMech) {
        jobject r = staticFieldOfType(env, cMech, ICON_TYPE);
        if (r) return r;
    }
    jobject ctrl = findCtrl(env);
    if (ctrl) {
        jobject r = fieldOfType(env, ctrl, ICON_TYPE);
        if (r) return r;
        jobject mech = fieldOfType(env, ctrl, MECH_TYPE);
        if (mech) {
            r = fieldOfType(env, mech, ICON_TYPE);
            if (r) return r;
        }
    }
    return nullptr;
}

// ------------------------------------------------------ system services

static bool resolveApp(JNIEnv* env) {
    jclass cAT = env->FindClass("android/app/ActivityThread");
    if (!cAT) { clr(env); return false; }
    jmethodID mCur = env->GetStaticMethodID(cAT, "currentApplication",
        "()Landroid/app/Application;");
    if (!mCur) { clr(env); return false; }
    jobject app = env->CallStaticObjectMethod(cAT, mCur);
    if (clr(env) || !app) return false;

    jmethodID mGetCL = env->GetMethodID(env->GetObjectClass(app),
        "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (!mGetCL) { clr(env); return false; }
    jobject cl = env->CallObjectMethod(app, mGetCL);
    if (clr(env) || !cl) return false;

    gApp = env->NewGlobalRef(app);
    gCL = env->NewGlobalRef(cl);
    gLoadClass = env->GetMethodID(env->GetObjectClass(cl), "loadClass",
        "(Ljava/lang/String;)Ljava/lang/Class;");
    if (!gLoadClass) { clr(env); return false; }

    mGetContentResolver = env->GetMethodID(env->GetObjectClass(app),
        "getContentResolver", "()Landroid/content/ContentResolver;");
    clr(env);
    LOGI("got SystemUI application + classloader");
    return true;
}

static bool loadHelper(JNIEnv* env) {
    jclass cIMDCL = env->FindClass("dalvik/system/InMemoryDexClassLoader");
    if (!cIMDCL) { clr(env); LOGE("no InMemoryDexClassLoader"); return false; }

    jmethodID ctor = env->GetMethodID(cIMDCL, "<init>",
        "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
    bool single = ctor != nullptr;
    if (clr(env)) ctor = nullptr;
    jmethodID ctorArr = nullptr;
    if (!ctor) {
        ctorArr = env->GetMethodID(cIMDCL, "<init>",
            "([Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (clr(env)) ctorArr = nullptr;
    }
    if (!ctor && !ctorArr) { LOGE("no dex classloader ctor"); return false; }

    jobject loader = nullptr;
    if (single) {
        jobject buf = env->NewDirectByteBuffer((void*)kHelperDex, (jlong)kHelperDexLen);
        loader = env->NewObject(cIMDCL, ctor, buf, gCL);
        env->DeleteLocalRef(buf);
    } else {
        jclass cBuf = env->FindClass("java/nio/ByteBuffer");
        jobjectArray arr = env->NewObjectArray(1, cBuf, nullptr);
        jobject buf = env->NewDirectByteBuffer((void*)kHelperDex, (jlong)kHelperDexLen);
        env->SetObjectArrayElement(arr, 0, buf);
        env->DeleteLocalRef(buf);
        loader = env->NewObject(cIMDCL, ctorArr, arr, gCL);
        env->DeleteLocalRef(arr);
    }
    if (clr(env) || !loader) { LOGE("helper dex classloader failed"); return false; }

    jstring name = env->NewStringUTF("mahiro.fod.NativeUi");
    jobject cls = env->CallObjectMethod(loader, gLoadClass, name);
    env->DeleteLocalRef(name);
    if (clr(env) || !cls) { LOGE("NativeUi class not found"); return false; }

    gHelperCls = (jclass)env->NewGlobalRef(cls);
    mHelperDown = env->GetStaticMethodID(gHelperCls, "onDown",
        "(Landroid/view/View;Ljava/lang/Object;Z)V");
    mHelperUp = env->GetStaticMethodID(gHelperCls, "onUp",
        "(Landroid/view/View;Ljava/lang/Object;Z)V");
    mHelperDestroy = env->GetStaticMethodID(gHelperCls, "destroyHbm", "()V");
    jmethodID mInit = env->GetStaticMethodID(gHelperCls, "init", "()V");
    if (clr(env) || !mHelperDown || !mHelperUp || !mHelperDestroy || !mInit) {
        LOGE("NativeUi methods not found");
        return false;
    }
    env->CallStaticVoidMethod(gHelperCls, mInit);
    clr(env);
    LOGI("embedded NativeUi loaded");
    return true;
}

static bool forceDimLayer(JNIEnv* env) {
    jclass cOpt = loadClass(env, OPT_TYPE);
    if (!cOpt) return false;

    jfieldID fDel = env->GetStaticFieldID(cOpt, "isDisableAppDimLayer$delegate",
        "Lkotlin/Lazy;");
    if (clr(env) || !fDel) return false;
    jobject del = env->GetStaticObjectField(cOpt, fDel);
    if (clr(env) || !del) return false;

    jclass cLazy = env->GetObjectClass(del);
    jfieldID fVal = env->GetFieldID(cLazy, "_value", "Ljava/lang/Object;");
    jfieldID fInit = env->GetFieldID(cLazy, "initializer",
        "Lkotlin/jvm/functions/Function0;");
    clr(env);

    jclass cBool = env->FindClass("java/lang/Boolean");
    jfieldID fTrue = env->GetStaticFieldID(cBool, "TRUE", "Ljava/lang/Boolean;");
    jobject bTrue = fTrue ? env->GetStaticObjectField(cBool, fTrue) : nullptr;
    if (clr(env) || !fVal || !bTrue) return false;

    env->SetObjectField(del, fVal, bTrue);
    if (fInit) env->SetObjectField(del, fInit, nullptr);
    clr(env);
    LOGI("KeyguardFeatureOption.isDisableAppDimLayer forced to TRUE");
    return true;
}

// ------------------------------------------------------ refresh limiter

static bool limit120Enabled() {
    char v[PROP_VALUE_MAX] = {0};
    __system_property_get("persist.mahiro.fod.limit120", v);
    return v[0] == '1';
}

static jobject contentResolver(JNIEnv* env) {
    if (!gApp || !mGetContentResolver) return nullptr;
    jobject cr = env->CallObjectMethod(gApp, mGetContentResolver);
    if (clr(env)) return nullptr;
    return cr;
}

static void rateSaveSet(JNIEnv* env) {
    if (!limit120Enabled() || gRateSaved) return;
    if (!gSysCls || !mSysGetFloat || !mSysPutFloat) return;
    jobject cr = contentResolver(env);
    if (!cr) return;
    jstring kPeak = env->NewStringUTF("peak_refresh_rate");
    jstring kMin = env->NewStringUTF("min_refresh_rate");

    gSavedPeak = env->CallStaticFloatMethod(gSysCls, mSysGetFloat, cr, kPeak, 144.0f);
    if (clr(env)) gSavedPeak = 144.0f;
    gSavedMin = env->CallStaticFloatMethod(gSysCls, mSysGetFloat, cr, kMin, 0.0f);
    if (clr(env)) gSavedMin = 0.0f;

    env->CallStaticBooleanMethod(gSysCls, mSysPutFloat, cr, kPeak, 120.0f);
    clr(env);
    env->CallStaticBooleanMethod(gSysCls, mSysPutFloat, cr, kMin, 120.0f);
    clr(env);

    env->DeleteLocalRef(kPeak);
    env->DeleteLocalRef(kMin);
    env->DeleteLocalRef(cr);
    gRateSaved = true;
    LOGI("FOD refresh limited to 120Hz (saved peak=%.1f min=%.1f)", gSavedPeak, gSavedMin);
}

static void rateRestore(JNIEnv* env) {
    if (!gRateSaved) return;
    jobject cr = contentResolver(env);
    if (!cr) return;
    jstring kPeak = env->NewStringUTF("peak_refresh_rate");
    jstring kMin = env->NewStringUTF("min_refresh_rate");
    env->CallStaticBooleanMethod(gSysCls, mSysPutFloat, cr, kPeak, gSavedPeak);
    clr(env);
    env->CallStaticBooleanMethod(gSysCls, mSysPutFloat, cr, kMin, gSavedMin);
    clr(env);
    env->DeleteLocalRef(kPeak);
    env->DeleteLocalRef(kMin);
    env->DeleteLocalRef(cr);
    gRateSaved = false;
    LOGI("FOD refresh restored (peak=%.1f min=%.1f)", gSavedPeak, gSavedMin);
}

// ------------------------------------------------------------ resolution

static void dropIcon(JNIEnv* env) {
    if (gIcon) env->DeleteGlobalRef(gIcon);
    if (gMech) env->DeleteGlobalRef(gMech);
    if (gIconCls) env->DeleteGlobalRef(gIconCls);
    gIcon = nullptr;
    gMech = nullptr;
    gIconCls = nullptr;
    mScreenOff = nullptr;
    mGetVisibility = nullptr;
    mIsAttached = nullptr;
}

static void resolveMech(JNIEnv* env, jobject icon) {
    if (gMech || !icon) return;

    jfieldID f = env->GetFieldID(gIconCls, "onScreenFingerprintUiMech",
        "Lcom/oplus/systemui/biometrics/finger/udfps/OnScreenFingerprintUiMech;");
    if (clr(env)) f = nullptr;
    jobject mech = f ? env->GetObjectField(icon, f) : nullptr;
    if (clr(env)) mech = nullptr;

    if (!mech) {
        jobject ctrl = findCtrl(env);
        if (ctrl) mech = fieldOfType(env, ctrl, MECH_TYPE);
    }
    if (!mech) return;

    gMech = env->NewGlobalRef(mech);
    jclass cM = env->GetObjectClass(mech);
    mScreenOff = env->GetMethodID(cM, "getScreenTurnedOff", "()Z");
    if (clr(env)) mScreenOff = nullptr;
    LOGI("OnScreenFingerprintUiMech resolved (getScreenTurnedOff=%s)",
         mScreenOff ? "yes" : "no");
}

static bool iconStillAttached(JNIEnv* env) {
    if (!gIcon || !mIsAttached) return true;
    jboolean attached = env->CallBooleanMethod(gIcon, mIsAttached);
    if (clr(env)) return true;
    return attached == JNI_TRUE;
}

static bool ensureIcon(JNIEnv* env) {
    if (gIcon) {
        if (iconStillAttached(env)) {
            if (!gMech) resolveMech(env, gIcon);
            return true;
        }
        LOGI("cached icon detached, rediscovering");
        dropIcon(env);
    }

    env->PushLocalFrame(256);
    jobject icon = findIconFromRoots(env);
    if (!icon) icon = findIconFallback(env);

    bool ok = false;
    if (icon) {
        jclass cView = env->FindClass("android/view/View");
        if (cView) {
            mGetVisibility = env->GetMethodID(cView, "getVisibility", "()I");
            if (clr(env)) mGetVisibility = nullptr;
            mIsAttached = env->GetMethodID(cView, "isAttachedToWindow", "()Z");
            if (clr(env)) mIsAttached = nullptr;
        }
        jclass cIcon = env->GetObjectClass(icon);
        gIconCls = (jclass)env->NewGlobalRef(cIcon);
        gIcon = env->NewGlobalRef(icon);
        resolveMech(env, icon);
        ok = (gIcon != nullptr && mGetVisibility != nullptr);
        LOGI("OnScreenFingerprintIcon resolved (%s)", ok ? "ok" : "no methods");
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

// --------------------------------------------------------------- actions

static bool screenTurnedOff(JNIEnv* env) {
    if (!gMech || !mScreenOff) return false;
    jboolean b = env->CallBooleanMethod(gMech, mScreenOff);
    if (clr(env)) return false;
    return b == JNI_TRUE;
}

static void handleDown(JNIEnv* env) {
    if (!ensureIcon(env)) {
        LOGE("FINGER_DOWN but icon not found yet");
        return;
    }
    jint vis = env->CallIntMethod(gIcon, mGetVisibility);
    if (clr(env)) return;
    bool visible = (vis == 0);
    bool screenOff = screenTurnedOff(env);

    // smali receiver: hidden icon is only handled when the screen is off
    if (!visible && !screenOff) {
        LOGI("FINGER_DOWN ignored (icon hidden, screen on)");
        return;
    }

    bool show = (!visible && screenOff);
    gSawVisible = visible;
    rateSaveSet(env);
    env->CallStaticVoidMethod(gHelperCls, mHelperDown, gIcon, gMech, (jboolean)show);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    gHbmOn = true;
    LOGI("finger DOWN (visible=%d screenOff=%d show=%d)", visible, screenOff, show);
}

static void handleUp(JNIEnv* env) {
    if (!ensureIcon(env)) {
        LOGE("FINGER_UP but icon not found yet");
        return;
    }
    bool wasScreenOff = screenTurnedOff(env);
    env->CallStaticVoidMethod(gHelperCls, mHelperUp, gIcon, gMech, (jboolean)wasScreenOff);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    gHbmOn = false;
    gSawVisible = false;
    rateRestore(env);
    LOGI("finger UP (wasScreenOff=%d)", wasScreenOff);
}

static void cleanupAfterSocketClose(JNIEnv* env) {
    if (!gHbmOn) return;
    LOGI("bridge disconnected while finger down, cleaning up");
    env->CallStaticVoidMethod(gHelperCls, mHelperUp, gIcon, gMech, (jboolean)JNI_FALSE);
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    gHbmOn = false;
    gSawVisible = false;
    rateRestore(env);
}

static void watchdog(JNIEnv* env, int tick) {
    if (!gDimForced && (tick % 50) == 0) {
        env->PushLocalFrame(32);
        gDimForced = forceDimLayer(env);
        env->PopLocalFrame(nullptr);
    }

    if (!gIcon) {
        if ((tick % 20) == 0) ensureIcon(env);
        return;
    }

    if (!iconStillAttached(env)) {
        LOGI("icon detached, cleaning up");
        if (gHelperCls) {
            env->CallStaticVoidMethod(gHelperCls, mHelperDestroy);
            if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
        }
        gHbmOn = false;
        gSawVisible = false;
        rateRestore(env);
        dropIcon(env);
        return;
    }
    if (!gMech) {
        env->PushLocalFrame(64);
        resolveMech(env, gIcon);
        env->PopLocalFrame(nullptr);
    }

    if (gHbmOn && gIcon) {
        jint vis = env->CallIntMethod(gIcon, mGetVisibility);
        if (!clr(env)) {
            if (vis == 0) {
                gSawVisible = true;
            } else if (gSawVisible) {
                // smali setVisibility() hook: hide => destroy HBM
                LOGI("icon hidden while HBM active, destroying HBM");
                env->CallStaticVoidMethod(gHelperCls, mHelperDestroy);
                if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
                gHbmOn = false;
                gSawVisible = false;
                rateRestore(env);
            }
        }
    }
}

// ------------------------------------------------------------------ main

static JavaVM* getVm() {
    const char* libs[] = {"libnativehelper.so", "libart.so", "libandroid_runtime.so"};
    for (const char* n : libs) {
        void* h = dlopen(n, RTLD_NOW | RTLD_GLOBAL);
        if (!h) continue;
        typedef jint (*GetVMs_t)(JavaVM**, jsize, jsize*);
        GetVMs_t f = (GetVMs_t)dlsym(h, "JNI_GetCreatedJavaVMs");
        if (!f) continue;
        JavaVM* vm = nullptr;
        jsize cnt = 0;
        if (f(&vm, 1, &cnt) == JNI_OK && cnt > 0 && vm) {
            LOGI("JavaVM via %s", n);
            return vm;
        }
    }
    return nullptr;
}

static void* worker(void*) {
    usleep(500 * 1000);
    gVm = getVm();
    if (!gVm) { LOGE("no JavaVM"); return nullptr; }

    JNIEnv* env = nullptr;
    if (gVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
        LOGE("attach failed");
        return nullptr;
    }

    for (int i = 0; i < 600 && !gApp; ++i) {
        env->PushLocalFrame(64);
        bool ok = resolveApp(env);
        env->PopLocalFrame(nullptr);
        if (ok) break;
        clr(env);
        usleep(500 * 1000);
    }
    if (!gApp) { LOGE("no SystemUI application"); return nullptr; }

    for (int i = 0; i < 600; ++i) {
        env->PushLocalFrame(64);
        bool ok = loadHelper(env);
        env->PopLocalFrame(nullptr);
        if (ok) break;
        clr(env);
        usleep(500 * 1000);
    }
    if (!gHelperCls) { LOGE("could not load NativeUi"); return nullptr; }

    gSysCls = (jclass)env->NewGlobalRef(env->FindClass("android/provider/Settings$System"));
    if (gSysCls) {
        mSysGetFloat = env->GetStaticMethodID(gSysCls, "getFloat",
            "(Landroid/content/ContentResolver;Ljava/lang/String;F)F");
        mSysPutFloat = env->GetStaticMethodID(gSysCls, "putFloat",
            "(Landroid/content/ContentResolver;Ljava/lang/String;F)Z");
        clr(env);
    }

    env->PushLocalFrame(32);
    gDimForced = forceDimLayer(env);
    env->PopLocalFrame(nullptr);

    for (int i = 0; i < 60 && !gIcon; ++i) {
        if (ensureIcon(env)) break;
        usleep(500 * 1000);
    }

    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) { LOGE("socket failed: %s", strerror(errno)); return nullptr; }
    sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    const char* nm = "fodhook";
    a.sun_path[0] = '\0';
    memcpy(a.sun_path + 1, nm, strlen(nm));
    socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + strlen(nm);

    if (bind(s, (sockaddr*)&a, len) != 0 || listen(s, 4) != 0) {
        LOGE("socket bind/listen failed: %s", strerror(errno));
        close(s);
        return nullptr;
    }
    LOGI("listening on @fodhook");

    int tick = 0;
    for (;;) {
        struct pollfd pfd = { s, POLLIN, 0 };
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            int c = accept(s, nullptr, nullptr);
            if (c >= 0) {
                LOGI("bridge connected");
                char b;
                while (read(c, &b, 1) == 1) {
                    if (b == '1') handleDown(env);
                    else if (b == '0') handleUp(env);
                }
                close(c);
                cleanupAfterSocketClose(env);
            }
        }
        watchdog(env, tick++);
    }
    return nullptr;
}

__attribute__((constructor))
static void on_load() {
    LOGI("Mahiro native FOD hook loaded");
    pthread_t t;
    pthread_create(&t, nullptr, worker, nullptr);
    pthread_detach(t);
}
