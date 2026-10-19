# Mahiro FOD Native - ColorOS 17 / OPLUS (No Smali Patch)

Implementasi native dari [RapliVx/Transsion-BBK16-FOD-Hack](https://github.com/RapliVx/Transsion-BBK16-FOD-Hack)
dan [Mahiro-FOD-BridgeAIDL](https://github.com/usermahiro/Mahiro-FOD-BridgeAIDL):
semua perilaku patch smali `SystemUI.apk` (classes3.dex/classes5.dex) dipindahkan ke
**binary + lib native**, sehingga SystemUI tetap **stock** (tanpa decompile, tanpa re-sign).

---

## Mapping Patch Smali -> Native

| Patch smali COS17 | Pengganti native |
|---|---|
| `FingerKeyReceiver` + `registerReceiver` (`com.rianixia.FINGER_DOWN/UP`) | abstract unix socket `@fodhook` (dikirim `fodbridged`) |
| `handleFingerprintKeyPress()` -> `createHbmSurfaceControl()` | `mahiro.fod.NativeUi.onDown()` di dalam `libfodhook.so` (bikin window alpha-0 bertitle `RianixiaHBMController`) |
| `handleFingerprintKeyRelease()` -> `destroyHbmSurfaceControl()` | `NativeUi.onUp()` (remove window) |
| `updateOpticalUI(setVisibility VISIBLE/GONE)` | `icon.setVisibility()` via main-thread handler helper |
| `OnScreenFingerprintUiMech.onFpTouch(Z)` | dipanggil via reflection dari helper (main thread) |
| Hook `setVisibility()` -> destroy HBM saat icon GONE | watchdog 100ms di `libfodhook.so` (destroy HBM saat visibility != 0) |
| `OnScreenFingerprintUiMech` patch `isDisableAppDimLayer()` = true | force `KeyguardFeatureOption.isDisableAppDimLayer` lazy ke TRUE |
| `FodRefreshRateController` (limit 120Hz) | opsional, `persist.mahiro.fod.limit120=1` (Settings.System peak/min) |
| Sender broadcast `com.rianixia` | `fodbridged` (baca `EV_KEY` 195, kirim ke socket) |

Tidak ada class/method smali yang di-inject ke SystemUI. `libfodhook.so` menemukan
`OnScreenFingerprintIcon` + `OnScreenFingerprintUiMech` dengan refleksi JNI
(WindowManagerGlobal view tree, fallback DependencyEx/KeyguardFingerprintUtils).

---

## Yang Tetap Butuh Binary Patch (bukan smali)

1. **`hwcomposer.*.so`** di `/vendor/lib64/hw/` - HBM layer name:
   ```bash
   perl -pi -e 's/OnScreenFingerprintDimLayer/VRI[RianixiaHBMController]\x00/g' /vendor/lib64/hw/hwcomposer.mt*.so
   ```
2. **`/odm/lib64/libgf_hal.so`** (Goodix) - bypass uevent HBM + `uiReadyStatus`:
   ```bash
   python3 patch_libgf_hal.py /path/to/libgf_hal.so
   ```
   (file patch ada di repo ini; `prebuilt/libgf_hal_patched.so` contoh hasil)

---

## Isi Overlay

| File | Lokasi ROM | Mode | SELinux |
|---|---|---|---|
| `fodbridged` | `/vendor/bin/hw/fodbridged` | 0755 | `system_file` |
| `fodinject` | `/vendor/bin/hw/fodinject` | 0755 | `system_file` |
| `libfodhook.so` | `/system/lib64/libfodhook.so` | 0644 | `system_lib_file` |
| `fodnative.rc` | `/system/etc/init/fodnative.rc` | 0644 | `system_file` |
| `vendor/*` | `/vendor/...` (Transsion HAL) | - | - |

> Library **wajib** di `/system/lib64`: `fodinject` memanggil dlopen dari namespace
> system/default, SystemUI tidak bisa membaca `/vendor/lib64`.

---

## Cara Kerja

```
[driver touchscreen]  EV_KEY 195
        |
        v
   fodbridged  ---- '1'/'0' ---->  @fodhook (abstract socket)
                                        |
                                        v
                          libfodhook.so (di dalam SystemUI)
                                        |
              +-------------------------+--------------------------+
              | FINGER_DOWN                                       | FINGER_UP
              |  - icon hidden + screen off: setVisibility(0)     |  - destroy HBM window
              |  - add window "RianixiaHBMController"             |  - onFpTouch(false)
              |  - onFpTouch(true)                                |  - icon setVisibility(8) jika screen off
              +---------------------------------------------------+
                                        |
        hwcomposer (patched) melihat layer VRI[RianixiaHBMController] -> FULL HBM ON
```

Dim layer bypass + limit refresh rate dijalankan otomatis oleh `libfodhook.so`.

---

## Build

Butuh NDK r27c + Android SDK (untuk `d8` dan `android.jar`):

```bash
export NDK=/path/to/android-ndk-r27c
export ANDROID_SDK_ROOT=/path/to/android-sdk   # optional jika tidak ada di PATH
./build.sh
```

`build.sh` meng-compile `NativeUi.java` -> `classes.dex`, meng-embed dex itu ke
`libfodhook.so` (via `InMemoryDexClassLoader`), lalu meng-compile 3 binary aarch64.
Tidak ada file dex terpisah yang perlu di-push.

CI: `.github/workflows/build.yml` (artifact `fod-native-cos17.zip`).

---

## Deploy

1. Salin `vendor/` repo ini ke `/vendor` perangkat (HAL Transsion: `fp_hal`,
   `android.hardware.biometrics.fingerprint@2.3-service.transsion`, lib interface, VINTF, `fp_hal.rc`).
2. Patch `hwcomposer.*.so` (langkah di atas).
3. Patch `libgf_hal.so` dan ganti di `/odm/lib64/`.
4. Pasang `fodbridged`, `fodinject` ke `/vendor/bin/hw/`, `libfodhook.so` ke `/system/lib64/`,
   `fodnative.rc` ke `/system/etc/init/`.
5. Boot pertama: **SELinux permissive** (`androidboot.selinux=permissive` atau `setenforce 0`).
6. Reboot. Cek: `adb logcat -s fodbridge fodhook fodinject`.

Kalau device bukan `event4` / nama touch bukan `mtk-tpd`, jalankan `fodbridged --dev /dev/input/eventX`
(edit `fodnative.rc`). Tanpa `--dev`, daemon auto-scan device yang mendukung `KEY 195`.

---

## Konfigurasi

| Sysprop | Efek |
|---|---|
| `persist.mahiro.fod.limit120` = `1` | Limit refresh 120Hz saat FOD aktif (untuk panel 144Hz) |

```bash
setprop persist.mahiro.fod.limit120 1
```

---

## Log yang Diharapkan

```
fodinject: injecting into com.android.systemui (pid xxx)
fodhook:   Mahiro native FOD hook loaded
fodhook:   OnScreenFingerprintIcon resolved (mech attached)
fodhook:   OnScreenFingerprintUiMech resolved (getScreenTurnedOff=yes)
fodhook:   KeyguardFeatureOption.isDisableAppDimLayer forced to TRUE
fodhook:   listening on @fodhook
fodbridge: finger DOWN (sent to @fodhook)
fodhook:   FINGER_DOWN visible=0 screenOff=1 show=1
fodhook:   HBM layer added 200x300 @540,1800
fodhook:   finger UP (wasScreenOff=1)
fodhook:   HBM layer removed
```

---

## Catatan & Batasan

- Gunakan **SystemUI stock** COS17 (class `com.oplus.systemui.biometrics.finger.udfps.*`).
- Nama class/field mengikuti guide BBK16 COS17; kalau build ROM berbeda, ubah
  konstanta di `src/libfodhook.cpp`.
- SELinux enforcing perlu policy untuk: `fodinject` ptrace ke `systemui`, `fodbridged`
  baca `/dev/input` + connect socket, dan `libfodhook` baca `/proc`/tulis settings.
- Injection via ptrace rapuh terhadap update linker/SystemUI; `fodinject` akan
  re-inject otomatis kalau SystemUI restart.

## Credits

- [irawansalt & fajarxtr](https://github.com/irawansalt) - Transsion AOSP FOD HALs
- [ryanistr / rianixia](https://github.com/ryanistr) - riset implementasi OPLUS FOD
- [RapliVx](https://github.com/RapliVx) - guide BBK16 COS17
- [Cartethyiaaa / usermahiro](https://github.com/usermahiro) - native bridge & injector
