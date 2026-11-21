#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

SDK="${ANDROID_JAR:-$HOME/android-sdk/platforms/android-36/android.jar}"
CLASSES=classes
DEX=dexdir

rm -rf "$CLASSES" "$DEX" battmon-unsigned.apk battmon-aligned.apk
mkdir -p "$CLASSES" "$DEX"

javac -source 8 -target 8 -bootclasspath "$SDK" -d "$CLASSES" $(find src -name '*.java')
d8 --lib "$SDK" --min-api 29 --output "$DEX" $(find "$CLASSES" -name '*.class')
aapt2 link -o battmon-unsigned.apk -I "$SDK" --manifest AndroidManifest.xml \
    --min-sdk-version 29 --target-sdk-version 36
zip -j -0 battmon-unsigned.apk "$DEX/classes.dex" >/dev/null
zipalign -f 4 battmon-unsigned.apk battmon-aligned.apk

if [ ! -f battmon.keystore ]; then
    keytool -genkeypair -keystore battmon.keystore -storepass battmon -keypass battmon \
        -alias battmon -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=BattMon"
fi

apksigner sign --ks battmon.keystore --ks-pass pass:battmon --key-pass pass:battmon \
    --out battmon.apk battmon-aligned.apk
apksigner verify battmon.apk
echo "built $(pwd)/battmon.apk"
