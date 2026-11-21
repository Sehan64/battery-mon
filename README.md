# battery-mon

Android battery monitoring tools built from Binder, sysfs, uevent, and a
companion app.

## Layout

- `battery_mon/` - C CLI with multiple battery sources and an automatic
  fallback chain. See [`battery_mon/README.md`](battery_mon/README.md) for the
  method details, output format, cache behavior, and limitations.
- `battmon/` - small companion Android app. It exposes the current battery line
  through an abstract Unix socket, an exported read-only `ContentProvider`, and
  an app-specific shared file.

## Build

### `battery_mon`

```sh
cd battery_mon
make
make test
```

### `battmon`

```sh
cd battmon
./build.sh
```

`build.sh` uses `$HOME/android-sdk/platforms/android-36/android.jar` by default.
It creates a local `battmon.keystore` on first use if one is missing. The
keystore, APKs, and generated build directories are not committed.

## App transport

`battery_mon --method app_broadcastreceiver_method` tries these transports in
order:

1. Abstract socket `com.battmon.battery` - root and shell.
2. `content://com.battmon.battery/stream` - root and shell; Android denies
   `ACCESS_CONTENT_PROVIDERS_EXTERNALLY` to untrusted app UIDs.
3. `/sdcard/Android/media/com.battmon/battery.log` - the normal Terminal app UID
   path, read with `inotify` and a one-second polling fallback.

The companion app has no `INTERNET` permission. The provider is exported
without a read permission, and the shared file is app-specific external media,
so treat the relayed data as local and unauthenticated.

## Tested

Infinix X6873, Android 16 (API 36), arm64, any Terminal app, KernelSU root.
