# RenderDoc OES_EGL_image_external 捕获与回放机制

> 分类：RenderDoc / OpenGL 驱动 / 捕获与回放
> 适用范围：Android GLES（OES_EGL_image_external，`samplerExternalOES`）
> 版本：v1.x 分支

## 1. 背景与问题

在 Android 视频编辑 / 播放场景中，应用通常通过 `glEGLImageTargetTexture2DOES` 把
`GL_TEXTURE_EXTERNAL_OES`（由 `SurfaceTexture` / `EGLImage` 支持的外部纹理）绑定到当前
纹理单元，再用 `samplerExternalOES` 在着色器中采样。

RenderDoc 原本存在两个障碍：

1. **外部纹理无法被直接回读**。`GL_TEXTURE_EXTERNAL_OES` 既不能挂载到 FBO，也不能用
   `glGetTexImage` 直接读像素（GLES 限制），导致既有的 `PrepareTextureInitialContents`
   初始内容落盘机制对外部纹理完全失效。
2. **`samplerExternalOES` 与 `sampler2D` 不兼容**。回放环境里没有真正的外部源，若直接重发
   外部绑定，要么失败、要么采样到空纹理。

因此需要在捕获端把外部纹理“烤”成一张普通的 2D 纹理快照，并在回放端把着色器里的
`samplerExternalOES` 改写为 `sampler2D`，绑定该 2D 快照，从而消除外部依赖。

## 2. 总体方案

```
捕获期 (Capture)                                    回放期 (Replay)
─────────────────────────────────────────          ─────────────────────────────────────────
glEGLImageTargetTexture2DOES(EXTERNAL_OES)
   │  SERIALISE_TIME_CALL 转发真实调用
   │  查询 level-0 宽高/格式，补全 TextureData
   ▼
SnapshotExternalOESTexture()
   │  用 FBO + 全屏三角形 + 极简着色器
   │  把外部纹理采样渲染进一张新建的 2D 纹理
   │  登记 ResourceId，填充 TextureData
   ▼
m_ExternalOESSnapshot[externalId] = snapId
   │  MarkResourceFrameReferenced(snapId, eFrameRef_Read)
   ▼
帧末 PrepareTextureInitialContents 把 snapId 像素落盘 ───────┐
                                                              │
Serialise_glEGLImageTargetTexture2DOES                       │
   序列化: target / texId / snapshotId / activeUnit ─────────┘
                                                              ▼
                                         回放: 不再发外部绑定
                                         把 snapshotId 以 GL_TEXTURE_2D
                                         绑到 activeUnit 对应的纹理单元

Serialise_glShaderSource
   回放: 若源码含 samplerExternalOES
         全部替换为 sampler2D 再编译 ──► 采样 snapshotId 工作正常
```

核心思想：**捕获端把“动态外部源”固化为“静态 2D 快照”，回放端把 shader 语义从
`samplerExternalOES` 降级为 `sampler2D`**，两者配合后回放完全无外部依赖。

## 3. 关键数据结构

### `WrappedOpenGL` 新增成员（`gl_driver.h`）

```cpp
// external OES 纹理 ResourceId -> 2D 快照 ResourceId
std::map<ResourceId, ResourceId> m_ExternalOESSnapshot;
```

### `ShaderData` 新增标记（`gl_driver.h`）

```cpp
// 该着色器在回放时是否把 samplerExternalOES 改写为 sampler2D
bool externalOESRewritten = false;
```

### `WrappedOpenGL` 新增方法声明（`gl_driver.h`）

```cpp
ResourceId SnapshotExternalOESTexture(ResourceId externalId, GLuint externalName,
                                      uint32_t w, uint32_t h);
```

## 4. 捕获端实现

### 4.1 外部纹理 → 2D 快照（`SnapshotExternalOESTexture`）

位于 `gl_texture_funcs.cpp`，逻辑要点：

1. **去重**：`m_ExternalOESSnapshot` 中已存在则直接返回，避免同一外部纹理每帧重复创建。
2. **创建 2D 快照纹理**：`glGenTextures` + `glTexImage2D`（用未定大小格式 `eGL_RGBA` 以兼容
   GLES2；像素仍是 RGBA8）+ `LINEAR` 过滤 + `CLAMP_TO_EDGE`（外部 OES 本就强制这些参数）。
3. **FBO 拷贝**：新建 FBO，把快照纹理挂到 `COLOR_ATTACHMENT0`。
4. **极简拷贝着色器**：
   - VS：全屏三角形（3 顶点覆盖屏幕），输出 `aUV` varying。
   - FS：`#extension GL_OES_EGL_image_external : require` + `uniform samplerExternalOES uTex`
     + `gl_FragColor = texture2D(uTex, vUV)`。
5. **状态保存 / 恢复**：扰动前用 `glGetIntegerv` / `glIsEnabled` / `glGetVertexAttribiv`
   保存 `ACTIVE_TEXTURE`、`CURRENT_PROGRAM`、`ARRAY_BUFFER_BINDING`、
   `DRAW/READ_FRAMEBUFFER_BINDING`、`VIEWPORT`、`eGL_TEXTURE_BINDING_2D`、
   `eGL_TEXTURE_BINDING_EXTERNAL_OES`、顶点属性 0/1 使能、BLEND/DEPTH/CULL/SCISSOR 使能；
   拷贝完成后逐一恢复，避免污染应用状态。
6. **顶点数据**：用 12 个 float（每顶点：pos.xy + uv.xy，stride 16 字节）的全屏三角形，
   `aPos` 偏移 0，`aUV` 偏移 `2 * sizeof(float)`（2 个 float = 8 字节）。
7. **资源登记与元数据**：
   ```cpp
   ResourceId snapId = GetResourceManager()->RegisterResource(ResourceId(), TextureRes(GetCtx(), snap));
   GetResourceManager()->AddResourceRecord(snapId);
   m_ExternalOESSnapshot[externalId] = snapId;

   TextureData &sd = m_Textures[snapId];
   sd.curType        = eGL_TEXTURE_2D;
   sd.internalFormat = eGL_RGBA8;
   sd.width  = w;
   sd.height = h;
   sd.depth  = 1;
   sd.dimension = 2;
   ```
   记录 `TextureData` 后，既有的帧末 `PrepareTextureInitialContents` 会把 2D 快照像素自动落盘。

> 注意：`glActiveTexture` 的参数在 RenderDoc dispatch 表中是强类型 `RDCGLenum`，
> 因此恢复时须显式转换 `GL.glActiveTexture((RDCGLenum)prevActiveTex)`。

### 4.2 `glEGLImageTargetTexture2DOES` 捕获分支

原来只处理 `GL_TEXTURE_2D`；现扩展为同时处理 `GL_TEXTURE_2D` 与
`GL_TEXTURE_EXTERNAL_OES`：

- 对两者都查询 level-0 宽高/格式并补全 `TextureData`（应用常不预先 `glTexImage2D` 分配存储）。
- `GL_TEXTURE_EXTERNAL_OES`：调用 `SnapshotExternalOESTexture` 烤成 2D 快照，
  并 `MarkResourceFrameReferenced(snapId, eFrameRef_Read)`。
- `GL_TEXTURE_2D`（普通 OES_EGL_image）：直接 `MarkResourceFrameReferenced(texId, eFrameRef_Read)`，
  由通用初始内容路径回读像素。

## 5. 回放端实现

### 5.1 序列化内容（`Serialise_glEGLImageTargetTexture2DOES`）

除原 `target` / `texId` 外，新增：

- `snapshotId`：外部 OES 情形为 2D 快照的 ResourceId；2D 情形等于 `texId` 自身。
- `activeUnit`：应用当时把该纹理绑定的纹理单元
  （`GetCtxData().m_TextureUnit`）。

回放时**不再重发外部绑定**，而是：

```cpp
GLint prevActive = 0;
GL.glGetIntegerv(eGL_ACTIVE_TEXTURE, &prevActive);
GL.glActiveTexture((RDCGLenum)activeUnit);
GL.glBindTexture(eGL_TEXTURE_2D, snapRes.name);
GL.glActiveTexture((RDCGLenum)prevActive);
```

即把 2D 快照以 `GL_TEXTURE_2D` 绑到原纹理单元，后续 `sampler2D` 采样零外部依赖。

### 5.2 着色器改写（`Serialise_glShaderSource`，`gl_shader_funcs.cpp`）

回放分支中，若任一 source 含 `samplerExternalOES`，则把所有 `samplerExternalOES` token
替换为 `sampler2D`，改写后的源码存入 `m_Shaders[id].sources`（供显示与
GLSL→SPIR-V 反射路径看到合法 GLSL），并置 `externalOESRewritten = true`，再编译。

`rdcstr::find` 返回 `int32_t`（-1 表示未找到），替换循环写法：

```cpp
rdcstr token("samplerExternalOES"), repl("sampler2D");
int off = 0;
while (true) {
    int idx = rewritten.find(token, off);
    if (idx < 0) break;
    rewritten = rewritten.substr(0, idx) + repl + rewritten.substr(idx + (int)token.size());
    off = idx + (int)repl.size();
}
```

## 6. 已知限制

1. **静态帧快照**：外部 OES 快照是绑定时刻的一次性内容（沿用既有初始内容机制）。
   若同一外部纹理在帧内被视频源多次更新，仅捕获首次内容。要做逐绘制动态视频，
   需在每次该纹理参与绘制前重烤（可在 `Serialise_glDraw*` 路径中对引用的外部纹理触发
   `SnapshotExternalOESTexture`）。
2. **FBO 拷贝依赖采样参数**：极简着色器以 `LINEAR` + `CLAMP_TO_EDGE` 采样，外部 OES 本就
   强制这些参数，故一般无影响；若应用用到特殊纹理矩阵/变换，需相应调整拷贝着色器。
3. **仅 GLES 运行期触发**：`eGL_TEXTURE_BINDING_EXTERNAL_OES` 查询在桌面端不会执行；
   外部 OES 相关逻辑只在 Android/EGL 环境下生效。
4. **环境验证**：本文档对应的代码改动需在 Android GLES 构建下编译 + 真机/模拟器上捕获回放验证；
   当前环境（缺少 `common.h` 头与 Android NDK）仅做了静态类型/枚举/资源登记路径核查。

## 7. 涉及文件清单

| 文件 | 改动 |
| --- | --- |
| `renderdoc/driver/gl/gl_driver.h` | 新增 `m_ExternalOESSnapshot` 成员、`SnapshotExternalOESTexture` 声明、`ShaderData::externalOESRewritten` 标记 |
| `renderdoc/driver/gl/wrappers/gl_texture_funcs.cpp` | 新增 `SnapshotExternalOESTexture`；扩展 `glEGLImageTargetTexture2DOES` 捕获分支与序列化内容 |
| `renderdoc/driver/gl/wrappers/gl_shader_funcs.cpp` | `Serialise_glShaderSource` 回放分支增加 `samplerExternalOES → sampler2D` 改写 |
