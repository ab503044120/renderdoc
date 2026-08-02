# OES 外部纹理捕获与回放机制

> 分类：RenderDoc / OpenGL 驱动 / 捕获与回放
> 适用范围：Android GLES（OES_EGL_image_external，`samplerExternalOES`）
> 目标设备：小米 / OPPO / vivo，Android 9+
> 版本：v1.43_oes 分支
> 代码同步说明：本文档已与实际代码实现同步，覆盖主体功能（提交 `89a37da5c`）与
> 资源生命周期加固（提交 `6a8da6592`，新增 `eglDestroyImage` hook 与 `AHardwareBuffer_release` 释放）。

## 1. 背景与问题

在 Android 视频编辑 / 播放场景中，应用通常通过 `glEGLImageTargetTexture2DOES` 把
`GL_TEXTURE_EXTERNAL_OES`（由 `SurfaceTexture` / `EGLImage` 支持的外部纹理）绑定到当前
纹理单元，再用 `samplerExternalOES` 在着色器中采样。

### 1.1 根本障碍：OES 纹理无法查询大小

`GL_TEXTURE_EXTERNAL_OES` 是**无存储纹理（storageless texture）**，驱动本身不维护
`TEXTURE_WIDTH` / `TEXTURE_HEIGHT` 状态。因此：

- `glGetTexLevelParameteriv(TEXTURE_WIDTH)` 返回 `0`，无法用该 API 取得尺寸。
- 应用通常也不预先 `glTexImage2D` 分配存储，故连"从应用调用反推"的路径都不可靠。
- OES 纹理不能通过 `glGetTexImage` 读回，也不能直接 attach 到 FBO 采样。

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
> 关系（初始经验值 `0x10`）。该值可在 map 链路成功解析时**动态校准**（见 §5.1），
> 仅作为 map 查不到时的兜底，不实用于状态机。

## 2. 总体方案

```
捕获期 (Capture)                                    回放期 (Replay)
─────────────────────────────────────────          ─────────────────────────────────────────
[链路 map 建立]
 eglGetNativeClientBufferANDROID ─► AHW↔CB 进表
 eglCreateImageKHR (NATIVE_BUFFER) ─► Image↔CB 进表
 eglDestroyImage                   ─► Image↔CB 出表（资源清理）
[捕获分支] glEGLImageTargetTexture2DOES
   │  由 image 反查 AHardwareBuffer (map 优先, 失败则 offset 兜底)
   │  AHardwareBuffer_describe 取 w/h/format + sanity-check
   ▼
CaptureExternalOESPixels()   双分支
   ├─ RGBA_8888 : AHardwareBuffer_lock 直接读内存
   └─ 其他格式   : 独立线程+独立EGL ctx 绑 OES 采样烤成 RGBA
   ▼
登记 ResourceId, 填充 TextureData(RGBA), 像素落盘 ────┐
    (BMP dump, 可选调试)                                │
Serialise_glEGLImageTargetTexture2DOES                 │
   序列化: target / imageHandle / snapId ──────────────┘
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

## 3. 关键数据结构（`gl_driver.h` `WrappedOpenGL`）

### 3.1 三段式 map

```cpp
// AHW <-> CB 双向关联（钩子 eglGetNativeClientBufferANDROID 时填充）
// 类型用 const void* 而非 AHardwareBuffer*，以兼容 ANativeWindowBuffer 的指针语义
std::map<const void *, EGLClientBuffer> m_AHWToCB;
std::map<EGLClientBuffer, const void *> m_CBToAHW;

// Image <-> CB 关联 (hook eglCreateImageKHR 时填充, eglDestroyImage 时清理)
std::map<EGLImageKHR, EGLClientBuffer> m_ImageToCB;

// Java SurfaceTexture 路径兜底偏移 (ANativeWindowBuffer - offset = AHardwareBuffer)
// 初始 0x10，可在 map 链路成功解析时动态校准
int64_t g_ClientBufferToAHBOffset = 0x10;

// 外部 OES 纹理 ResourceId -> 已捕获 RGBA 快照 ResourceId (每帧清空)
std::map<ResourceId, ResourceId> m_ExternalOESSnapshot;

// 捕获阶段算出的 snapId，因 hook 函数签名无法携带该值，故先暂存于此；
// Serialise_glEGLImageTargetTexture2DOES 在写路径通过 ser.Serialise("snapId"_lit, ...) 读取，
// 而非通过 SERIALISE_ELEMENT_LOCAL 传入（SERIALISE_ELEMENT_LOCAL 仅用于 imageHandle 字段）
ResourceId m_OESCurrentSnapId = ResourceId();
```

### 3.2 捕获线程与独立 EGL 上下文（worker 线程模型）

```cpp
// 一次性采样任务：由调用线程提交，工作线程完成，经条件变量取回
struct OESSampleJob {
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  uint32_t width = 0, height = 0;
  byte *pixels = NULL;   // 由工作线程填充，由调用线程释放
  bool done = false, success = false;
};

// 线程与上下文相关成员
std::mutex m_CaptureMutex;
std::condition_variable m_CaptureCV;
std::mutex m_CaptureQueueMutex;
bool m_CaptureThreadInit = false;   // 线程已初始化
bool m_CaptureThreadExit = false;   // 请求退出
std::thread m_CaptureThread;
EGLDisplay m_CaptureDisplay = EGL_NO_DISPLAY;
EGLContext m_CaptureContext = EGL_NO_CONTEXT;      // 工作线程自有的上下文
EGLContext m_CaptureShareContext = EGL_NO_CONTEXT; // 与调用线程共享的组
EGLConfig m_CaptureConfig = NULL;                  // 调用线程选定的配置
EGLSurface m_CaptureSurface = EGL_NO_SURFACE;      // 工作线程的 pbuffer 表面

// 任务提交/完成同步
OESSampleJob m_CaptureJob;
std::mutex m_CaptureJobMutex;
std::condition_variable m_CaptureJobSubmitCV;  // 调用线程 -> 工作线程：新任务就绪
std::condition_variable m_CaptureJobDoneCV;    // 工作线程 -> 调用线程：任务完成

// 离屏采样路径防递归: SampleOESToRGBA 会回调 GL.glEGLImageTargetTexture2DOES
bool m_InOESSample = false;
```

### 3.3 新增方法声明

```cpp
IMPLEMENT_FUNCTION_SERIALISED(void, glEGLImageTargetTexture2DOES, GLenum target, GLeglImageOES image);

// EGL hook 层回调, 维护 AHW<->CB<->Image 三段 map
void CaptureHook_eglGetNativeClientBufferANDROID(const void *buffer, EGLClientBuffer cb);
void CaptureHook_eglCreateImage(EGLenum target, EGLClientBuffer buffer, EGLImageKHR image);
void CaptureHook_eglDestroyImage(EGLImageKHR image);   // 清理 m_ImageToCB

// 解析 EGLImage 背后的 AHardwareBuffer (map 优先 + offset 兜底)
void *ResolveAHardwareBuffer(EGLImageKHR image);
// 捕获外部 OES 纹理像素为 RGBA 快照, 返回其 ResourceId
ResourceId CaptureExternalOESPixels(EGLImageKHR image, ResourceId externalId, GLuint externalName);
// 回放: 用捕获的 RGBA 快照重建真实 OES 纹理
void ReplayExternalOES(ResourceId snapId, GLuint oesTexture);
```

## 4. 链路 map 建立（EGL hook 层）

### 4.1 `egl_dispatch_table.h` 扩展

- `EGL_HOOKED_SYMBOLS` 新增 `CreateImage`（EGL 1.5 core）与 `CreateImageKHR`（扩展）。
- 新增 `EGL_ANDROID_HOOKED_SYMBOLS(FUNC)` 宏，Android 下含 `GetNativeClientBufferANDROID`，
  非 Android 下为空；统一接入 `EGL_PTR_GEN` 生成 dispatch 成员。
- 新增 `typedef PFNEGLCREATEIMAGEKHRPROC PFN_eglCreateImageKHR`、
  `PFNEGLCREATEIMAGEPROC PFN_eglCreateImage`、
  `PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC PFN_eglGetNativeClientBufferANDROID`。

### 4.2 `egl_hooks.cpp` 自定义 hook（替代原 passthru）

`eglCreateImage` / `eglCreateImageKHR` 均替换为自定义 hook，调用真实实现后在捕获态
（`!RenderDoc::Inst().IsReplayApp()` 且 `img != EGL_NO_IMAGE_KHR`）回调
`eglhook.driver.CaptureHook_eglCreateImage(target, buffer, img)`：

```cpp
HOOK_EXPORT EGLImageKHR EGLAPIENTRY eglCreateImage_renderdoc_hooked(
    EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLAttrib *attrib_list) {
  EnsureRealLibraryLoaded();
  EGLImageKHR img = EGL.CreateImage(dpy, ctx, target, buffer, attrib_list);
#if ENABLED(RDOC_ANDROID)
  if(!RenderDoc::Inst().IsReplayApp() && img != EGL_NO_IMAGE_KHR)
    eglhook.driver.CaptureHook_eglCreateImage(target, buffer, img);
#endif
  return img;
}
```

新增 `eglGetNativeClientBufferANDROID` hook，捕获态回调
`CaptureHook_eglGetNativeClientBufferANDROID(buffer, cb)`。

**资源清理（提交 `6a8da6592`）**：`eglDestroyImage` 由 `EGL_PASSTHRU_2` 替换为自定义 hook，
调用真实实现后，捕获态且成功（`ret` 非 0）时回调 `CaptureHook_eglDestroyImage(image)`：

```cpp
HOOK_EXPORT EGLBoolean EGLAPIENTRY eglDestroyImage_renderdoc_hooked(EGLDisplay dpy, EGLImageKHR image) {
  EnsureRealLibraryLoaded();
  typedef EGLBoolean (*eglDestroyImage_hooktype)(EGLDisplay, EGLImageKHR);
  eglDestroyImage_hooktype real =
      (eglDestroyImage_hooktype)Process::GetFunctionAddress(eglhook.handle, "eglDestroyImage");
  EGLBoolean ret = real(dpy, image);
#if ENABLED(RDOC_ANDROID)
  if(!RenderDoc::Inst().IsReplayApp() && ret)
    eglhook.driver.CaptureHook_eglDestroyImage(image);  // 清理 map
#endif
  return ret;
}
HOOK_EXPORT EGLBoolean EGLAPIENTRY eglDestroyImage(EGLDisplay dpy, EGLImageKHR image) {
  return eglDestroyImage_renderdoc_hooked(dpy, image);
}
```

### 4.3 `gl_oes_capture.cpp` map 填充/清理回调

```cpp
void WrappedOpenGL::CaptureHook_eglGetNativeClientBufferANDROID(const void *buffer, EGLClientBuffer cb) {
  if(buffer == NULL || cb == NULL) return;
  m_AHWToCB[buffer] = cb;
  m_CBToAHW[cb] = buffer;
}
void WrappedOpenGL::CaptureHook_eglCreateImage(EGLenum target, EGLClientBuffer buffer, EGLImageKHR image) {
  if(target == EGL_NATIVE_BUFFER_ANDROID && image != EGL_NO_IMAGE_KHR && buffer != NULL)
    m_ImageToCB[image] = buffer;   // 仅对 NATIVE_BUFFER 目标记录
}
void WrappedOpenGL::CaptureHook_eglDestroyImage(EGLImageKHR image) {
  RDCLOG("CaptureHook_eglDestroyImage: image=%p", (void *)image);
  if(image != EGL_NO_IMAGE_KHR)
    m_ImageToCB.erase(image);   // 清理 Image<->CB 关联, 防止 map 无限增长
}
```

### 4.4 AHardwareBuffer / EGL 入口解析

`gl_oes_capture.cpp` 不直接链接 NDK 的 `AHardwareBuffer_*` 函数（NDK 24 将其标记为
`__INTRODUCED_IN(26)`，在低 minSdk 下不可用），改为在运行时从 `libandroid.so` 动态解析：

```cpp
typedef void (*PFN_AHardwareBuffer_describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
typedef int  (*PFN_AHardwareBuffer_lock)(AHardwareBuffer *, uint64_t, int32_t, const ARect *, void **);
typedef int  (*PFN_AHardwareBuffer_unlock)(AHardwareBuffer *, int32_t *);
typedef int  (*PFN_AHardwareBuffer_allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
typedef void (*PFN_AHardwareBuffer_release)(AHardwareBuffer *);   // 提交 6a8da6592 新增
// InitOESAndroidFuncs() 通过 Process::GetFunctionAddress(android, "AHardwareBuffer_xxx") 填充
```

EGL 入口（`GetCurrentDisplay` / `GetNativeClientBufferANDROID` / `CreateImage` / `CreateImageKHR`）
直接走 dispatch 表 `EGL.xxx`，遵循"使用 Android 原生 EGL"约定，不额外封装。

## 5. 捕获端实现

### 5.1 `ResolveAHardwareBuffer`（三级解析 + offset 动态校准）

```cpp
void *WrappedOpenGL::ResolveAHardwareBuffer(EGLImageKHR image) {
  // Path 1: 走 map 链路 (C++ 路径, 调用了 eglGetNativeClientBufferANDROID)
  auto it = m_ImageToCB.find(image);
  if(it != m_ImageToCB.end()) {
    auto it2 = m_CBToAHW.find(it->second);
    if(it2 != m_CBToAHW.end()) {
      // 用 cb - ahw 校准 g_ClientBufferToAHBOffset, 供 Path 2/3 使用
      int64_t offset = (int64_t)((char *)it->second - (char *)it2->second);
      if(offset != g_ClientBufferToAHBOffset) { g_ClientBufferToAHBOffset = offset; ... }
      return (void *)it2->second;
    }
    // Path 2: Image 在表, CB->AHW 空 (Java SurfaceTexture) => cb - offset
    return (void *)((char *)it->second - g_ClientBufferToAHBOffset);
  }
  // Path 3: 两张表都无 (Java 路径) => EGLImageKHR 即 ANativeWindowBuffer* => image - offset
  return (void *)((char *)image - g_ClientBufferToAHBOffset);
}
```

### 5.2 `glEGLImageTargetTexture2DOES` 捕获分支（`gl_texture_funcs.cpp`）

先转发真实调用保证应用正常运行，再在捕获态对 EXTERNAL_OES 触发像素捕获：

```cpp
void WrappedOpenGL::glEGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image) {
  SERIALISE_TIME_CALL(GL.glEGLImageTargetTexture2DOES(target, image));  // 先转发
  if(IsReplayMode(m_State)) return;
  if(m_InOESSample) return;   // 防递归: 离屏采样会回调本函数

  ResourceId snapId = ResourceId();
  if(IsActiveCapturing(m_State)) {
    if(target == eGL_TEXTURE_EXTERNAL_OES) {
      GLResourceRecord *record = GetCtxData().GetActiveTexRecord(target);
      if(record != NULL) {
        ResourceId externalId = record->GetResourceID();
        snapId = CaptureExternalOESPixels((EGLImageKHR)image, externalId, record->Resource.name);
        if(snapId != ResourceId())
          GetResourceManager()->MarkResourceFrameReferenced(snapId, eFrameRef_Read);
      }
    }
    m_OESCurrentSnapId = snapId;   // 暂存供 Serialise 使用
    USE_SCRATCH_SERIALISER();
    SCOPED_SERIALISE_CHUNK(gl_CurChunk);
    Serialise_glEGLImageTargetTexture2DOES(ser, target, image);
    GetContextRecord()->AddChunk(scope.Get());
  }
  else if(IsCaptureMode(m_State) && target == eGL_TEXTURE_EXTERNAL_OES) {
    // 仅触发捕获, 不序列化 chunk
    ResourceId captured = CaptureExternalOESPixels((EGLImageKHR)image, externalId, record->Resource.name);
    ...
  }
}
```

### 5.3 像素捕获双分支 `CaptureExternalOESPixels`

```cpp
ResourceId WrappedOpenGL::CaptureExternalOESPixels(EGLImageKHR image, ResourceId externalId, GLuint externalName) {
  // 去重: 同一外部纹理本帧已烤过则直接返回快照
  auto cached = m_ExternalOESSnapshot.find(externalId);
  if(cached != m_ExternalOESSnapshot.end()) return cached->second;

  void *ahb = ResolveAHardwareBuffer(image);
  if(ahb == NULL) { RDCWARN(...); return ResourceId(); }
  InitOESAndroidFuncs();

  AHardwareBuffer_Desc desc = {};
  AHardwareBuffer_describe((AHardwareBuffer *)ahb, &desc);

  // sanity-check: 非法描述符 (w/h 为 0 或 >16384, format 为 0) 说明 AHB 解析错误, 跳过捕获
  if(desc.width == 0 || desc.height == 0 || desc.format == 0 ||
     desc.width > 16384 || desc.height > 16384) { RDCWARN(...); return ResourceId(); }

  byte *pixels = NULL;
  if(desc.format == R8G8B8A8_UNORM || desc.format == R8G8B8X8_UNORM) {
    // 分支一: RGBA_8888 -> AHardwareBuffer_lock 直接读内存, 按 stride 逐行拷贝成连续 RGBA
    if(AHardwareBuffer_lock(...) == 0 && addr) { memcpy 按 stride 拷贝; AHardwareBuffer_unlock(...); }
  }
  if(pixels == NULL) {
    // 分支二: 其他格式 (YUV 等) -> 提交 OESSampleJob 给独立捕获线程烘烤成 RGBA
    InitCaptureContext(this);   // 起独立 EGL ctx worker 线程
    if(m_CaptureThread.joinable()) {
      // 填 job(image/w/h), notify, wait_for(5s); 超时回退当前 ctx 直接 SampleOESToRGBA
    } else {
      pixels = SampleOESToRGBA(this, image, w, h);   // 无线程则当前 ctx 兜底
    }
  }
  if(pixels == NULL) { RDCWARN(...); return ResourceId(); }

  // 调试: 落盘 BMP 到 app files 目录 (oes_dump_frame%u_%ux%u.bmp)
  // 创建 RGBA8 2D 快照纹理, glTexImage2D 上传像素
  GLuint snap; glGenTextures/glBindTexture/glTexImage2D(RGBA8, w, h, ...);
  GLResource res = TextureRes(GetCtx(), snap);
  ResourceId snapId = GetResourceManager()->RegisterResource(ResourceId(), res);

  if(IsActiveCapturing(m_State)) {
    // 序列化 glGenTextures + glBindTexture 两个 chunk (用显式 GLChunk::glGenTextures,
    // 而非 gl_CurChunk), 保证回放端能重建资源映射与正确 curType
    //   注意: 不用 gl_CurChunk, 否则类型标签错误导致回放反序列化误读
    ...
  }
  // 填充 TextureData(RGBA8, w, h), 触发标准 initial-contents 序列化
  GetResourceManager()->PrepareTextureInitialContents(snapId, ...);

  m_ExternalOESSnapshot[externalId] = snapId;
  free(pixels);
  return snapId;
}
```

### 5.4 分支二：独立线程采样烘烤

- **初始化 `InitCaptureContext`**：在 caller 线程取 `EGL.GetCurrentDisplay()` /
  `EGL.GetCurrentContext()`；用 `EGL.ChooseConfig` 挑选 pbuffer 兼容 config；
  随后 `std::thread` 启动 `OESCaptureThread`。EGL ctx/surface 的创建**必须在 worker 线程**完成，
  避免经 renderdoc hook 层造成锁竞争（caller 已持有相关锁）。
- **worker `OESCaptureThread`**：用**未 hook 的 EGL dispatch**（`EGL.CreateContext` /
  `EGL.CreatePbufferSurface` / `EGL.MakeCurrent`）创建与 caller 共享 group 的 context + pbuffer
  surface，然后等待 job，执行 `SampleOESToRGBA`，写回结果并 notify。
- **采样 `SampleOESToRGBA`**：在独立 ctx 下 `glEGLImageTargetTexture2DOES` 绑 OES 纹理（设
  `m_InOESSample=true` 防递归），用全屏三角形 + 极简着色器（`samplerExternalOES` +
  `texture2D`，`#extension GL_OES_EGL_image_external_essl3 : require`）渲染进 RGBA8 FBO，
  `glReadPixels` 读回。
- **生命周期**：`WrappedOpenGL` 析构时置 `m_CaptureThreadExit=true`、notify 并 `join()` 线程；
  `StartFrameCapture` 清空 `m_ExternalOESSnapshot` 使每帧重新捕获。

## 6. 回放端实现

### 6.1 序列化内容（`Serialise_glEGLImageTargetTexture2DOES`）

序列化三个字段，新格式 20 字节：

- `target`（GLenum，4 字节）
- `imageHandle`（void* 转 uint64，8 字节，`TypedAs("GLeglImageOES")`）
- `snapId`（ResourceId，8 字节）——捕获阶段算好的 RGBA 快照 ResourceId

**向后兼容**：老捕获（12 字节，无 snapId 字段）在读取时仅当
`ser.ChunkMetadata().length >= 20` 才反序列化 snapId，否则 snapId 保持空。

```cpp
SERIALISE_ELEMENT(target);
SERIALISE_ELEMENT_LOCAL(imageHandle, (uint64_t)(uintptr_t)image).TypedAs("GLeglImageOES"_lit);
ResourceId snapId = ResourceId();
if(ser.IsWriting() || ser.ChunkMetadata().length >= 20) {
  ScopedDeserialise<decltype(ser), ResourceId> CONCAT(deserialise_, __LINE__)(ser, snapId);
  if(ser.IsWriting()) snapId = m_OESCurrentSnapId;
  ser.Serialise("snapId"_lit, snapId);
}
SERIALISE_CHECK_READ_ERRORS();
```

> 注：文档初稿曾写"序列化 target/texId/capturedId/activeUnit"，实际代码采用
> `target/imageHandle/snapId`，宽高经快照纹理的 `TextureData` 随资源序列化，不再单列
> activeUnit（回放时通过查询当前绑定 OES 纹理名定位）。

### 6.2 回放重建真实 OES 纹理

回放读取时（`IsReplayingAndReading`）：

- 若 `target == EXTERNAL_OES && snapId != ResourceId()`：
  1. `glGetIntegerv(eGL_TEXTURE_BINDING_EXTERNAL_OES)` 取当前绑定 OES 纹理名；
  2. 遍历 `m_Textures` 按 `resource.name == oesTexName` 反查 ResourceId，失败回退
     `GetActiveTexRecord(target)`；
  3. 调用 `ReplayExternalOES(snapId, texName)` 重建真实 OES 纹理，并
     `MarkResourceFrameReferenced(snapId, eFrameRef_Read)`。
- 若 OES 目标但 snapId 为空：告警，OES 纹理不重建（黑 / 不完整）。

`ReplayExternalOES`（`gl_oes_capture.cpp`）：

```cpp
void WrappedOpenGL::ReplayExternalOES(ResourceId snapId, GLuint oesTexture) {
  // 1. 从 m_Textures[snapId] 取 w/h
  // 2. 分配 RGBA8 AHardwareBuffer (GPU_SAMPLED_IMAGE | CPU_WRITE_OFTEN | CPU_READ_OFTEN)
  //    成功后定义 releaseAHB() RAII, 保证所有出口统一 release
  // 3. 从 snapId 2D 纹理经 FBO glReadPixels 读回像素, AHardwareBuffer_lock + 按 stride 写入
  //    (Adreno 注意: usage 必须含 CPU_READ_OFTEN, 否则 eglCreateImageKHR 可能 crash)
  // 4. eglGetNativeClientBufferANDROID + eglCreateImageKHR 生成 EGLImage
  // 5. glBindTexture(EXTERNAL_OES, oesTexture) + glEGLImageTargetTexture2DOES 重建
}
```

**资源释放（提交 `6a8da6592`）**：`ReplayExternalOES` 中所有在 `AHardwareBuffer_allocate`
成功之后的提前 `return` 出口（快照名无效、lock 失败、EGL 入口缺失、display 无效、
`GetNativeClientBufferANDROID` 返回 NULL、`glEGLImageTargetTexture2DOES` 为 NULL）以及函数末尾，
统一经 `releaseAHB()` 调用 `AHardwareBuffer_release`，杜绝 buffer 泄漏。

```cpp
auto releaseAHB = [&]() {
  if(ahb && OESAndroid.AHardwareBuffer_release)
    OESAndroid.AHardwareBuffer_release(ahb);
};
```

### 6.3 无需 shader 改写（`gl_shader_funcs.cpp`）

回放端用真实 OES 纹理（由我们自己的 `AHardwareBuffer` 提供源），`samplerExternalOES`
采样结果与捕获帧完全一致，**无需对 `Serialise_glShaderSource` 做任何改写**，shader 保持原样即可。
（注：原"2D快照+sampler2D改写"设计从未在代码中落地，此处仅说明新方案不引入任何 shader 改写。）

## 7. 已知限制

1. **静态帧快照**：捕获是 `glEGLImageTargetTexture2DOES` 调用时刻的一次性内容。若同一外部
   纹理在帧内被视频源多次更新，仅捕获末次（或首帧）内容。重度动态视频场景可在
   `Serialise_glDraw*` 路径中对引用的外部纹理重复触发 `CaptureExternalOESPixels`。
2. **offset 兜底为经验值**：初始 `0x10` 针对小米 / OPPO / vivo + Android 9+ 验证；换机型
   若布局变动需重新校准。代码已在 map 链路成功时**动态校准** `g_ClientBufferToAHBOffset`，
   但纯 Java `SurfaceTexture` 路径（两张表皆空）仍依赖初始经验值。
3. **独立线程开销**：非 RGBA_8888 格式每帧需起 / 复用独立 EGL ctx 上下文做离屏采样，
   有额外线程与上下文切换成本；`std::thread` 创建亦有毫秒级开销。
4. **仅 Android/EGL 生效**：外部 OES 相关逻辑都在 Android GLES 构建下编译与运行，桌面端不触发。
5. **环境验证**：对应代码改动需在 Android NDK 构建下编译并真机捕获 / 回放验证；当前环境仅做
   静态类型 / 枚举 / 资源登记路径核查。

## 8. 涉及文件清单（与代码实际一致）

| 文件 | 改动 |
| --- | --- |
| `renderdoc/driver/gl/gl_driver.h` | 三段式 map（`m_AHWToCB` / `m_CBToAHW` / `m_ImageToCB`，`const void*` 键）、`g_ClientBufferToAHBOffset`、`m_ExternalOESSnapshot`、`m_OESCurrentSnapId`、worker 捕获线程全套成员（`OESSampleJob`/`m_CaptureThread`/CV 等）、`m_InOESSample`；方法声明含 `CaptureHook_eglDestroyImage` |
| `renderdoc/driver/gl/egl_hooks.cpp` | `eglCreateImage`/`eglCreateImageKHR` 自定义 hook（回调 `CaptureHook_eglCreateImage`）；新增 `eglGetNativeClientBufferANDROID` hook（回调 `CaptureHook_eglGetNativeClientBufferANDROID`）；`eglDestroyImage` 由 passthru 改为自定义 hook（回调 `CaptureHook_eglDestroyImage`） |
| `renderdoc/driver/gl/egl_dispatch_table.h` | `EGL_HOOKED_SYMBOLS` 增 `CreateImage`/`CreateImageKHR`；新增 `EGL_ANDROID_HOOKED_SYMBOLS`（含 `GetNativeClientBufferANDROID`） |
| `renderdoc/driver/gl/egl_platform.cpp` | `PopulateForReplay` 接入 `EGL_ANDROID_HOOKED_SYMBOLS(LOAD_FUNC)` |
| `renderdoc/driver/gl/gl_oes_capture.cpp` | **新增**：OES 捕获核心模块——`CaptureHook_*`（填/清 map）、`ResolveAHardwareBuffer`（三级 + offset 校准）、`CaptureExternalOESPixels`（RGBA lock / 离屏采样双分支 + sanity-check + BMP dump + 快照纹理序列化）、`SampleOESToRGBA`/`OESCaptureThread`（独立线程采样）、`ReplayExternalOES`（回放重建 + `AHardwareBuffer_release` 全路径释放） |
| `renderdoc/driver/gl/wrappers/gl_texture_funcs.cpp` | 实现 `Serialise_glEGLImageTargetTexture2DOES`（`target`/`imageHandle`/`snapId`，向后兼容）与 `glEGLImageTargetTexture2DOES` hook（捕获触发 + 防递归 + 序列化） |
| `renderdoc/driver/gl/gl_dispatch_table.h` / `gl_dispatch_table_defs.h` | 新增 `glEGLImageTargetTexture2DOES` dispatch 成员；`DefineOESHooks()` / `ForEachSupported_OES`；从 `UnsupportedWrapper2` 移入 `FuncWrapper2` |
| `renderdoc/driver/gl/gl_hooks.cpp` | `PopulateWithCallback` / `HookedGetProcAddress` 接入 `ForEachSupported_OES` |
| `renderdoc/driver/gl/gl_common.h` / `gl_common.cpp` | 新增 `GLChunk::glEGLImageTargetTexture2DOES`（Android only）；纹理索引/枚举增 `eGL_TEXTURE_EXTERNAL_OES -> 11`；`m_TextureRecord[11][256] -> [12][256]` |
| `renderdoc/driver/gl/gl_driver.cpp` | 析构 join 捕获线程；`StartFrameCapture` 清 `m_ExternalOESSnapshot`；`ProcessChunk` 新增 chunk case |
| `renderdoc/driver/gl/gl_initstate.cpp` | 初始内容路径恢复 w/h/depth；对无存储快照纹理按类型 `glTextureStorage*EXT` 分配 |
| `renderdoc/driver/gl/gl_manager.h` | `PrepareTextureInitialContents` 改为 public |
| `renderdoc/driver/gl/gl_program_iterate.cpp` | uniform 序列化/设置 case 增 `eGL_SAMPLER_EXTERNAL_OES` |
| `renderdoc/driver/gl/gl_rendertexture.cpp` / `gl_debug.cpp` / `gl_replay.cpp` / `gl_replay.h` / `gl_resources.cpp` | EXTERNAL_OES 映射为 TEX2D；`GetReplayEGLDisplay()`；`TextureBinding`/`TextureTarget` 支持 EXTERNAL_OES 绑定 |
| `renderdoc/driver/gl/CMakeLists.txt` | 新增 `gl_oes_capture.cpp` 到源列表 |
| `gen/build_android_linux.sh` | 新增 Android APK 一键构建脚本 |
| NDK 头 `<android/hardware_buffer.h>` / `<android/native_window.h>` | Vulkan 目录 `vk_android.cpp` 已引入，证明 Android 构建下 NDK 头可用 |
