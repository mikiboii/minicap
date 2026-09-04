
# I:\android-ndk-r10e\ndk-build.cmd -j8

W:\android-ndk-r27d\ndk-build.cmd  -j8

$dir = "/data/local/tmp/miki_2"

# Keep compatible with older devices that don't have `mkdir -p`.
# adb shell "mkdir -p $dir"

adb shell "mkdir $dir 2>/dev/null"




adb push "C:\Users\miki\Downloads\Programs\cpp\minicap\jni\minicap-shared\aosp\libs\android-31\arm64-v8a\minicap.so" "$dir/"

adb push "C:\Users\miki\Downloads\Programs\cpp\minicap\libs\arm64-v8a\minicap" "$dir/"

adb push "C:\Users\miki\Downloads\Programs\cpp\minicap\info\SF_so\libsf_r31.so" "$dir/"


adb forward tcp:8080 tcp:8080

adb shell chmod +x /data/local/tmp/miki_2/minicap
adb shell "chmod +x $dir/minicap"


adb shell LD_LIBRARY_PATH=/data/local/tmp/miki_2 /data/local/tmp/miki_2/minicap 

# Run binary on device
# adb shell /data/local/tmp/hello

# Keep PowerShell open
# Write-Host "`nPress Enter to exit..."
# $null = Read-Host
