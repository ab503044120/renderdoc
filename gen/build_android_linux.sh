#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# RenderDoc Android APK 一键构建脚本 (Linux)
#
# 用法:
#   ./gen/build_android_linux.sh                 # 默认 arm64-v8a + build-tools 30.0.3
#   ANDROID_ABI=armeabi-v7a ./gen/build_android_linux.sh
#   BUILD_TOOLS=34.0.0 NDK_VER=21.4.7075529 ./gen/build_android_linux.sh
#
# 前置要求:
#   - JDK 8 (编译 APK 需要 $JAVA_HOME/jre/lib/rt.jar, JDK9+ 已移除)
#   - Android SDK (含 build-tools / platforms)
#   - Android NDK (含 build/cmake/android.toolchain.cmake)
#   - cmake / make
# ---------------------------------------------------------------------------
set -euo pipefail

# ====================== 可按需修改的默认值 ======================
# 路径默认按当前 Linux 环境填写, 也可用环境变量覆盖
SDK_ROOT="${ANDROID_SDK:-/home/huihui/software/androidsdk}"
NDK_VER="${NDK_VER:-24.0.8215888}"
NDK_VER="${NDK_VER:-24.0.8215888}"
NDK_ROOT="${ANDROID_NDK:-$SDK_ROOT/ndk/$NDK_VER}"
JDK_ROOT="${JDK_ROOT:-/usr/lib/jvm/java-8-openjdk-amd64}"

# 构建参数
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-26}"
BUILD_TOOLS="${BUILD_TOOLS:-27.0.3}"      # 必须用 27.0.3: 28.0.0+ 移除了 aapt, 而 RenderDoc 打包脚本硬编码调用 aapt
BUILD_DIR="${BUILD_DIR:-build-android}"
JOBS="${JOBS:-$(nproc)}"

# ====================== 环境检查 ======================
echo "==> 检查依赖..."

[ -d "$SDK_ROOT" ]                    || { echo "ERROR: SDK 不存在: $SDK_ROOT"; exit 1; }
[ -d "$NDK_ROOT" ]                    || { echo "ERROR: NDK 不存在: $NDK_ROOT"; exit 1; }
[ -f "$NDK_ROOT/build/cmake/android.toolchain.cmake" ] \
                                   || { echo "ERROR: 找不到 android.toolchain.cmake"; exit 1; }
[ -f "$JDK_ROOT/jre/lib/rt.jar" ]    || { echo "ERROR: JDK8 的 rt.jar 不存在: $JDK_ROOT/jre/lib/rt.jar (APK 打包必须用 JDK8)"; exit 1; }
[ -d "$SDK_ROOT/build-tools" ]        || { echo "ERROR: SDK 缺少 build-tools 目录"; exit 1; }
command -v cmake >/dev/null 2>&1      || { echo "ERROR: 找不到 cmake"; exit 1; }
command -v make  >/dev/null 2>&1      || { echo "ERROR: 找不到 make"; exit 1; }

# ====================== 导出环境变量 ======================
export ANDROID_HOME="$SDK_ROOT"
export ANDROID_SDK_ROOT="$SDK_ROOT"
export ANDROID_SDK="$SDK_ROOT"
export ANDROID_NDK="$NDK_ROOT"
export ANDROID_NDK_HOME="$NDK_ROOT"
export JAVA_HOME="$JDK_ROOT"
export PATH="$JAVA_HOME/bin:$PATH"

echo "==> 环境:"
echo "    SDK       = $SDK_ROOT"
echo "    NDK       = $NDK_ROOT"
echo "    JDK       = $JAVA_HOME"
echo "    ABI       = $ANDROID_ABI"
echo "    PLATFORM  = $ANDROID_PLATFORM"
echo "    BUILD_TOOLS = $BUILD_TOOLS"
echo "    JOBS      = $JOBS"

# ====================== 定位仓库根目录 ======================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ====================== CMake 配置 ======================
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "==> 运行 cmake 配置..."
# RelWithDebInfo: 保留调试符号 (含行号信息), 用于 addr2line 定位 crash 点, 同时保持优化
cmake -DBUILD_ANDROID=On \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DANDROID_ABI="$ANDROID_ABI" \
      -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
      -DANDROID_BUILD_TOOLS_VERSION="$BUILD_TOOLS" \
      -G "Unix Makefiles" \
      ..

# ====================== 编译 ======================
echo "==> 开始编译 (make -j$JOBS) ..."
make -j"$JOBS"

# ====================== 产物 ======================
case "$ANDROID_ABI" in
  arm64-v8a)   APK_ABI=arm64 ;;
  armeabi-v7a) APK_ABI=arm32 ;;
  *)           APK_ABI="$ANDROID_ABI" ;;
esac
APK="$REPO_ROOT/$BUILD_DIR/bin/org.renderdoc.renderdoccmd.$APK_ABI.apk"

echo "==> 构建完成"
if [ -f "$APK" ]; then
  echo "    APK: $APK"
else
  echo "    WARN: 未找到预期 APK: $APK"
fi

# ====================== 拷贝到 RenderDoc 期望的目标位置 ======================
# RenderDoc (host 端) 在 InstallRenderDocServer() 中按设备“首个 ABI”拼文件名查找 APK。
# 对 arm64 设备, GetSupportedABIs() 返回 {armeabi_v7a, arm64_v8a} (32 位在前),
# 因此即使只编了 arm64, host 也会先找 org.renderdoc.renderdoccmd.arm32.apk。
# 这里把已编出的 APK 同时以另一 ABI 的名字拷贝一份, 保证两种情况下都能被找到。
# (安装阶段 host 会按真实 ABI 安装: 64 位设备自动跳过 32 位包, 反之亦然)
APK_DIR="$REPO_ROOT/$BUILD_DIR/bin"
case "$APK_ABI" in
  arm64) ALT_ABI=arm32; ALT_DIR="$REPO_ROOT/build-android-arm64/bin" ;;
  arm32) ALT_ABI=arm64; ALT_DIR="$REPO_ROOT/build-android-arm32/bin" ;;
  *)     ALT_ABI="";   ALT_DIR="" ;;
esac

if [ -n "$ALT_ABI" ] && [ -f "$APK" ]; then
  # 1) 同目录下的别名 APK: 满足 build-android/bin/ 这一主要搜索路径
  #    始终覆盖拷贝, 保证重新构建后副本与最新产物一致
  ALT_APK="$APK_DIR/org.renderdoc.renderdoccmd.$ALT_ABI.apk"
  echo "==> 拷贝别名 APK: $ALT_APK"
  cp -f "$APK" "$ALT_APK"

  # 2) ABI 专属目录: 满足 build-android-<abi>/bin/ 这一搜索路径
  if [ -n "$ALT_DIR" ]; then
    mkdir -p "$ALT_DIR"
    ABI_APK="$ALT_DIR/org.renderdoc.renderdoccmd.$APK_ABI.apk"
    echo "==> 拷贝到 ABI 专属目录: $ABI_APK"
    cp -f "$APK" "$ABI_APK"
  fi
fi

echo "==> 目标位置 APK 列表 ($(ls -1 "$APK_DIR"/*.apk 2>/dev/null | wc -l) 个):"
ls -1 "$APK_DIR"/*.apk 2>/dev/null || true

# ====================== 拷贝到 VS (Windows) 运行目录 ======================
# Windows 下用 VS 运行 qrenderdoc 时, libDir = qrenderdoc/renderdoc 所在目录 (如 x64/Development),
# host 端 InstallRenderDocServer() 优先搜索 libDir/plugins/android/ 这个 “Windows install” 路径。
# 因此把 APK 及其 ABI 别名一并拷到该目录, 保证 VS 直接 F5 运行就能找到 (无需依赖 build-android/bin)。
# 可用环境变量 VS_OUTPUT_DIR 覆盖 (例如 Release / Debug), 默认 x64/Development。
VS_DIR="${VS_OUTPUT_DIR:-x64/Development}"
VS_PLUGIN_DIR="$REPO_ROOT/$VS_DIR/plugins/android"
mkdir -p "$VS_PLUGIN_DIR"

echo "==> 拷贝到 VS 运行目录: $VS_PLUGIN_DIR"
for name in "$APK_ABI" $ALT_ABI; do
  [ -z "$name" ] && continue
  src="$APK_DIR/org.renderdoc.renderdoccmd.$name.apk"
  if [ ! -f "$src" ]; then
    echo "    WARN: 源 APK 不存在, 跳过: $src"
    continue
  fi
  dst="$VS_PLUGIN_DIR/org.renderdoc.renderdoccmd.$name.apk"
  cp -f "$src" "$dst"
  echo "    -> $dst"
done
