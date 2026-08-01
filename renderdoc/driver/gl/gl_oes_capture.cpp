/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * the above copyright notice and permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// OES external texture capture (Android only).
//
// GL_TEXTURE_EXTERNAL_OES is a storageless texture: the driver does not maintain
// TEXTURE_WIDTH/HEIGHT state, so glGetTexLevelParameteriv returns 0 and the texture cannot be
// read back via glGetTexImage or attached to an FBO. Its pixels live in an AHardwareBuffer.
//
// We resolve size/pixels by walking EGLImageKHR -> EGLClientBuffer -> AHardwareBuffer, populated
// through hooks on eglGetNativeClientBufferANDROID and eglCreateImageKHR. When the framework does
// not call eglGetNativeClientBufferANDROID (Java SurfaceTexture path) we fall back to a fixed
// offset between ANativeWindowBuffer and AHardwareBuffer.
//
// See gen/renderdoc/oes_external_texture_capture.md for the full design.

#include "gl_driver.h"
#include "gl_replay.h"
#include "common/common.h"
#include "stb/stb_image_write.h"
#include "strings/string_utils.h"
#include "egl_dispatch_table.h"
// NOTE: We deliberately do NOT pull the KHR/ANDROID EGL extension prototypes from the bundled
// eglext.h here. gl_common.h already includes it (without EGL_EGLEXT_PROTOTYPES) before this
// translation unit, so a second include is skipped by the header guard and the prototypes never
// appear. On top of that, the NDK 24 system headers mark eglCreateImageKHR /
// eglGetNativeClientBufferANDROID and the AHardwareBuffer_* helpers as __INTRODUCED_IN(26), which
// are reported unavailable when the NDK is built against a lower minSdk.
//
// Following the "use Android's EGL directly" guideline, we resolve these symbols at runtime from
// the system libEGL.so / libandroid.so via Process::LoadModule/GetFunctionAddress. The structs/types
// from <android/hardware_buffer.h> are still usable; only the function entry points are fetched
// dynamically.

#if ENABLED(RDOC_ANDROID)

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <chrono>

// ---------------------------------------------------------------------------
// Runtime resolution of Android AHardwareBuffer entry points.
// These are only guaranteed to exist on the Android 26+ devices we target for OES capture, so we
// fetch them lazily from the system libraries instead of linking against NDK 24's unavailable stubs.
// EGL entry points (eglGetCurrentDisplay, eglGetNativeClientBufferANDROID, eglCreateImage) are
// already available through the dispatch table (EGL.xxx), so they are NOT resolved here.
//
// Usage mirrors the EGL dispatch table pattern: OESAndroid.AHardwareBuffer_lock(...) etc.
// ---------------------------------------------------------------------------
namespace
{
typedef void (*PFN_AHardwareBuffer_describe)(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *outDesc);
typedef int (*PFN_AHardwareBuffer_lock)(AHardwareBuffer *buffer, uint64_t usage, int32_t fence,
                                        const ARect *rect, void **outVirtualAddress);
typedef int (*PFN_AHardwareBuffer_unlock)(AHardwareBuffer *buffer, int32_t *fence);
typedef int (*PFN_AHardwareBuffer_allocate)(const AHardwareBuffer_Desc *desc, AHardwareBuffer **outBuffer);
typedef void (*PFN_AHardwareBuffer_release)(AHardwareBuffer *buffer);

struct OESAndroidFuncs
{
  PFN_AHardwareBuffer_describe AHardwareBuffer_describe = NULL;
  PFN_AHardwareBuffer_lock AHardwareBuffer_lock = NULL;
  PFN_AHardwareBuffer_unlock AHardwareBuffer_unlock = NULL;
  PFN_AHardwareBuffer_allocate AHardwareBuffer_allocate = NULL;
  PFN_AHardwareBuffer_release AHardwareBuffer_release = NULL;
  bool resolved = false;
};
}    // namespace

static OESAndroidFuncs OESAndroid;

static void InitOESAndroidFuncs()
{
  if(OESAndroid.resolved)
    return;

  if(Process::IsModuleLoaded("libandroid.so"))
  {
    void *android = Process::LoadModule("libandroid.so");

    RDCLOG("InitOESAndroidFuncs: android=%p", android);

    OESAndroid.AHardwareBuffer_describe =
        (PFN_AHardwareBuffer_describe)Process::GetFunctionAddress(android, "AHardwareBuffer_describe");
    OESAndroid.AHardwareBuffer_lock =
        (PFN_AHardwareBuffer_lock)Process::GetFunctionAddress(android, "AHardwareBuffer_lock");
    OESAndroid.AHardwareBuffer_unlock =
        (PFN_AHardwareBuffer_unlock)Process::GetFunctionAddress(android, "AHardwareBuffer_unlock");
    OESAndroid.AHardwareBuffer_allocate =
        (PFN_AHardwareBuffer_allocate)Process::GetFunctionAddress(android, "AHardwareBuffer_allocate");
    OESAndroid.AHardwareBuffer_release =
        (PFN_AHardwareBuffer_release)Process::GetFunctionAddress(android, "AHardwareBuffer_release");
    RDCLOG("InitOESAndroidFuncs: AHardwareBuffer_allocate=%p AHardwareBuffer_release=%p",
           (void *)OESAndroid.AHardwareBuffer_allocate,
           (void *)OESAndroid.AHardwareBuffer_release);
  }
  else
  {
    RDCWARN("InitOESAndroidFuncs: libandroid not loaded, AHardwareBuffer entry points unavailable");
  }

  OESAndroid.resolved = true;
}

// ---------------------------------------------------------------------------
// Map population, called from the EGL hook layer.
// ---------------------------------------------------------------------------

void WrappedOpenGL::CaptureHook_eglGetNativeClientBufferANDROID(const void *buffer, EGLClientBuffer cb)
{
  RDCLOG("CaptureHook_eglGetNativeClientBufferANDROID: buffer=%p cb=%p", buffer, (void *)cb);
  if(buffer == NULL || cb == NULL)
    return;
  m_AHWToCB[buffer] = cb;
  m_CBToAHW[cb] = buffer;
}

void WrappedOpenGL::CaptureHook_eglCreateImage(EGLenum target, EGLClientBuffer buffer, EGLImageKHR image)
{
  RDCLOG("CaptureHook_eglCreateImage: target=0x%04x buffer=%p image=%p",
         target, (void *)buffer, (void *)image);
  if(target == EGL_NATIVE_BUFFER_ANDROID && image != EGL_NO_IMAGE_KHR && buffer != NULL)
    m_ImageToCB[image] = buffer;
}

void WrappedOpenGL::CaptureHook_eglDestroyImage(EGLImageKHR image)
{
  RDCLOG("CaptureHook_eglDestroyImage: image=%p", (void *)image);
  if(image != EGL_NO_IMAGE_KHR)
    m_ImageToCB.erase(image);
}

// ---------------------------------------------------------------------------
// Resolve the AHardwareBuffer backing an EGLImageKHR.
// ---------------------------------------------------------------------------

void *WrappedOpenGL::ResolveAHardwareBuffer(EGLImageKHR image)
{
  // Path 1: walk the populated maps (C++ path that calls eglGetNativeClientBufferANDROID).
  auto it = m_ImageToCB.find(image);
  if(it != m_ImageToCB.end())
  {
    EGLClientBuffer cb = it->second;

    auto it2 = m_CBToAHW.find(cb);
    if(it2 != m_CBToAHW.end())
    {
      // Calibrate g_ClientBufferToAHBOffset using the successfully resolved pair.
      // cb is an ANativeWindowBuffer* (via EGLClientBuffer). it2->second is the actual
      // AHardwareBuffer*. The difference is the offset we use for Path 2 (Java SurfaceTexture
      // path where eglGetNativeClientBufferANDROID was never called).
      int64_t offset = (int64_t)((char *)cb - (char *)it2->second);
      if(offset != g_ClientBufferToAHBOffset)
      {
        RDCLOG("ResolveAHardwareBuffer: updating g_ClientBufferToAHBOffset from 0x%llx to 0x%llx",
               (unsigned long long)g_ClientBufferToAHBOffset, (unsigned long long)offset);
        g_ClientBufferToAHBOffset = offset;
      }
      return (void *)it2->second;
    }

    // Path 2: m_ImageToCB has an entry, but m_CBToAHW does not (Java SurfaceTexture path:
    // eglGetNativeClientBufferANDROID was never called, so the CB->AHW map is empty).
    // The EGLClientBuffer is effectively an ANativeWindowBuffer*, which precedes the
    // AHardwareBuffer by the calibrated offset.
    RDCLOG("ResolveAHardwareBuffer: using offset fallback for CB=%p (no AHW in map, offset=0x%llx)",
           (void *)cb, (unsigned long long)g_ClientBufferToAHBOffset);
    return (void *)((char *)cb - g_ClientBufferToAHBOffset);
  }

  // Path 3: neither map has an entry (Java SurfaceTexture path where neither
  // eglCreateImageKHR nor eglGetNativeClientBufferANDROID were intercepted).
  // On Android EGL implementations, EGLImageKHR for EGL_NATIVE_BUFFER_ANDROID is typically
  // the ANativeWindowBuffer*, which precedes AHardwareBuffer by 0x10.
  RDCLOG("ResolveAHardwareBuffer: using 0x10 offset fallback for EGLImageKHR=%p",
         (void *)image);
  return (void *)((char *)image - g_ClientBufferToAHBOffset);
}

// ---------------------------------------------------------------------------
// Capture thread: sample non-RGBA_8888 EXTERNAL_OES textures into RGBA.
// ---------------------------------------------------------------------------

namespace
{
// Forward-declare: the worker thread samples in its own EGL context.
void OESCaptureThread(WrappedOpenGL *drv);

// Caller-side: record the parameters the worker needs and launch the thread.
// We must NOT call eglCreateContext / eglCreatePbufferSurface on the caller thread
// because those calls go through renderdoc hooks which may contend locks held by the caller.
void InitCaptureContext(WrappedOpenGL *drv)
{
  if(drv->m_CaptureThreadInit)
    return;

  std::lock_guard<std::mutex> lock(drv->m_CaptureMutex);
  if(drv->m_CaptureThreadInit)
    return;

  EGLDisplay appDisplay = EGL.GetCurrentDisplay();
  EGLContext appContext = EGL.GetCurrentContext();
  if(appDisplay == EGL_NO_DISPLAY || appContext == EGL_NO_CONTEXT)
  {
    RDCWARN("InitCaptureContext: no current EGL context; capture thread disabled");
    drv->m_CaptureThreadInit = true;
    return;
  }

  // Pick a pbuffer-compatible config on the caller side (eglChooseConfig is lightweight
  // and unlikely to deadlock). The actual context/surface creation happens on the worker.
  EGLConfig config = NULL;
  {
    EGLint numConfigs = 0;
    EGLint configAttribs[] = {EGL_SURFACE_TYPE,
                              EGL_PBUFFER_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES2_BIT,
                              EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              8,
                              EGL_NONE};
    if(!EGL.ChooseConfig(appDisplay, configAttribs, &config, 1, &numConfigs) || numConfigs == 0)
    {
      RDCWARN("InitCaptureContext: eglChooseConfig failed; capture thread disabled");
      drv->m_CaptureThreadInit = true;
      return;
    }
  }

  // Stash the init parameters for the worker; the worker will create the actual resources.
  drv->m_CaptureDisplay = appDisplay;
  drv->m_CaptureShareContext = appContext;
  drv->m_CaptureConfig = config;
  drv->m_CaptureThreadInit = true;
  drv->m_CaptureThreadExit = false;

  auto tBeforeThread = std::chrono::steady_clock::now();
  drv->m_CaptureThread = std::thread(OESCaptureThread, drv);
  auto tAfterThread = std::chrono::steady_clock::now();

  RDCLOG("InitCaptureContext: capture thread launched, display=%p shareCtx=%p, "
         "std::thread creation took %.1f ms",
         (void *)appDisplay, (void *)appContext,
         std::chrono::duration<double, std::milli>(tAfterThread - tBeforeThread).count());
}

// Vertex shader: fullscreen triangle (NDC coords).
static const char *kOESVertSrc = R"EOS(
#version 300 es
out vec2 v_texCoord;
void main()
{
  const vec2 pos[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
  gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
  v_texCoord = pos[gl_VertexID] * 0.5 + 0.5;
}
)EOS";

// Fragment shader: sample OES external texture.
static const char *kOESFragSrc = R"EOS(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES u_texOES;
in vec2 v_texCoord;
out vec4 fragColor;
void main()
{
  fragColor = texture(u_texOES, v_texCoord);
}
)EOS";

static GLuint CompileShader(GLenum type, const char *src)
{
  GLuint shader = GL.glCreateShader(type);
  GL.glShaderSource(shader, 1, &src, NULL);
  GL.glCompileShader(shader);
  GLint compiled = 0;
  GL.glGetShaderiv(shader, eGL_COMPILE_STATUS, &compiled);
  if(!compiled)
  {
    GLchar infoLog[512] = {};
    GL.glGetShaderInfoLog(shader, sizeof(infoLog), NULL, infoLog);
    RDCERR("SampleOESToRGBA: shader compile failed: %s", infoLog);
    GL.glDeleteShader(shader);
    return 0;
  }
  return shader;
}

static GLuint CreateOESSampleProgram()
{
  GLuint vert = CompileShader(eGL_VERTEX_SHADER, kOESVertSrc);
  GLuint frag = CompileShader(eGL_FRAGMENT_SHADER, kOESFragSrc);
  if(vert == 0 || frag == 0)
  {
    if(vert) GL.glDeleteShader(vert);
    if(frag) GL.glDeleteShader(frag);
    return 0;
  }
  GLuint prog = GL.glCreateProgram();
  GL.glAttachShader(prog, vert);
  GL.glAttachShader(prog, frag);
  GL.glLinkProgram(prog);
  GL.glDeleteShader(vert);
  GL.glDeleteShader(frag);
  GLint linked = 0;
  GL.glGetProgramiv(prog, eGL_LINK_STATUS, &linked);
  if(!linked)
  {
    GLchar infoLog[512] = {};
    GL.glGetProgramInfoLog(prog, sizeof(infoLog), NULL, infoLog);
    RDCERR("SampleOESToRGBA: program link failed: %s", infoLog);
    GL.glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

static GLuint GetOESSampleProgram()
{
  static GLuint prog = 0;
  if(prog == 0)
    prog = CreateOESSampleProgram();
  return prog;
}

// Actual GL sampling work -- must be called while the capture thread's own EGL context is current.
byte *SampleOESToRGBA(WrappedOpenGL *drv, EGLImageKHR image, uint32_t w, uint32_t h)
{
  GLuint tex = 0, fbo = 0, rbo = 0;
  GL.glGenTextures(1, &tex);
  GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, tex);
  drv->m_InOESSample = true;
  GL.glEGLImageTargetTexture2DOES(eGL_TEXTURE_EXTERNAL_OES, image);
  drv->m_InOESSample = false;

  // Create an RGBA8 render target for the FBO color attachment.
  GL.glGenRenderbuffers(1, &rbo);
  GL.glBindRenderbuffer(eGL_RENDERBUFFER, rbo);
  GL.glRenderbufferStorage(eGL_RENDERBUFFER, eGL_RGBA8, w, h);

  GL.glGenFramebuffers(1, &fbo);
  GL.glBindFramebuffer(eGL_FRAMEBUFFER, fbo);
  GL.glFramebufferRenderbuffer(eGL_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, eGL_RENDERBUFFER, rbo);

  // Draw a fullscreen triangle, sampling the OES texture into the FBO.
  GLuint prog = GetOESSampleProgram();
  if(prog != 0)
  {
    GLint prevViewport[4] = {};
    GL.glGetIntegerv(eGL_VIEWPORT, prevViewport);
    GL.glViewport(0, 0, w, h);
    GL.glUseProgram(prog);
    GL.glUniform1i(GL.glGetUniformLocation(prog, "u_texOES"), 0);
    GL.glDrawArrays(eGL_TRIANGLES, 0, 3);
    GL.glUseProgram(0);
    GL.glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
  }

  byte *buf = (byte *)malloc((size_t)w * h * 4);
  if(buf)
    GL.glReadPixels(0, 0, w, h, eGL_RGBA, eGL_UNSIGNED_BYTE, buf);

  GL.glBindFramebuffer(eGL_FRAMEBUFFER, 0);
  GL.glDeleteFramebuffers(1, &fbo);
  GL.glDeleteRenderbuffers(1, &rbo);
  GL.glDeleteTextures(1, &tex);
  return buf;
}

// Worker thread entry point. Creates its own EGL context (pbuffer) on this thread,
// then waits for OESSampleJob submissions, performs the GL sampling, and signals completion.
// Uses unhooked EGL functions (EGL.CreateContext etc.) to bypass renderdoc's hook layer,
// which would otherwise contend the glLock held by the caller thread.
void OESCaptureThread(WrappedOpenGL *drv)
{
  RDCLOG("OESCaptureThread: worker thread entry, about to create EGL context (unhooked)");

  // Create EGL context and pbuffer surface on THIS thread, not the caller's thread.
  // Use unhooked EGL dispatch table to avoid lock contention with the caller.
  EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  auto t0 = std::chrono::steady_clock::now();
  EGLContext ctx = EGL.CreateContext(drv->m_CaptureDisplay, drv->m_CaptureConfig,
                                     drv->m_CaptureShareContext, ctxAttribs);
  auto t1 = std::chrono::steady_clock::now();
  RDCLOG("OESCaptureThread: EGL.CreateContext (unhooked) took %.1f ms",
         std::chrono::duration<double, std::milli>(t1 - t0).count());
  if(ctx == EGL_NO_CONTEXT)
  {
    RDCERR("OESCaptureThread: EGL.CreateContext failed (err=0x%x), aborting", EGL.GetError());
    return;
  }

  EGLint pbAttribs[] = {EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE};
  auto t2 = std::chrono::steady_clock::now();
  EGLSurface surface = EGL.CreatePbufferSurface(drv->m_CaptureDisplay, drv->m_CaptureConfig,
                                                 pbAttribs);
  auto t3 = std::chrono::steady_clock::now();
  RDCLOG("OESCaptureThread: EGL.CreatePbufferSurface took %.1f ms",
         std::chrono::duration<double, std::milli>(t3 - t2).count());
  if(surface == EGL_NO_SURFACE)
  {
    RDCERR("OESCaptureThread: EGL.CreatePbufferSurface failed (err=0x%x), aborting",
           EGL.GetError());
    EGL.DestroyContext(drv->m_CaptureDisplay, ctx);
    return;
  }

  auto t4 = std::chrono::steady_clock::now();
  EGLBoolean makeCurrentOk = EGL.MakeCurrent(drv->m_CaptureDisplay, surface, surface, ctx);
  auto t5 = std::chrono::steady_clock::now();
  RDCLOG("OESCaptureThread: EGL.MakeCurrent took %.1f ms",
         std::chrono::duration<double, std::milli>(t5 - t4).count());
  if(!makeCurrentOk)
  {
    RDCERR("OESCaptureThread: EGL.MakeCurrent failed (err=0x%x), aborting", EGL.GetError());
    EGL.DestroySurface(drv->m_CaptureDisplay, surface);
    EGL.DestroyContext(drv->m_CaptureDisplay, ctx);
    return;
  }

  drv->m_CaptureContext = ctx;
  drv->m_CaptureSurface = surface;

  auto tTotal = std::chrono::duration<double, std::milli>(t5 - t0).count();
  RDCLOG("OESCaptureThread: fully initialized (unhooked), total init time %.1f ms, display=%p context=%p",
         tTotal, (void *)drv->m_CaptureDisplay, (void *)ctx);

  for(;;)
  {
    // Wait for a job to be submitted.
    {
      std::unique_lock<std::mutex> lock(drv->m_CaptureJobMutex);
      drv->m_CaptureJobSubmitCV.wait(lock, [drv] {
        return drv->m_CaptureJob.image != EGL_NO_IMAGE_KHR || drv->m_CaptureThreadExit;
      });
      if(drv->m_CaptureThreadExit)
        break;
    }

    WrappedOpenGL::OESSampleJob &job = drv->m_CaptureJob;

    byte *pixels =
        SampleOESToRGBA(drv, job.image, job.width, job.height);

    // Write result back and signal the caller.
    {
      std::lock_guard<std::mutex> lock(drv->m_CaptureJobMutex);
      job.pixels = pixels;
      job.success = (pixels != NULL);
      job.done = true;
    }
    drv->m_CaptureJobDoneCV.notify_one();
  }

  // Cleanup (also unhooked).
  EGL.MakeCurrent(drv->m_CaptureDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  EGL.DestroySurface(drv->m_CaptureDisplay, drv->m_CaptureSurface);
  EGL.DestroyContext(drv->m_CaptureDisplay, drv->m_CaptureContext);
  drv->m_CaptureSurface = EGL_NO_SURFACE;
  drv->m_CaptureContext = EGL_NO_CONTEXT;

  RDCLOG("OESCaptureThread: exited");
}
}    // namespace

// ---------------------------------------------------------------------------
// Pixel capture entry point: RGBA_8888 via lock, else offscreen sampling.
// ---------------------------------------------------------------------------

ResourceId WrappedOpenGL::CaptureExternalOESPixels(EGLImageKHR image, ResourceId externalId,
                                                   GLuint externalName)
{
  // Dedupe: same external texture already captured this frame.
  auto cached = m_ExternalOESSnapshot.find(externalId);
  if(cached != m_ExternalOESSnapshot.end())
    return cached->second;

  void *ahb = ResolveAHardwareBuffer(image);
  if(ahb == NULL)
  {
    RDCWARN("OES capture: could not resolve AHardwareBuffer; skipping external texture capture");
    return ResourceId();
  }

  InitOESAndroidFuncs();
  if(OESAndroid.AHardwareBuffer_describe == NULL || OESAndroid.AHardwareBuffer_lock == NULL ||
     OESAndroid.AHardwareBuffer_unlock == NULL)
  {
    RDCWARN("OES capture: AHardwareBuffer entry points unavailable; skipping external texture capture");
    return ResourceId();
  }

  AHardwareBuffer_Desc desc = {};
  OESAndroid.AHardwareBuffer_describe((AHardwareBuffer *)ahb, &desc);

  RDCLOG("OES capture (new): externalId=%s AHardwareBuffer=%p width=%u height=%u format=%u stride=%u layers=%u usage=0x%llx",
         ToStr(externalId).c_str(), ahb, desc.width, desc.height, desc.format, desc.stride, desc.layers,
         (unsigned long long)desc.usage);

  // Sanity-check the descriptor: a valid AHardwareBuffer must have non-zero width, height, and a
  // recognized format. Garbage values (width=12608 height=16 format=0) indicate the AHardwareBuffer
  // pointer was resolved incorrectly (e.g. offset fallback on a non-ANativeWindowBuffer image).
  if(desc.width == 0 || desc.height == 0 || desc.format == 0 ||
     desc.width > 16384 || desc.height > 16384)
  {
    RDCWARN("OES capture: AHardwareBuffer descriptor looks invalid (w=%u h=%u fmt=%u stride=%u), "
            "skipping capture. This likely means ResolveAHardwareBuffer returned a wrong pointer.",
            desc.width, desc.height, desc.format, desc.stride);
    return ResourceId();
  }

  const uint32_t w = desc.width;
  const uint32_t h = desc.height;

  byte *pixels = NULL;
  bool fromLock = false;

  if(desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
     desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM)
  {
    // Branch 1: RGBA_8888 - read directly via lock.
    void *addr = NULL;
    if(OESAndroid.AHardwareBuffer_lock((AHardwareBuffer *)ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL,
                               &addr) == 0 &&
       addr != NULL)
    {
      const uint32_t bpp = 4;
      const uint32_t srcStride = desc.stride * bpp;    // desc.stride is in pixels
      const uint32_t dstStride = w * bpp;
      pixels = (byte *)malloc((size_t)dstStride * h);
      if(pixels)
      {
        const byte *srcRow = (const byte *)addr;
        byte *dstRow = pixels;
        for(uint32_t y = 0; y < h; y++)
        {
          memcpy(dstRow, srcRow, dstStride);
          srcRow += srcStride;
          dstRow += dstStride;
        }
      }
      OESAndroid.AHardwareBuffer_unlock((AHardwareBuffer *)ahb, NULL);
      fromLock = true;
    }
  }

  if(pixels == NULL)
  {
    // Branch 2: other formats (YUV etc.) - submit job to the dedicated capture thread.
    InitCaptureContext(this);

    if(m_CaptureThread.joinable())
    {
      // Submit the job.
      {
        std::lock_guard<std::mutex> lock(m_CaptureJobMutex);
        m_CaptureJob.image = image;
        m_CaptureJob.width = w;
        m_CaptureJob.height = h;
        m_CaptureJob.pixels = NULL;
        m_CaptureJob.done = false;
        m_CaptureJob.success = false;
      }
      m_CaptureJobSubmitCV.notify_one();

      // Wait for the worker to complete (with timeout in case the worker aborted).
      {
        std::unique_lock<std::mutex> lock(m_CaptureJobMutex);
        if(!m_CaptureJobDoneCV.wait_for(lock, std::chrono::seconds(5),
                                        [this] { return m_CaptureJob.done; }))
        {
          RDCWARN("OES capture: worker timed out, falling back to SampleOESToRGBA");
          m_CaptureJob.image = EGL_NO_IMAGE_KHR;
          pixels = SampleOESToRGBA(this, image, w, h);
        }
        else
        {
          pixels = m_CaptureJob.pixels;
        }
      }

      // Clear the job image so the worker doesn't re-process.
      {
        std::lock_guard<std::mutex> lock(m_CaptureJobMutex);
        m_CaptureJob.image = EGL_NO_IMAGE_KHR;
      }
    }
    else
    {
      // Fallback: no capture thread available, sample on the calling context.
      pixels = SampleOESToRGBA(this, image, w, h);
    }
  }

  if(pixels == NULL)
  {
    RDCWARN("OES capture: failed to obtain pixels for external texture");
    return ResourceId();
  }

  // Dump the captured RGBA pixels as BMP to the app's files directory for debugging/analysis.
  {
    rdcstr dumpPath = FileIO::GetAppFolderFilename(
        StringFormat::Fmt("oes_dump_frame%u_%ux%u.bmp", m_FrameCounter, w, h));
    if(stbi_write_bmp(dumpPath.c_str(), (int)w, (int)h, 4, pixels) != 0)
      RDCLOG("OES capture: dumped BMP to %s", dumpPath.c_str());
    else
      RDCWARN("OES capture: failed to write BMP dump to %s", dumpPath.c_str());
  }

  // Create an RGBA8 2D snapshot texture, upload the captured pixels, and let the standard initial
  // contents path serialise them.
  GLuint snap = 0;
  GL.glGenTextures(1, &snap);
  GL.glBindTexture(eGL_TEXTURE_2D, snap);
  GL.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_MIN_FILTER, eGL_LINEAR);
  GL.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_MAG_FILTER, eGL_LINEAR);
  GL.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_WRAP_S, eGL_CLAMP_TO_EDGE);
  GL.glTexParameteri(eGL_TEXTURE_2D, eGL_TEXTURE_WRAP_T, eGL_CLAMP_TO_EDGE);
  GL.glTexImage2D(eGL_TEXTURE_2D, 0, eGL_RGBA8, w, h, 0, eGL_RGBA, eGL_UNSIGNED_BYTE, pixels);

  GLResource res = TextureRes(GetCtx(), snap);
  ResourceId snapId = GetResourceManager()->RegisterResource(ResourceId(), res);

  if(IsActiveCapturing(m_State))
  {
    // Serialise a glGenTextures chunk so the replay path can reconstruct the resource mapping
    // and create the backing GL texture object.
    // IMPORTANT: use GLChunk::glGenTextures explicitly, NOT gl_CurChunk. gl_CurChunk is set to
    // glEGLImageTargetTexture2DOES by the outer hook, and using it here would create a chunk with
    // the wrong type tag, causing the replay deserialiser to misinterpret the data.
    GLResourceRecord *record = GetResourceManager()->AddResourceRecord(snapId);
    RDCASSERT(record);
    record->datatype = TextureBinding(eGL_TEXTURE_2D);

    // Chunk 1: glGenTextures -- creates the GL texture object and registers the ResourceId.
    {
      Chunk *chunk = NULL;
      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(GLChunk::glGenTextures);
        Serialise_glGenTextures(ser, 1, &snap);
        chunk = scope.Get();
      }
      record->AddChunk(chunk);
    }

    // Chunk 2: glBindTexture -- binds the snapshot as TEXTURE_2D so that the replay path sets
    // curType correctly. Without this, Serialise_glGenTextures leaves curType == GL_NONE, which
    // causes "Unexpected target GL_NONE" errors during initial-contents serialisation.
    {
      Chunk *chunk = NULL;
      {
        USE_SCRATCH_SERIALISER();
        SCOPED_SERIALISE_CHUNK(GLChunk::glBindTexture);
        Serialise_glBindTexture(ser, eGL_TEXTURE_2D, snap);
        chunk = scope.Get();
      }
      record->AddChunk(chunk);
    }
  }
  else
  {
    GLResourceRecord *record = GetResourceManager()->AddResourceRecord(snapId);
    RDCASSERT(record);
    record->datatype = TextureBinding(eGL_TEXTURE_2D);
  }

  TextureData &sd = m_Textures[snapId];
  sd.resource = TextureRes(GetCtx(), snap);
  sd.curType = eGL_TEXTURE_2D;
  sd.internalFormat = eGL_RGBA8;
  sd.width = w;
  sd.height = h;
  sd.depth = 1;
  sd.dimension = 2;

  // Trigger the standard initial-contents serialisation (reads back the live texture).
  GetResourceManager()->PrepareTextureInitialContents(snapId, TextureRes(GetCtx(), snap));

  m_ExternalOESSnapshot[externalId] = snapId;

  free(pixels);
  return snapId;
}

// ---------------------------------------------------------------------------
// Replay: rebuild a real EXTERNAL_OES texture from the captured RGBA snapshot.
// ---------------------------------------------------------------------------

void WrappedOpenGL::ReplayExternalOES(ResourceId snapId, GLuint oesTexture)
{
  // The captured RGBA pixels are uploaded into a fresh AHardwareBuffer, which then backs a real
  // EGLImage + EXTERNAL_OES texture. The app's samplerExternalOES therefore samples the captured
  // frame exactly, with no shader rewriting.
  TextureData &sd = m_Textures[snapId];
  const uint32_t w = sd.width, h = sd.height;

  RDCLOG("ReplayExternalOES: snapId=%s oesTexture=%u w=%u h=%u curType=0x%04x internalFormat=0x%04x",
         ToStr(snapId).c_str(), oesTexture, w, h, sd.curType, sd.internalFormat);

  InitOESAndroidFuncs();

  AHardwareBuffer_Desc desc = {};
  desc.width = w;
  desc.height = h;
  desc.layers = 1;
  desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

  AHardwareBuffer *ahb = NULL;
  if(OESAndroid.AHardwareBuffer_allocate == NULL || OESAndroid.AHardwareBuffer_lock == NULL ||
     OESAndroid.AHardwareBuffer_unlock == NULL)
  {
    RDCERR("ReplayExternalOES: AHardwareBuffer entry points unavailable "
           "(allocate=%p lock=%p unlock=%p)",
           (void *)OESAndroid.AHardwareBuffer_allocate, (void *)OESAndroid.AHardwareBuffer_lock,
           (void *)OESAndroid.AHardwareBuffer_unlock);
    return;
  }
  if(w == 0 || h == 0)
  {
    RDCERR("ReplayExternalOES: invalid texture dimensions w=%u h=%u (snapId=%s, oesTexture=%u)",
           w, h, ToStr(snapId).c_str(), oesTexture);
    return;
  }
  int allocRet = OESAndroid.AHardwareBuffer_allocate(&desc, &ahb);
  if(allocRet != 0 || ahb == NULL)
  {
    RDCERR("ReplayExternalOES: AHardwareBuffer_allocate failed (ret=%d, ahb=%p, w=%u, h=%u)",
           allocRet, (void *)ahb, w, h);
    return;
  }

  // Ensure ahb is released on every exit path after a successful allocation.
  auto releaseAHB = [&]() {
    if(ahb && OESAndroid.AHardwareBuffer_release)
      OESAndroid.AHardwareBuffer_release(ahb);
  };

  // Re-read the descriptor after allocation: some drivers do not fill in desc.stride
  // during allocate, and we need the real stride for the CPU-side row copy below.
  if(OESAndroid.AHardwareBuffer_describe)
    OESAndroid.AHardwareBuffer_describe(ahb, &desc);

  RDCLOG("ReplayExternalOES: AHardwareBuffer allocated ahb=%p stride=%u", (void *)ahb, desc.stride);

  // The captured RGBA pixels already live in the snapId 2D texture (applied via the standard
  // initial-contents path during replay setup). Read them back and write into the AHardwareBuffer.
  rdcarray<byte> pixels;
  pixels.resize((size_t)w * h * 4);
  {
    GLuint snapName = m_Textures[snapId].resource.name;
    if(snapName == 0)
    {
      RDCERR("ReplayExternalOES: snapshot texture name is 0 for snapId=%s",
             ToStr(snapId).c_str());
      releaseAHB();
      return;
    }

    GLuint fbo = 0;
    GL.glGenFramebuffers(1, &fbo);
    GL.glBindFramebuffer(eGL_FRAMEBUFFER, fbo);
    GL.glFramebufferTexture2D(eGL_FRAMEBUFFER, eGL_COLOR_ATTACHMENT0, eGL_TEXTURE_2D,
                              snapName, 0);
    GLenum fboStatus = GL.glCheckFramebufferStatus(eGL_FRAMEBUFFER);
    GL.glReadPixels(0, 0, w, h, eGL_RGBA, eGL_UNSIGNED_BYTE, pixels.data());
    GL.glBindFramebuffer(eGL_FRAMEBUFFER, 0);
    GL.glDeleteFramebuffers(1, &fbo);

    RDCLOG("ReplayExternalOES: read back %u x %u from snapName=%u, FBO status=0x%04x",
           w, h, snapName, fboStatus);
  }

  void *addr = NULL;
  int lockRet = OESAndroid.AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &addr);
  if(lockRet == 0 && addr)
  {
    // desc.stride is in pixels; fall back to width if the driver left stride at 0.
    const uint32_t rowPixels = desc.stride > 0 ? desc.stride : w;
    const uint32_t dstStride = rowPixels * 4;
    const uint32_t srcStride = w * 4;
    byte *dstRow = (byte *)addr;
    const byte *srcRow = pixels.data();
    for(uint32_t y = 0; y < h; y++)
    {
      memcpy(dstRow, srcRow, srcStride);
      dstRow += dstStride;
      srcRow += srcStride;
    }
    OESAndroid.AHardwareBuffer_unlock(ahb, NULL);

    RDCLOG("ReplayExternalOES: copied %u x %u pixels to AHardwareBuffer (rowPixels=%u dstStride=%u)",
           w, h, rowPixels, dstStride);
  }
  else
  {
    RDCERR("ReplayExternalOES: AHardwareBuffer_lock failed (ret=%d, addr=%p)",
           lockRet, addr);
    releaseAHB();
    return;
  }

  // Log every EGL function pointer so the crash dump clearly shows which one is NULL.
  RDCLOG("ReplayExternalOES: EGL fn ptrs: GetCurrentDisplay=%p "
         "GetNativeClientBufferANDROID=%p CreateImage=%p GetError=%p",
         (void *)EGL.GetCurrentDisplay, (void *)EGL.GetNativeClientBufferANDROID,
         (void *)EGL.CreateImage, (void *)EGL.GetError);

  if(EGL.GetNativeClientBufferANDROID == NULL || EGL.CreateImage == NULL)
  {
    RDCERR("ReplayExternalOES: EGL entry points unavailable via dispatch table "
           "(GetNativeClientBufferANDROID=%p CreateImage=%p)",
           (void *)EGL.GetNativeClientBufferANDROID, (void *)EGL.CreateImage);
    releaseAHB();
    return;
  }

  EGLDisplay dpy = EGL.GetCurrentDisplay();
  if(dpy == EGL_NO_DISPLAY)
  {
    RDCERR("ReplayExternalOES: eglGetCurrentDisplay returned EGL_NO_DISPLAY");
    releaseAHB();
    return;
  }

  RDCLOG("ReplayExternalOES: calling EGL.GetNativeClientBufferANDROID(ahb=%p)...", (void *)ahb);
  EGLClientBuffer cb = EGL.GetNativeClientBufferANDROID((const AHardwareBuffer *)ahb);
  if(cb == NULL)
  {
    RDCERR("ReplayExternalOES: eglGetNativeClientBufferANDROID returned NULL (ahb=%p dpy=%p)",
           (void *)ahb, (void *)dpy);
    releaseAHB();
    return;
  }
  RDCLOG("ReplayExternalOES: cb=%p, calling EGL.CreateImage(dpy=%p "
         "target=eEGL_NATIVE_BUFFER_ANDROID cb=%p)...",
         (void *)cb, (void *)dpy, (void *)cb);

  // On some Qualcomm Adreno drivers eglCreateImageKHR with EGL_NATIVE_BUFFER_ANDROID can
  // crash internally if the AHardwareBuffer usage flags don't include CPU_READ_OFTEN. The
  // descriptor already requests this flag; additionally verify the allocated descriptor.
  if(OESAndroid.AHardwareBuffer_describe)
  {
    AHardwareBuffer_Desc verifyDesc = {};
    OESAndroid.AHardwareBuffer_describe(ahb, &verifyDesc);
    RDCLOG("ReplayExternalOES: AHB verify: w=%u h=%u fmt=%u usage=0x%" PRIx64 " stride=%u",
           verifyDesc.width, verifyDesc.height, verifyDesc.format, verifyDesc.usage, verifyDesc.stride);
  }

  RDCLOG("ReplayExternalOES: about to call EGL.CreateImage (fn=%p)...", (void *)EGL.CreateImage);
  EGLAttrib imgAttribs[] = { EGL_NONE };
  EGLImageKHR img = EGL.CreateImage(dpy, EGL_NO_CONTEXT,
                                    eEGL_NATIVE_BUFFER_ANDROID, cb, imgAttribs);
  RDCLOG("ReplayExternalOES: EGL.CreateImage returned img=%p", (void *)img);
  if(img != EGL_NO_IMAGE_KHR)
  {
    if(GL.glEGLImageTargetTexture2DOES == NULL)
    {
      RDCERR("ReplayExternalOES: GL.glEGLImageTargetTexture2DOES is NULL, cannot bind OES texture");
      releaseAHB();
      return;
    }

    GL.glBindTexture(eGL_TEXTURE_EXTERNAL_OES, oesTexture);
    GL.glEGLImageTargetTexture2DOES(eGL_TEXTURE_EXTERNAL_OES, img);

    GLenum glErr = GL.glGetError();
    RDCLOG("ReplayExternalOES: EGLImage created img=%p, bound to OES texture %u, GL error=0x%04x",
           (void *)img, oesTexture, glErr);
  }
  else
  {
    EGLint eglErr = EGL.GetError ? EGL.GetError() : 0;
    RDCERR("ReplayExternalOES: eglCreateImage failed (EGL error=0x%04x, dpy=%p, cb=%p "
           "CreateImage=%p ahb=%p w=%u h=%u)",
           eglErr, (void *)dpy, (void *)cb, (void *)EGL.CreateImage, (void *)ahb, w, h);
  }

  releaseAHB();
}

#endif    // RDOC_ANDROID
