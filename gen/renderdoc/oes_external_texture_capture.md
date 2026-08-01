# RenderDoc OES_EGL_image_external 捕获与回放机制

> 分类：RenderDoc / OpenGL 驱动 / 捕获与回放
> 适用范围：Android GLES（OES_EGL_image_external，`samplerExternalOES`）
> 目标设备：小米 / OPPO / vivo，Android 9+
> 版本：v1.43_oes 分支

## 1. 背景与问题

在 Android 视频编辑 / 播放场景中，应用通常通过 `glEGLImageTargetTexture2DOES` 把
`GL_TEXTURE_EXTERNAL_OES`（由 `SurfaceTexture` / `EGLImage` 支持的外部纹理）绑定到当前
纹理单元，再用 `samplerExternalOES` 在着色器中采样。

### 1.1 根本障碍：OES 纹理无法查询大小

`GL_TEXTURE_EXTERNAL_OES` 是**无存储纹理（storageless texture）**，驱动本身不维护
`TEXTURE_WIDTH` / `TEXTURE_HEIGHT` 状态。因此：

- `glGetTexLevelParameteriv(TEXTURE_WIDTH)` 返回 `0`，无法用该 API 取得尺寸。
- 应用通常也不预先 `glTexImage2D` 分配存储，故连"从应用调用反推"的路径都不可靠。

原方案（2D 快照 + `samplerExternalOES`→`sampler2D` 改写）依赖"查询到 level-0 宽高"
这一前提，而该前提在 OES 纹理上**不成立**，因此被推翻。

### 1.2 正确思路：尺寸来自 AHardwareBuffer

外部纹理的像素实际由一块 `AHardwareBuffer` 承载，其尺寸是唯一可靠来源。
完整链路为：

```
AHardwareBuffer
   │  eglGetNativeClientBufferANDROID   (显式绑定，C++ 路径)
   ▼
EGLClientBuffer (= void*)
   │  eglCreateImageKHR (target=EGL_NATIVE_BUFFER_ANDROID)
   ▼
EGLImageKHR
   │  glEGLImageTargetTexture2DOES
   ▼
GL_TEXTURE_EXTERNAL_OES (纹理)
```

只要在前两条边上用 hook 建立 map 关联，到 `glEGLImageTargetTexture2DOES` 时即可
由 `EGLImageKHR` 反查出 `AHardwareBuffer`，进而用 NDK 官方 API `AHardwareBuffer_describe`
拿到宽高，再用 lock / 独立线程采样拿到像素。

> 偏移兜底：Java `SurfaceTexture` 路径下 framework 不调用
> `eglGetNativeClientBufferANDROID`，map 无法填充。此时 `EGLClientBuffer` 实际即为
> `ANativeWindowBuffer*`，而 `ANativeWindowBuffer` 与 `AHardwareBuffer` 之间为固定偏移
> 关系（经验值 `0x10`），故可 `AHardwareBuffer* = (char*)clientBuffer - 0x10` 强行推断。
> 该值不实用于状态机，仅作为 map 查不到时的兜底。

## 2. 总体方案

```
捕获期 (Capture)                                    回放期 (Replay)
─────────────────────────────────────────          ─────────────────────────────────────────
[链路 map 建立]
 eglGetNativeClientBufferANDROID ─► AHW↔CB 进表
 eglCreateImageKHR (NATIVE_BUFFER) ─► Image↔CB 进表
[捕获分支] glEGLImageTargetTexture2DOES
   │  由 image 反查 AHardwareBuffer (map 优先, 失败则 -0x10 兜底)
   │  AHardwareBuffer_describe 取 w/h/format
   ▼
CaptureExternalOESPixels()   双分支
   ├─ RGBA_8888 : AHardwareBuffer_lock 直接读内存
   └─ 其他格式   : 独立线程+独立EGL ctx 绑 OES 采样烤成 RGBA
   ▼
登记 ResourceId, 填充 TextureData(RGBA), 像素落盘 ────┐
                                                        │
Serialise_glEGLImageTargetTexture2DOES                 │
   序列化: target / texId / capturedId / activeUnit ───┘
                                                        ▼
                                         回放: 一律 RGBA
                                         用 AHardwareBuffer 承载捕获像素
                                         重建真实 OES 纹理 (仍发 glEGLImageTargetTexture2DOES)
                                         绑到原纹理单元, shader 不做改写
```

核心思想：

1. **尺寸与像素来源**全部从 `AHardwareBuffer` 取得（NDK 官方 API，零布局假设）。
2. **捕获双分支**：RGBA_8888 走 `lock` 直接读；其余格式走独立线程采样烤成 RGBA。
3. **回放保真**：回放端一律用 RGBA 的 `AHardwareBuffer` 重建真实 OES 纹理（仍发真实
   `glEGLImageTargetTexture2DOES`），**不再改写 shader**，规避 `samplerExternalOES` 兼容问题。

## 3. 关键数据结构

### 3.1 三段式 map（`gl_driver.h` `WrappedOpenGL` 新增成员）

```cpp
// AHW <-> CB 双向关联 (hook eglGetNativeClientBufferANDROID 时填充)
std::map<AHardwareBuffer*, EGLClientBuffer> m_AHWToCB;
std::map<EGLClientBuffer,  AHardwareBuffer*> m_CBToAHW;

// Image <-> CB 关联 (hook eglCreateImageKHR 时填充)
std::map<EGLImageKHR, EGLClientBuffer> m_ImageToCB;

// Java SurfaceTexture 路径兜底偏移 (ANativeWindowBuffer - 0x10 = AHardwareBuffer)
int64_t g_ClientBufferToAHBOffset = 0x10;

// 外部 OES 纹理 ResourceId -> 已捕获 RGBA 快照 ResourceId
std::map<ResourceId, ResourceId> m_ExternalOESSnapshot;
```

### 3.2 捕获线程所需独立 EGL 上下文（`gl_driver.h` 新增成员）

```cpp
// 独立线程 + 独立 EGL context, 用于非 RGBA_8888 格式的 OES 采样烘烤
EGLDisplay m_CaptureDisplay = EGL_NO_DISPLAY;
EGLContext m_CaptureContext = EGL_NO_CONTEXT;
std::mutex m_CaptureMutex;
```

### 3.3 新增方法声明（`gl_driver.h`）

```cpp
// 由 image 反查 AHardwareBuffer 并取像素, 返回建好的 RGBA 快照 ResourceId
ResourceId CaptureExternalOESPixels(EGLImageKHR image, ResourceId externalId,
                                    GLuint externalName);
```

## 4. 链路 map 建立（hook 实现）

### 4.1 hook `eglGetNativeClientBufferANDROID`（`gl_egl_funcs.cpp`）

```cpp
EGLClientBuffer Cap_eglGetNativeClientBufferANDROID(const AHardwareBuffer *buffer) {
    EGLClientBuffer cb = Real_eglGetNativeClientBufferANDROID(buffer);
    m_AHWToCB[(AHardwareBuffer*)buffer] = cb;
    m_CBToAHW[cb] = (AHardwareBuffer*)buffer;
    return cb;
}
```

> 该 hook 仅在 C++ 显式调用 `eglGetNativeClientBufferANDROID` 时触发，用于填充 AHW↔CB。
> Java `SurfaceTexture` 路径不经过此函数，对应关联留空，由后续 offset 兜底补充。

### 4.2 hook `eglCreateImageKHR`（`gl_egl_funcs.cpp`）

仅对 `target == EGL_NATIVE_BUFFER_ANDROID`（0x3140）的记录 Image↔CB 关联：

```cpp
EGLImageKHR Cap_eglCreateImageKHR(EGLDisplay dpy, EGLContext ctx, EGLenum target,
                                  EGLClientBuffer buffer, const EGLint *attrib_list) {
    EGLImageKHR img = Real_eglCreateImageKHR(dpy, ctx, target, buffer, attrib_list);
    if (target == EGL_NATIVE_BUFFER_ANDROID && img != EGL_NO_IMAGE_KHR)
        m_ImageToCB[img] = buffer;
    return img;
}
```

## 5. 捕获端实现

### 5.1 `glEGLImageTargetTexture2DOES` 捕获分支（`gl_texture_funcs.cpp`）

当 `target == GL_TEXTURE_EXTERNAL_OES` 时，先反查 `AHardwareBuffer`，再进入像素捕获：

```cpp
AHardwareBuffer *ahb = nullptr;

// 路径一: 通过 map 链路反查
auto it = m_ImageToCB.find((EGLImageKHR)image);
if (it != m_ImageToCB.end()) {
    auto it2 = m_CBToAHW.find(it->second);
    if (it2 != m_CBToAHW.end())
        ahb = it2->second;
}

// 路径二: map 查不到 (Java SurfaceTexture 路径), 用偏移兜底推断
if (!ahb)
    ahb = (AHardwareBuffer*)((char*)image - g_ClientBufferToAHBOffset);

if (ahb) {
    ResourceId snapId = CaptureExternalOESPixels((EGLImageKHR)image, externalId, externalName);
    MarkResourceFrameReferenced(snapId, eFrameRef_Read);
} else {
    // 极端兜底: 取当前 viewport 尺寸 (丢帧风险, 仅记录告警)
    GLint vp[4] = {0};
    GL.glGetIntegerv(eGL_VIEWPORT, vp);
    // 以 viewport 尺寸登记, 像素留空
}
```

### 5.2 像素捕获双分支 `CaptureExternalOESPixels`

```cpp
ResourceId CaptureExternalOESPixels(EGLImageKHR image, ResourceId externalId, GLuint externalName) {
    // 去重: 同一外部纹理本帧已烤过则直接返回
    auto cached = m_ExternalOESSnapshot.find(externalId);
    if (cached != m_ExternalOESSnapshot.end())
        return cached->second;

    // 反查 AHardwareBuffer (同 5.1 逻辑)
    AHardwareBuffer *ahb = ResolveAHB(image);

    AHardwareBuffer_Desc desc = {};
    AHardwareBuffer_describe(ahb, &desc);   // 拿到 width / height / format

    uint8_t *pixels = nullptr;
    ResourceId snapId;

    if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM) {
        // 分支一: RGBA_8888 直接 lock 读内存
        void *addr = nullptr;
        AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &addr);
        // 处理 stride(行跨距) 拷贝出连续 RGBA 行
        pixels = CopyWithStride((uint8_t*)addr, desc.width, desc.height, desc.stride);
        AHardwareBuffer_unlock(ahb, nullptr);
    } else {
        // 分支二: 其他格式 (YUV 等) 走独立线程 + 独立 EGL ctx 采样烤 RGBA
        pixels = CaptureOESViaOffscreenThread(image, desc.width, desc.height);
    }

    // 登记 RGBA 快照纹理, 填充 TextureData, 像素落盘
    snapId = RegisterRGBASnapshot(pixels, desc.width, desc.height);
    m_ExternalOESSnapshot[externalId] = snapId;
    return snapId;
}
```

### 5.3 分支二：独立线程采样烘烤

- 独立线程持有自己的 `EGLDisplay` / `EGLContext`（通过 `eglGetDisplay` + `eglCreateContext`
  创建，PixelBuffer surface 或离线 framebuffer）。
- 在该 ctx 下用 `glEGLImageTargetTexture2DOES` 把 `image` 绑成 OES 纹理，再用全屏三角形 +
  极简着色器（`samplerExternalOES` + `texture2D`）渲染进 FBO，读回 `glReadPixels`
  得到 RGBA。
- 该 ctx 与主线程 ctx 完全隔离，不污染应用 GL 状态。
- 线程随捕获会话初始化 / 销毁，`m_CaptureMutex` 保护跨线程像素拷贝。

## 6. 回放端实现

### 6.1 序列化内容（`Serialise_glEGLImageTargetTexture2DOES`）

除原 `target` / `texId` 外，新增：

- `capturedId`：外部 OES 情形为捕获阶段建好的 RGBA 快照 ResourceId。
- `activeUnit`：应用当时绑定的纹理单元（`GetCtxData().m_TextureUnit`）。
- 宽高经 `TextureData` 随资源一并序列化，无需单独字段。

### 6.2 回放重建真实 OES 纹理（仍发真实外部绑定）

回放端**不改写 shader**，而是用 `AHardwareBuffer` 承载捕获到的 RGBA 像素，重建一张真实
的 `GL_TEXTURE_EXTERNAL_OES`：

```cpp
// 1. 用捕获的 RGBA 像素创建 AHardwareBuffer
AHardwareBuffer *ahb = CreateRGBAHardwareBuffer(w, h, capturedPixels);

// 2. 关联 EGLClientBuffer / EGLImage
EGLClientBuffer cb = Real_eglGetNativeClientBufferANDROID(ahb);
EGLImageKHR img = Real_eglCreateImageKHR(dpy, ctx,
                                         EGL_NATIVE_BUFFER_ANDROID, cb, nullptr);

// 3. 仍发真实外部绑定 (回放环境由我们自己的 AHardwareBuffer 提供源)
GLint prevActive = 0;
GL.glGetIntegerv(eGL_ACTIVE_TEXTURE, &prevActive);
GL.glActiveTexture((RDCGLenum)activeUnit);
GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, texName);
Real_glEGLImageTargetTexture2DOES(eGL_TEXTURE_EXTERNAL_OES, img);
GL.glActiveTexture((RDCGLenum)prevActive);
```

由于回放时 OES 纹理的输入就是我们自己捕获的 RGBA `AHardwareBuffer`，`samplerExternalOES`
采样结果与捕获帧完全一致，无需任何 shader 语义改写，彻底规避原方案的兼容问题。

### 6.3 无需 shader 改写（`gl_shader_funcs.cpp`）

回放端用真实 OES 纹理（由我们自己的 `AHardwareBuffer` 提供源），`samplerExternalOES`
采样结果与捕获帧完全一致，**无需对 `Serialise_glShaderSource` 做任何改写**，shader 保持原样即可。
（注：原"2D快照+sampler2D改写"设计从未在代码中落地，此处仅说明新方案不引入任何 shader 改写。）

## 7. 已知限制

1. **静态帧快照**：捕获是 `glEGLImageTargetTexture2DOES` 调用时刻的一次性内容。若同一外部
   纹理在帧内被视频源多次更新，仅捕获末次（或首帧）内容。重度动态视频场景可在
   `Serialise_glDraw*` 路径中对引用的外部纹理重复触发 `CaptureExternalOESPixels`。
2. **offset 兜底为经验值**：`0x10` 偏移针对小米 / OPPO / vivo + Android 9+ 验证；换机型
   若布局变动需重新校准 `g_ClientBufferToAHBOffset`。优先信任 map 链路，兜底仅用于
   Java `SurfaceTexture` 路径。
3. **独立线程开销**：非 RGBA_8888 格式每帧需起 / 复用独立 EGL ctx 上下文做离屏采样，
   有额外线程与上下文切换成本。
4. **仅 Android/EGL 生效**：外部 OES 相关逻辑都在 Android GLES 构建下编译与运行，桌面端不触发。
5. **环境验证**：对应代码改动需在 Android NDK 构建下编译并真机捕获 / 回放验证；当前环境仅做
   静态类型 / 枚举 / 资源登记路径核查。

## 8. 涉及文件清单

| 文件 | 改动 |
| --- | --- |
| `renderdoc/driver/gl/gl_driver.h` | 新增三段式 map（`m_AHWToCB` / `m_CBToAHW` / `m_ImageToCB`）、`g_ClientBufferToAHBOffset`、独立捕获 EGL ctx 成员、`m_ExternalOESSnapshot`、`CaptureExternalOESPixels` 声明 |
| `renderdoc/driver/gl/wrappers/gl_egl_funcs.cpp` | 新增 `eglGetNativeClientBufferANDROID` / `eglCreateImageKHR` 的 hook，填充 map |
| `renderdoc/driver/gl/wrappers/gl_texture_funcs.cpp` | 实现 `glEGLImageTargetTexture2DOES` 与 `Serialise_glEGLImageTargetTexture2DOES`（含 snapId 序列化与回放重建），捕获分支触发 `CaptureExternalOESPixels` |
| `renderdoc/driver/gl/gl_oes_capture.cpp` | **新增**：OES 捕获核心模块——`CaptureHook_eglGetNativeClientBufferANDROID` / `CaptureHook_eglCreateImageKHR`（填 map）、`ResolveAHardwareBuffer`（map 优先 + offset 兜底）、`CaptureExternalOESPixels`（RGBA lock / 离屏采样双分支）、`ReplayExternalOES`（回放重建真实 OES） |
| `renderdoc/driver/gl/egl_hooks.cpp` | `eglCreateImageKHR` 改为自定义 hook（捕获态通知 driver 填 map）；新增 `eglGetNativeClientBufferANDROID` hook（填 map） |
| `renderdoc/driver/gl/gl_dispatch_table_defs.h` | `glEGLImageTargetTexture2DOES` 由 `UnsupportedWrapper2` 改为 `FuncWrapper2` 接入 driver |
| `renderdoc/driver/gl/gl_driver.cpp` | `ProcessChunk` 中新增 `case GLChunk::glEGLImageTargetTexture2DOES` 分发到 `Serialise_glEGLImageTargetTexture2DOES`（回放初始化与逐帧回放均需此 case） |
| `renderdoc/driver/gl/CMakeLists.txt` | 新增 `gl_oes_capture.cpp` 到源列表 |
| `renderdoc/driver/gl/wrappers/gl_shader_funcs.cpp` | 无需改动（回放用真实 OES，不改写 shader） |
| NDK 头 `<android/hardware_buffer.h>` / `<android/native_window.h>` | Vulkan 目录 `vk_android.cpp` 已引入，证明 Android 构建下 NDK 头可用 |
