# battery_mon

A tiny battery monitor in C for Android, built to run from any terminal app. It can read battery state in seven different ways, and it prints a single line whenever something changes. The runtime has no daemon and no JVM linked in. If the transaction-code cache is missing or stale, root and shell can run the optional `code_fetcher` helper once and then read the refreshed cache.

```
battery=85% status=CHARGING health=GOOD charger=AC present=1 voltage=4123mV temp=31.5C
```

This project is a collection of ways to read battery stats on Android efficiently. Each source has its own trade-offs in permissions, overhead and the fields it exposes, so battery_mon puts all seven behind one binary that works whether you're root, in an adb shell, or in an ordinary terminal app.

Because it emits a clean line only when the state changes, it also works well as a data source for statistical battery learning: pipe the output into a logger and build charge/discharge curves, health trends or usage models from it.

## Building

You need a C11 compiler and the Linux UAPI binder header (`linux/android/binder.h`). Most Android terminal toolchains (Termux, for example) ship both, on rooted and stock devices alike.

```sh
make               # builds ./battery_mon
make test          # runs the parcel and transaction-cache tests
make code_fetcher  # builds the optional JNI helper
make clean
```

The default flags are `-O2 -Wall -Wextra -Werror -std=gnu11 -fstack-protector-strong -D_FORTIFY_SOURCE=2`, linked with `-Wl,-z,relro,-z,now`. If your toolchain needs something different, pass `CFLAGS`, `LDFLAGS` or `CC` on the make command line.

## Usage

```sh
./battery_mon [--method NAME]
```

If you leave out `--method` it uses `auto`.

| Method | What it uses | Needs root |
|---|---|---|
| `root_binder` | AIDL `android.hardware.health.IHealth/default` on `/dev/binder`, with a HIDL `@2.1`/`@2.0` fallback on `/dev/hwbinder` | yes |
| `netlink_uevent` | `NETLINK_KOBJECT_UEVENT`; re-reads sysfs when a `power_supply` event arrives | yes |
| `shell_sysfs_polling` | polls `/sys/class/power_supply` once a second | root or shell UID |
| `battery_properties_registrar` | AIDL `android.os.IBatteryPropertiesRegistrar` on `/dev/binder` (capacity, status, current, charge counter) | no |
| `battery_dump` | raw binder `dump()` on the `battery` service, then parses the `BatteryService` text | adb shell or root |
| `app_broadcastreceiver_method` | reads a line stream from the companion BattMon app over the abstract socket, the exported ContentProvider, or a shared file | no |
| `input_battery_listener` | input-service battery callbacks, for styli and other peripherals | input service; some devices need root or `BLUETOOTH` |
| `auto` | tries methods in a fixed order until one starts | depends |

### How `auto` picks

As root, it tries `root_binder`, then `netlink_uevent`, `shell_sysfs_polling`, `battery_properties_registrar`, and finally `app_broadcastreceiver_method`.

As a regular app, it tries `battery_properties_registrar`, then `shell_sysfs_polling`, then `app_broadcastreceiver_method`.

Two methods are never picked automatically. `input_battery_listener` reports peripheral batteries rather than the phone's own, so it would be a confusing default. `battery_dump` only works under adb shell or root, since an app UID can't resolve the `battery` service at all.

### Examples

```sh
su -c './battery_mon'                                        # auto, as root
./battery_mon --method shell_sysfs_polling                   # sysfs; needs root/shell on stock Android
./battery_mon --method battery_properties_registrar          # binder, no root
adb shell /data/local/tmp/battery_mon --method battery_dump  # shell, full dump
./battery_mon -m app_broadcastreceiver_method                # relay from a local app
```

Ctrl-C (`SIGINT`) or `SIGTERM` stops it cleanly.

Exit codes: `0` for a clean stop, `1` if no method was available or something failed at runtime, `2` for a usage error.

## Output

The main battery line prints once at startup and again whenever a field changes:

```
battery=85% status=CHARGING health=GOOD charger=AC present=1 voltage=4123mV temp=31.5C
```

A few notes on the fields:

- `charger` is picked in this priority order: AC, USB, WIRELESS, DOCK, NONE.
- `present`, `voltage` and `temp` show `unknown` if the source can't provide them.
- `status` and `health` use the Android `BatteryManager` constant names.

Some methods print something a little different.

`input_battery_listener` prints one line per device:

```
input-device=6 battery=42% status=DISCHARGING
input-device=2 no battery
```

`app_broadcastreceiver_method` echoes each line it receives, with control bytes (other than tab) replaced by `?`.

`battery_properties_registrar` prints only what that interface exposes:

```
battery=18% status=CHARGING charge_counter=1100000uAh current=1332300uA current_avg=1731800uA
```

Properties the device doesn't support are left out. The energy counter, for example, is often unsupported.

`battery_dump` prints the standard line, parsed from the service's text dump:

```
battery=22% status=CHARGING health=GOOD charger=AC present=1 voltage=3931mV temp=41.5C
```

## How each method works

### `root_binder`

It opens `/dev/binder` and looks up `android.hardware.health.IHealth/default` through `android.os.IServiceManager.checkService`, falling back to `listServices` if that fails. Then it calls `update()`, registers a local `IHealthInfoCallback`, and decodes the `HealthInfo` that arrives in the callback parcel.

If the AIDL service isn't there, it retries with the HIDL HAL (`@2.1`, then `@2.0`) on `/dev/hwbinder`, including the `IBase::interfaceChain` and `interfaceDescriptor` replies. If something fails mysteriously, check `dmesg` for SELinux `avc` denials.

### `netlink_uevent`

Binds the kernel uevent socket (group 1) and ignores everything except `SUBSYSTEM=power_supply`. When one of those events shows up, it re-reads sysfs and prints only if the state is different from last time. This is the cheapest of the root methods because nothing is polled.

### `shell_sysfs_polling`

Reads `capacity`, `status`, `health`, `present`, `voltage_now`, `temp`, and the `ac`/`wired`, `usb`, `wireless` and `dock` online flags from `/sys/class/power_supply`. It samples once per second and prints when something changed.

It assumes `voltage_now` is in microvolts, but values at or below 10000 are treated as millivolts, because some vendors report it that way. `temp` is assumed to be tenths of a degree Celsius.

### `app_broadcastreceiver_method`

Connects to the abstract Unix socket `com.battmon.battery` exposed by the companion BattMon app. If the socket is denied, it tries the exported provider `content://com.battmon.battery/stream`, then the shared file `/sdcard/Android/media/com.battmon/battery.log`. The file reader prints the last line immediately, then follows `inotify` events with a one-second polling fallback. It exits on EOF or when signalled. It never opens a TCP port and never uses the network.

Android reserves `ACCESS_CONTENT_PROVIDERS_EXTERNALLY` for privileged UIDs, so the `content` CLI provider path only works as root or shell. A regular terminal app normally reaches the shared file instead, while the provider stays available to system and privileged callers. The app truncates the shared file at 256 KiB.

This one is a convenience for apps that already receive `ACTION_BATTERY_CHANGED` and want to hand it off. The provider output is filtered to lines beginning `battery=`; the socket and file streams are passed through.

### `input_battery_listener`

Resolves the `input` service, calls `getInputDeviceIds()`, and registers an `IInputDeviceBatteryListener` callback for each device. It prints `BatteryState` changes (`deviceId`, `isPresent`, `status`, `capacityPercent`). Devices with no battery report `no battery`, and a present battery with an unknown capacity reports `battery=unknown`.

### `battery_properties_registrar`

Resolves the `batteryproperties` service through `android.os.IServiceManager.checkService`, then calls `IBatteryPropertiesRegistrar.getProperty(int)` once a second. It reads only the unprivileged ids:

| Id | Property |
|---|---|
| 1 | charge counter |
| 2 | current now |
| 3 | current average |
| 4 | capacity |
| 5 | energy counter |
| 6 | status |

Apps and the shell can both reach this service without root. Ids 7 and up (manufacturing date, first-usage date, charging policy, state of health, serial number, part status) need `BATTERY_STATS`, so they aren't used. The `BatteryProperty` string field is ignored.

### `battery_dump`

Resolves the `battery` service through `android.os.IServiceManager.checkService` and sends a raw `IBinder.dump()` transaction (`0x5f444d50`) once a second. The parcel holds a write-end pipe fd and an empty string array. The service writes its usual `dumpsys battery` text into the pipe, and the method parses `level`, `status`, `health`, `present`, `voltage`, `temperature` and the AC/USB/Wireless/Dock flags out of it.

You get the same data as `dumpsys battery` without spawning a process. The catch is that it's shell/root only: the app UID can't resolve the `battery` service, and the shell domain can't call the health HAL directly.

## Wire format

`src/bm_binder.c` has the parcel and binder code shared by `root_binder`, `input_battery_listener`, `battery_properties_registrar` and `battery_dump`.

- **Stable AIDL parcels** start with strict-mode, work-source and `SYST` headers, followed by a String16 interface token. Binder objects carry a stability value.
- **HIDL** has no interface token. `hidl_string` arguments are buffer objects that point at a pointer-sized header (16 bytes on 64-bit, 12 on 32-bit) plus the string itself.
- **Raw dump** has no interface token and no stability value. The parcel is a `BINDER_TYPE_FD` object for the pipe's write end, followed by a zero-length string array.
- **Limits:** parcel writers are fixed at 2 KiB with 32 object offsets, and every read is bounds-checked before any multiplication or pointer arithmetic.

The transaction codes for the service manager, health HAL, input service, battery properties and battery dump are the platform AIDL/HIDL/IBinder values for the Android version the tool was built against. If a HAL layout changes in a future release, the codes will need updating to match.

### Transaction-code cache

`src/bm_txn_cache.c` reads and writes the runtime cache. The file is a fixed-size native struct (4352 bytes on arm64) holding:

- magic `0x424d5443`, version 1, size, CRC32, generator uid and generation time
- `ro.build.fingerprint` and the zygote boot classpath
- the `framework.jar` stat (`dev`, `ino`, `size`, `mtime`, mode, uid, gid)
- the ten fetched transaction codes

Writes use `mkstemp`, `fsync` and `rename`, with mode 0600. Reads use `O_NOFOLLOW` and validate the size, CRC, fingerprint and framework stat before applying anything.

Where the cache lives depends on the effective uid:

| uid | Path |
|---|---|
| 0 | `/data/data/android/transact_codes.bin` |
| 2000 (`shell`) | `/data/user_de/0/com.android.shell/transact_codes.bin` |
| other | `/sdcard/.1t4c.bin` |

On a miss, a stale cache or a corrupt cache, uid 0 and 2000 exec `code_fetcher --cache PATH --quiet`. The helper is `BM_CODE_FETCHER` if that's set to an absolute executable path, otherwise the sibling `code_fetcher` next to `battery_mon`. It is never found through `PATH`. A stale cache passes its stored boot classpath to the helper, which is what lets the shell refresh after an OTA. If the helper fails, the runtime keeps the compiled defaults, and stale, corrupt and I/O failures print a warning.

Other UIDs only read the cache. A valid `/sdcard/.1t4c.bin` is applied, but a stale one falls back to defaults.

**A note on trust:** the cache is integrity-checked, not authenticated. CRC32 catches corruption, and the root and shell copies are mode 0600, but a process running as that uid can still write its own cache. `/sdcard/.1t4c.bin` is shared storage, so treat it as untrusted and never use its stored boot classpath for a root or shell refresh.

#### Generating codes with `code_fetcher`

`make code-fetch` builds the top-level `code_fetcher` and writes `src/bm_txn_codes.h` from the running framework. It creates a JVM through `libnativehelper.so` (resolved from the `com_android_art` namespace) and `JniInvocationInit("libart.so")`, rather than opening `libart.so` directly. It reads the zygote boot classpath, so run it as root or set `BOOTCLASSPATH`:

```sh
su -c './code_fetcher' > src/bm_txn_codes.h
```

The generated `IServiceManager` values are `checkService=3` and `listServices=6`. `root_binder` still uses the legacy 2/4 calls, so compare before switching. The cache stores all ten values, but `bm_txn_cache_apply()` deliberately leaves the two service-manager fields alone.

## Tests

`make test` builds and runs `tests/test_parcel.c` and `tests/test_txn_cache.c`.

The parcel suite covers:

- writer/reader round trips for `i32`, String16 and C strings
- hostile String16 lengths (`INT32_MAX`, truncated payloads) and null strings
- writer overflow on an oversized string
- C strings with no terminator
- truncated fixed-width reads
- binder handle and null-object parsing, with and without an offsets array
- file-descriptor object layout for the raw dump transaction
- HIDL string object layout

The cache suite covers missing-cache reporting, code and bootclasspath round-trips with a non-empty fingerprint, safe-code application without overwriting the legacy service-manager pair, and CRC corruption detection.

There's no device-simulation harness. The binder paths have been checked by hand on a real device: the AIDL health callback, input-service callbacks, battery-properties polling and battery dump polling. The cache paths have been checked as root, app and shell, including stale-fingerprint refresh for root and shell.

A sanitizer build is a handy extra check:

```sh
cc -O1 -g -std=gnu11 -Isrc -fsanitize=address,undefined \
   -o battery_mon_asan src/main/bm_main.c src/bm_common.c src/bm_binder.c \
   src/bm_txn_cache.c src/m1/root_binder.c src/m2/shell_sysfs_polling.c \
   src/m3/app_broadcastreceiver_method.c src/m4/netlink_uevent.c \
   src/m5/input_battery_listener.c src/m6/battery_properties_registrar.c \
   src/m7/battery_dump.c
```

## Project layout

| Path | Purpose |
|---|---|
| `src/bm_common.c` / `.h` | sysfs state, output formatting |
| `src/bm_binder.c` / `.h` | parcel + binder plumbing shared by m1/m5/m6/m7 |
| `src/bm_txn_cache.c` / `.h` | transaction-code cache, fingerprint/stat validation, refresh |
| `src/main/bm_main.c` | CLI, signal handling, auto fallback, cache init |
| `src/m1/` | `root_binder` |
| `src/m2/` | `shell_sysfs_polling` |
| `src/m3/` | `app_broadcastreceiver_method` |
| `src/m4/` | `netlink_uevent` |
| `src/m5/` | `input_battery_listener` |
| `src/m6/` | `battery_properties_registrar` |
| `src/m7/` | `battery_dump` |
| `src/bm_txn_codes.h` | optional generated transaction-code header |
| `tools/code_fetcher.c` | one-shot JNI fetcher; writes a header or `--cache` file |
| `tests/test_parcel.c` | parcel unit tests |
| `tests/test_txn_cache.c` | cache round-trip/corruption tests |

## Known limitations

- The `voltage_now` and `temp` unit guesses work for most vendors, but not all.
- The dock charger only shows up if `/sys/class/power_supply/dock/online` exists.
- The AIDL `HealthInfo` and `BatteryState` layouts follow the stable AIDL definitions for the Android version you're on.
- The HIDL fallback only gets exercised on devices without the AIDL health service.
- `battery_properties_registrar` only exposes the unprivileged property ids, so voltage, temperature, health, charger and present aren't available through it.
- `battery_dump` relies on the `BatteryService` text format, which isn't a stable API, and it only works from adb shell or root.
- The app method's provider path is root/shell-only: Android denies `ACCESS_CONTENT_PROVIDERS_EXTERNALLY` to untrusted app UIDs, so a regular terminal app normally uses the shared file. The exported provider has no read permission and exposes battery data to any app that resolves it.
- The shared file is app-specific external media (`0660`, media_rw), so treat its contents as unauthenticated local data.
- The transaction-code cache is CRC32-checked, not signed. Root and shell caches rely on 0600 permissions, and `/sdcard/.1t4c.bin` is untrusted shared storage.
- A shell first run with no cache and no `BOOTCLASSPATH` can't refresh, so seed the shell cache once as root. Root can read the zygote environment itself.
- The framework reports `IServiceManager` `checkService=3`/`listServices=6`, but the runtime keeps the working 2/4 pair and does not apply those two cached values.
- `code_fetcher` uses `_exit` after fetching because `DestroyJavaVM` crashes on this ART build.
- It runs in the foreground. If you want it to start on its own, use `nohup`, a supervisor, or your terminal app's boot hook (such as `Termux:Boot`).
