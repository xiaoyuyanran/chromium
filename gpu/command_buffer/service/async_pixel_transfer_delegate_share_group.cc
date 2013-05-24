// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gpu/command_buffer/service/async_pixel_transfer_delegate_share_group.h"

#include "base/bind.h"
#include "base/debug/trace_event.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/synchronization/cancellation_flag.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "gpu/command_buffer/service/async_pixel_transfer_delegate.h"
#include "gpu/command_buffer/service/safe_shared_memory_pool.h"
#include "ui/gl/gl_bindings.h"
#include "ui/gl/gl_context.h"
#include "ui/gl/gl_surface.h"
#include "ui/gl/gpu_preference.h"
#include "ui/gl/scoped_binders.h"

namespace gpu {

namespace {

const char kAsyncTransferThreadName[] = "AsyncTransferThread";

// TODO(backer): Factor out common thread scheduling logic from the EGL and
// ShareGroup implementations. http://crbug.com/239889
class TransferThread : public base::Thread {
 public:
  TransferThread()
      : base::Thread(kAsyncTransferThreadName),
        initialized_(false) {
    Start();
#if defined(OS_ANDROID) || defined(OS_LINUX)
    SetPriority(base::kThreadPriority_Background);
#endif
  }

  virtual ~TransferThread() {
    // The only instance of this class was declared leaky.
    NOTREACHED();
  }

  void InitializeOnMainThread(gfx::GLContext* parent_context) {
    TRACE_EVENT0("gpu", "TransferThread::InitializeOnMainThread");
    if (initialized_)
      return;

    base::WaitableEvent wait_for_init(true, false);
    message_loop_proxy()->PostTask(
      FROM_HERE,
      base::Bind(&TransferThread::InitializeOnTransferThread,
                 base::Unretained(this),
                 base::Unretained(parent_context),
                 &wait_for_init));
    wait_for_init.Wait();
  }

  virtual void CleanUp() OVERRIDE {
    surface_ = NULL;
    context_ = NULL;
  }

  SafeSharedMemoryPool* safe_shared_memory_pool() {
    return &safe_shared_memory_pool_;
  }

 private:
  bool initialized_;

  scoped_refptr<gfx::GLSurface> surface_;
  scoped_refptr<gfx::GLContext> context_;
  SafeSharedMemoryPool safe_shared_memory_pool_;

  void InitializeOnTransferThread(gfx::GLContext* parent_context,
                                   base::WaitableEvent* caller_wait) {
    TRACE_EVENT0("gpu", "InitializeOnTransferThread");

    if (!parent_context) {
      LOG(ERROR) << "No parent context provided.";
      caller_wait->Signal();
      return;
    }

    surface_ = gfx::GLSurface::CreateOffscreenGLSurface(false, gfx::Size(1, 1));
    if (!surface_) {
      LOG(ERROR) << "Unable to create GLSurface";
      caller_wait->Signal();
      return;
    }

    // TODO(backer): This is coded for integrated GPUs. For discrete GPUs
    // we would probably want to use a PBO texture upload for a true async
    // upload (that would hopefully be optimized as a DMA transfer by the
    // driver).
    context_ = gfx::GLContext::CreateGLContext(parent_context->share_group(),
                                               surface_,
                                               gfx::PreferIntegratedGpu);
    if (!context_) {
      LOG(ERROR) << "Unable to create GLContext.";
      caller_wait->Signal();
      return;
    }

    context_->MakeCurrent(surface_);
    initialized_ = true;
    caller_wait->Signal();
  }

  DISALLOW_COPY_AND_ASSIGN(TransferThread);
};

base::LazyInstance<TransferThread>::Leaky
    g_transfer_thread = LAZY_INSTANCE_INITIALIZER;

base::MessageLoopProxy* transfer_message_loop_proxy() {
  return g_transfer_thread.Pointer()->message_loop_proxy();
}

SafeSharedMemoryPool* safe_shared_memory_pool() {
  return g_transfer_thread.Pointer()->safe_shared_memory_pool();
}

// Class which holds async pixel transfers state.
// The texture_id is accessed by either thread, but everything
// else accessed only on the main thread.
class TransferStateInternal
    : public base::RefCountedThreadSafe<TransferStateInternal> {
 public:
  TransferStateInternal(GLuint texture_id,
                        const AsyncTexImage2DParams& define_params)
      : texture_id_(texture_id),
        transfer_completion_(true, true) {
    define_params_ = define_params;
  }

  // Implement AsyncPixelTransferState:
  bool TransferIsInProgress() {
    return !transfer_completion_.IsSignaled();
  }

  void BindTransfer() {
    TRACE_EVENT2("gpu", "BindAsyncTransfer",
                 "width", define_params_.width,
                 "height", define_params_.height);
    DCHECK(texture_id_);

    glBindTexture(GL_TEXTURE_2D, texture_id_);
    bind_callback_.Run();
  }

  void MarkAsTransferIsInProgress() {
    transfer_completion_.Reset();
  }

  void MarkAsCompleted() {
    TRACE_EVENT0("gpu", "MarkAsCompleted");
    glFlush();
    transfer_completion_.Signal();
  }

  void WaitForTransferCompletion() {
    TRACE_EVENT0("gpu", "WaitForTransferCompletion");
    // TODO(backer): Deschedule the channel rather than blocking the main GPU
    // thread (crbug.com/240265).
    transfer_completion_.Wait();
  }

  GLuint texture_id() { return texture_id_; }

  void SetBindCallback(base::Closure bind_callback) {
    bind_callback_ = bind_callback;
  }

  void PerformAsyncTexImage2D(AsyncTexImage2DParams tex_params,
                              AsyncMemoryParams mem_params,
                              ScopedSafeSharedMemory* safe_shared_memory) {
    base::AutoLock locked(upload_lock_);
    if (cancel_upload_flag_.IsSet())
      return;

    TRACE_EVENT2("gpu",
                 "PerformAsyncTexImage",
                 "width",
                 tex_params.width,
                 "height",
                 tex_params.height);
    DCHECK_EQ(0, tex_params.level);

    void* data =
        AsyncPixelTransferDelegate::GetAddress(safe_shared_memory, mem_params);

    {
      TRACE_EVENT0("gpu", "glTexImage2D");
      glBindTexture(GL_TEXTURE_2D, texture_id_);
      glTexImage2D(GL_TEXTURE_2D,
                   tex_params.level,
                   tex_params.internal_format,
                   tex_params.width,
                   tex_params.height,
                   tex_params.border,
                   tex_params.format,
                   tex_params.type,
                   data);
      glBindTexture(GL_TEXTURE_2D, 0);
    }

    MarkAsCompleted();
  }

  void PerformAsyncTexSubImage2D(
      AsyncTexSubImage2DParams tex_params,
      AsyncMemoryParams mem_params,
      ScopedSafeSharedMemory* safe_shared_memory,
      scoped_refptr<AsyncPixelTransferUploadStats> texture_upload_stats) {
    base::AutoLock locked(upload_lock_);
    if (cancel_upload_flag_.IsSet())
      return;

    TRACE_EVENT2("gpu",
                 "PerformAsyncTexSubImage2D",
                 "width",
                 tex_params.width,
                 "height",
                 tex_params.height);
    DCHECK_EQ(0, tex_params.level);

    base::TimeTicks begin_time;
    if (texture_upload_stats)
      begin_time = base::TimeTicks::HighResNow();

    void* data =
        AsyncPixelTransferDelegate::GetAddress(safe_shared_memory, mem_params);

    {
      TRACE_EVENT0("gpu", "glTexSubImage2D");
      glBindTexture(GL_TEXTURE_2D, texture_id_);
      glTexSubImage2D(GL_TEXTURE_2D,
                      tex_params.level,
                      tex_params.xoffset,
                      tex_params.yoffset,
                      tex_params.width,
                      tex_params.height,
                      tex_params.format,
                      tex_params.type,
                      data);
      glBindTexture(GL_TEXTURE_2D, 0);
    }

    MarkAsCompleted();

    if (texture_upload_stats) {
      texture_upload_stats->AddUpload(base::TimeTicks::HighResNow() -
                                      begin_time);
    }
  }

  base::CancellationFlag* cancel_upload_flag() { return &cancel_upload_flag_; }
  base::Lock* upload_lock() { return &upload_lock_; }

 private:
  friend class base::RefCountedThreadSafe<TransferStateInternal>;

  virtual ~TransferStateInternal() {
  }

  // Used to cancel pending uploads.
  base::CancellationFlag cancel_upload_flag_;
  base::Lock upload_lock_;

  GLuint texture_id_;

  // Definition params for texture that needs binding.
  AsyncTexImage2DParams define_params_;

  // Indicates that an async transfer is in progress.
  base::WaitableEvent transfer_completion_;

  // Callback to invoke when AsyncTexImage2D is complete
  // and the client can safely use the texture. This occurs
  // during BindCompletedAsyncTransfers().
  base::Closure bind_callback_;
};

void PerformNotifyCompletion(
    AsyncMemoryParams mem_params,
    ScopedSafeSharedMemory* safe_shared_memory,
    const AsyncPixelTransferDelegate::CompletionCallback& callback) {
  TRACE_EVENT0("gpu", "PerformNotifyCompletion");
  AsyncMemoryParams safe_mem_params = mem_params;
  safe_mem_params.shared_memory = safe_shared_memory->shared_memory();
  callback.Run(safe_mem_params);
}

}  // namespace

// ShareGroup needs thread-safe ref-counting, so this just wraps
// an internal thread-safe ref-counted state object.
class AsyncTransferStateImpl : public AsyncPixelTransferState {
 public:
  AsyncTransferStateImpl(GLuint texture_id,
                         const AsyncTexImage2DParams& define_params)
      : internal_(new TransferStateInternal(texture_id, define_params)) {}

  virtual ~AsyncTransferStateImpl() {
    TRACE_EVENT0("gpu", " ~AsyncTransferStateImpl");
    base::AutoLock locked(*internal_->upload_lock());
    internal_->cancel_upload_flag()->Set();
  }

  virtual bool TransferIsInProgress() OVERRIDE {
      return internal_->TransferIsInProgress();
  }

  TransferStateInternal* internal() { return internal_.get(); }

 private:
  scoped_refptr<TransferStateInternal> internal_;
};

AsyncPixelTransferDelegateShareGroup::AsyncPixelTransferDelegateShareGroup(
      gfx::GLContext* context) {
  g_transfer_thread.Pointer()->InitializeOnMainThread(context);

  // TODO(reveman): Skip this if --enable-gpu-benchmarking is not present.
  texture_upload_stats_ = make_scoped_refptr(new AsyncPixelTransferUploadStats);
}

AsyncPixelTransferDelegateShareGroup::~AsyncPixelTransferDelegateShareGroup() {
}

AsyncPixelTransferState*
    AsyncPixelTransferDelegateShareGroup::
        CreatePixelTransferState(
            GLuint texture_id,
            const AsyncTexImage2DParams& define_params) {
  return static_cast<AsyncPixelTransferState*>(
      new AsyncTransferStateImpl(texture_id, define_params));
}

void AsyncPixelTransferDelegateShareGroup::BindCompletedAsyncTransfers() {
  scoped_ptr<gfx::ScopedTextureBinder> texture_binder;

  while (!pending_allocations_.empty()) {
    if (!pending_allocations_.front().get()) {
      pending_allocations_.pop_front();
      continue;
    }
    scoped_refptr<TransferStateInternal> state =
        static_cast<AsyncTransferStateImpl*>
            (pending_allocations_.front().get())->internal();
    // Terminate early, as all transfers finish in order, currently.
    if (state->TransferIsInProgress())
      break;

    if (!texture_binder)
      texture_binder.reset(new gfx::ScopedTextureBinder(GL_TEXTURE_2D, 0));

    // Used to set tex info from the gles2 cmd decoder once upload has
    // finished (it'll bind the texture and call a callback).
    state->BindTransfer();

    pending_allocations_.pop_front();
  }
}

void AsyncPixelTransferDelegateShareGroup::AsyncNotifyCompletion(
    const AsyncMemoryParams& mem_params,
    const CompletionCallback& callback) {
  DCHECK(mem_params.shared_memory);
  DCHECK_LE(mem_params.shm_data_offset + mem_params.shm_data_size,
            mem_params.shm_size);
  // Post a PerformNotifyCompletion task to the upload thread. This task
  // will run after all async transfers are complete.
  transfer_message_loop_proxy()->PostTask(
      FROM_HERE,
      base::Bind(&PerformNotifyCompletion,
                 mem_params,
                 base::Owned(
                     new ScopedSafeSharedMemory(safe_shared_memory_pool(),
                                                mem_params.shared_memory,
                                                mem_params.shm_size)),
                 callback));
}

void AsyncPixelTransferDelegateShareGroup::WaitForTransferCompletion(
      AsyncPixelTransferState* transfer_state) {
  scoped_refptr<TransferStateInternal> state =
      static_cast<AsyncTransferStateImpl*>(transfer_state)->internal();
  DCHECK(state);
  DCHECK(state->texture_id());

  if (state->TransferIsInProgress()) {
#if defined(OS_ANDROID) || defined(OS_LINUX)
    g_transfer_thread.Pointer()->SetPriority(base::kThreadPriority_Normal);
#endif

    state->WaitForTransferCompletion();
    DCHECK(!state->TransferIsInProgress());

#if defined(OS_ANDROID) || defined(OS_LINUX)
    g_transfer_thread.Pointer()->SetPriority(base::kThreadPriority_Background);
#endif
  }
}

void AsyncPixelTransferDelegateShareGroup::AsyncTexImage2D(
    AsyncPixelTransferState* transfer_state,
    const AsyncTexImage2DParams& tex_params,
    const AsyncMemoryParams& mem_params,
    const base::Closure& bind_callback) {
  scoped_refptr<TransferStateInternal> state =
      static_cast<AsyncTransferStateImpl*>(transfer_state)->internal();
  DCHECK(mem_params.shared_memory);
  DCHECK_LE(mem_params.shm_data_offset + mem_params.shm_data_size,
            mem_params.shm_size);
  DCHECK(state);
  DCHECK(state->texture_id());
  DCHECK(!state->TransferIsInProgress());
  DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D), tex_params.target);
  DCHECK_EQ(tex_params.level, 0);

  // Mark the transfer in progress and save the late bind
  // callback, so we can notify the client when it is bound.
  pending_allocations_.push_back(transfer_state->AsWeakPtr());
  state->SetBindCallback(bind_callback);

  // Mark the transfer in progress.
  state->MarkAsTransferIsInProgress();

  // Duplicate the shared memory so there is no way we can get
  // a use-after-free of the raw pixels.
  transfer_message_loop_proxy()->PostTask(
      FROM_HERE,
      base::Bind(
          &TransferStateInternal::PerformAsyncTexImage2D,
          state,
          tex_params,
          mem_params,
          base::Owned(new ScopedSafeSharedMemory(safe_shared_memory_pool(),
                                                 mem_params.shared_memory,
                                                 mem_params.shm_size))));
}

void AsyncPixelTransferDelegateShareGroup::AsyncTexSubImage2D(
    AsyncPixelTransferState* transfer_state,
    const AsyncTexSubImage2DParams& tex_params,
    const AsyncMemoryParams& mem_params) {
  TRACE_EVENT2("gpu", "AsyncTexSubImage2D",
               "width", tex_params.width,
               "height", tex_params.height);
  scoped_refptr<TransferStateInternal> state =
      static_cast<AsyncTransferStateImpl*>(transfer_state)->internal();

  DCHECK(state->texture_id());
  DCHECK(!state->TransferIsInProgress());
  DCHECK(mem_params.shared_memory);
  DCHECK_LE(mem_params.shm_data_offset + mem_params.shm_data_size,
            mem_params.shm_size);
  DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D), tex_params.target);
  DCHECK_EQ(tex_params.level, 0);

  // Mark the transfer in progress.
  state->MarkAsTransferIsInProgress();

  // Duplicate the shared memory so there are no way we can get
  // a use-after-free of the raw pixels.
  transfer_message_loop_proxy()->PostTask(
      FROM_HERE,
      base::Bind(
          &TransferStateInternal::PerformAsyncTexSubImage2D,
          state,
          tex_params,
          mem_params,
          base::Owned(new ScopedSafeSharedMemory(safe_shared_memory_pool(),
                                                 mem_params.shared_memory,
                                                 mem_params.shm_size)),
          texture_upload_stats_));
}

uint32 AsyncPixelTransferDelegateShareGroup::GetTextureUploadCount() {
  DCHECK(texture_upload_stats_);
  return texture_upload_stats_->GetStats(NULL);
}

base::TimeDelta
    AsyncPixelTransferDelegateShareGroup::GetTotalTextureUploadTime() {
  DCHECK(texture_upload_stats_);
  base::TimeDelta total_texture_upload_time;
  texture_upload_stats_->GetStats(&total_texture_upload_time);
  return total_texture_upload_time;
}

void AsyncPixelTransferDelegateShareGroup::ProcessMorePendingTransfers() {
}

bool AsyncPixelTransferDelegateShareGroup::NeedsProcessMorePendingTransfers() {
  return false;
}

}  // namespace gpu
