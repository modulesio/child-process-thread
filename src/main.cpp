#include <v8.h>
#include <nan.h>
#include <node.h>
#include <uv.h>

#include <memory>
#include <map>
#include <thread>

#if _WIN32
#include <Windows.h>
#include <io.h>
#include "../deps/pthread-win32/config.h"
#include "../deps/pthread-win32/pthread.h"
#else
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <pthread.h>
#endif

using namespace v8;
using namespace node;
using namespace std;

#define JS_FN(x) (Nan::GetFunction(x).ToLocalChecked())
#define JS_STR(...) Nan::New<v8::String>(__VA_ARGS__).ToLocalChecked()
#define JS_INT(val) Nan::New<v8::Integer>(val)
#define JS_NUM(val) Nan::New<v8::Number>(val)
#define JS_FLOAT(val) Nan::New<v8::Number>(val)
#define JS_BOOL(val) Nan::New<v8::Boolean>(val)

#define TO_NUM(x) (Nan::To<double>(x).FromJust())
#define TO_BOOL(x) (Nan::To<bool>(x).FromJust())
#define TO_UINT32(x) (Nan::To<unsigned int>(x).FromJust())
#define TO_INT32(x) (Nan::To<int>(x).FromJust())
#define TO_FLOAT(x) (Nan::To<float>(x).FromJust())

#define STDIO_BUF_SIZE 4096

namespace childProcessThread {

uv_key_t threadKey;

NAN_METHOD(nop) {}
void walkHandlePruneFn(uv_handle_t *handle, void *arg) {
  if (uv_is_active(handle)) {
    if (handle->type == 16) { // UV_SIGNAL
      uv_close(handle, nullptr);
    }
  }
}

class QueueEntry {
public:
  QueueEntry(uintptr_t address, size_t size) : address(address), size(size) {}
  QueueEntry(const QueueEntry &queueEntry) : address(queueEntry.address), size(queueEntry.size) {}

  uintptr_t address;
  size_t size;
};

class Thread : public Nan::ObjectWrap {
public:
  static Local<Object> Initialize();
  const string &getJsPath() const;
  pthread_t &getThread();
  uv_loop_t &getLoop();
  unique_ptr<uv_async_t> &getExitAsync();
  unique_ptr<uv_async_t> &getMessageAsyncIn();
  unique_ptr<uv_async_t> &getMessageAsyncOut();
  uv_mutex_t &getMutex();
  void pushMessageIn(const QueueEntry &queueEntry);
  void pushMessageOut(const QueueEntry &queueEntry);
  queue<QueueEntry> getMessageQueueIn();
  queue<QueueEntry> getMessageQueueOut();

  void setThreadGlobal(Local<Object> global);
  Local<Object> getThreadGlobal();
  void removeThreadGlobal();
  void setThreadObject(Local<Object> threadObj);
  Local<Object> getThreadObject();
  void removeThreadObject();
  bool getLive() const;
  void setLive(bool live);

  static const string &getChildJsPath();
  static void setChildJsPath(const string &childJsPath);
  static const vector<pair<string, uintptr_t>> &getNativeRequires();
  static Thread *getThreadByKey(uintptr_t key);
  static void setThreadByKey(uintptr_t key, Thread *thread);
  static void removeThreadByKey(uintptr_t key);
  static Thread *getCurrentThread();
  static void setCurrentThread(Thread *thread);
  static uv_loop_t *getCurrentEventLoop();

  Thread(const string &jsPath);
  ~Thread();
  static NAN_METHOD(New);
  static NAN_METHOD(Fork);
  static NAN_METHOD(SetChildJsPath);
  static NAN_METHOD(SetNativeRequire);
  static NAN_METHOD(Terminate);
  static NAN_METHOD(Cancel);
  static NAN_METHOD(RequireNative);
  static NAN_METHOD(PostThreadMessageIn);
  static NAN_METHOD(PostThreadMessageOut);
  static NAN_METHOD(Tick);

private:
  static string childJsPath;
  static map<uintptr_t, Thread*> threadMap;
  static vector<pair<string, uintptr_t>> nativeRequires;

  string jsPath;
  uv_loop_t loop;
  uv_mutex_t mutex;
  unique_ptr<uv_async_t> exitAsync;
  unique_ptr<uv_async_t> messageAsyncIn;
  unique_ptr<uv_async_t> messageAsyncOut;
  pthread_t thread;
  Persistent<Object> global;
  Persistent<Object> threadObj;
  queue<QueueEntry> messageQueueIn;
  queue<QueueEntry> messageQueueOut;
  bool live;
};

bool ShouldAbortOnUncaughtException(Isolate *isolate) {
  return false;
}
/* void OnMessage(Local<Message> message, Local<Value> error) {
  Nan::HandleScope handleScope;

  Thread *thread = Thread::getCurrentThread();
  Local<Object> global = thread->getThreadGlobal();
  Local<Object> processObj = Local<Object>::Cast(global->Get(JS_STR("process")));
  Local<Function> fatalExceptionFn = Local<Function>::Cast(processObj->Get(JS_STR("_fatalException")));
  fatalExceptionFn->Call(processObj, 1, &error);
} */

inline int Start(
  Thread *thread, Isolate *isolate, IsolateData *isolate_data,
  int argc, const char* const* argv,
  int exec_argc, const char* const* exec_argv
) {
  HandleScope handle_scope(isolate);
  Local<Context> context = Context::New(isolate);
  // Local<Context> context = NewContext(isolate);
  Context::Scope context_scope(context);

  {
    Local<Object> global = context->Global();
    global->Set(JS_STR("requireNative"), Nan::New<Function>(Thread::RequireNative));
    global->Set(JS_STR("onthreadmessage"), Nan::Null());
    global->Set(JS_STR("postThreadMessage"), Nan::New<Function>(Thread::PostThreadMessageOut));

    thread->setThreadGlobal(global);
  }

  {
#if _WIN32
    HMODULE handle = GetModuleHandle(nullptr);
    FARPROC address = GetProcAddress(handle, "?FLAG_allow_natives_syntax@internal@v8@@3_NA");
#else
    void *handle = dlopen(NULL, RTLD_LAZY);
    void *address = dlsym(handle, "_ZN2v88internal25FLAG_allow_natives_syntaxE");
#endif
    bool *flag = (bool *)address;
    *flag = true;
  }

  Environment *env = CreateEnvironment(isolate_data, context, argc, argv, exec_argc, exec_argv);

  // uv_key_t thread_local_env;
  // uv_key_create(&thread_local_env);
  // CHECK_EQ(0, uv_key_create(&thread_local_env));
  // uv_key_set(&thread_local_env, env);
  // env->Start(argc, argv, exec_argc, exec_argv, v8_is_profiling);

  LoadEnvironment(env);

  uv_walk(&thread->getLoop(), walkHandlePruneFn, nullptr);

  {
    Local<Object> asyncObj = Nan::New<Object>();
    AsyncResource asyncResource(Isolate::GetCurrent(), asyncObj, "asyncResource");
    Local<Function> asyncFunction = Nan::New<Function>(nop);

    /* const char* path = argc > 1 ? argv[1] : nullptr;
    StartInspector(&env, path, debug_options);

    if (debug_options.inspector_enabled() && !v8_platform.InspectorStarted(&env))
      return 12;  // Signal internal error.

    env->set_abort_on_uncaught_exception(abort_on_uncaught_exception);

    if (no_force_async_hooks_checks) {
      env->async_hooks()->no_force_checks();
    }

    {
      Environment::AsyncCallbackScope callback_scope(env);
      env->async_hooks()->push_async_ids(1, 0);
      LoadEnvironment(env);
      env->async_hooks()->pop_async_id(1);
    }

    env->set_trace_sync_io(trace_sync_io); */

    {
      SealHandleScope seal(isolate);
      bool more;
      // PERFORMANCE_MARK(&env, LOOP_START)

      thread->setLive(true);

      do {
        uv_run(&thread->getLoop(), UV_RUN_ONCE);

        // v8_platform.DrainVMTasks(isolate);

        more = uv_loop_alive(&thread->getLoop()) && thread->getLive();
        if (more) {
          HandleScope handle_scope(isolate);
          asyncResource.MakeCallback(asyncFunction, 0, nullptr);
        }

        // EmitBeforeExit(env);

        // Emit `beforeExit` if the loop became alive either after emitting
        // event, or after running some callbacks.
        // more = uv_loop_alive(&thread->getLoop());
      } while (more == true);
      // PERFORMANCE_MARK(&env, LOOP_EXIT);
    }
  }

  thread->removeThreadGlobal();

  // env.set_trace_sync_io(false);

  /* const int exit_code = EmitExit(&env);
  RunAtExit(&env); */
  // uv_key_delete(&thread_local_env);

  // FreeEnvironment(env); // XXX crashes on terminate; should just use native Worker

  /* v8_platform.DrainVMTasks(isolate);
  v8_platform.CancelVMTasks(isolate);
  WaitForInspectorDisconnect(&env);
#if defined(LEAK_SANITIZER)
  __lsan_do_leak_check();
#endif */

  return 0;
}

inline int Start(Thread *thread,
  int argc, const char* const* argv,
  int exec_argc, const char* const* exec_argv
) {
  Isolate::CreateParams params;
  unique_ptr<ArrayBuffer::Allocator> allocator(ArrayBuffer::Allocator::NewDefaultAllocator());
  params.array_buffer_allocator = allocator.get();
/* #ifdef NODE_ENABLE_VTUNE_PROFILING
  params.code_event_handler = vTune::GetVtuneCodeEventHandler();
#endif */

  Isolate* const isolate = Isolate::New(params);
  if (isolate == nullptr)
    return 12;  // Signal internal error.

  // isolate->AddMessageListener(OnMessage);
  isolate->SetAbortOnUncaughtExceptionCallback(ShouldAbortOnUncaughtException);
  // isolate->SetAutorunMicrotasks(false);
  // isolate->SetFatalErrorHandler(OnFatalError);

  /* {
    Mutex::ScopedLock scoped_lock(node_isolate_mutex);
    CHECK_EQ(node_isolate, nullptr);
    node_isolate = isolate;
  } */

  int exit_code;
  {
    Locker locker(isolate);
    Isolate::Scope isolate_scope(isolate);
    HandleScope handle_scope(isolate);

#if _WIN32
    HMODULE handle = GetModuleHandle(nullptr);
    FARPROC address = GetProcAddress(handle, "?GetCurrentPlatform@V8@internal@v8@@SAPEAVPlatform@3@XZ");
#else
    void *handle = dlopen(NULL, RTLD_LAZY);
    void *address = dlsym(handle, "_ZN2v88internal2V818GetCurrentPlatformEv");
#endif
    Platform *(*GetCurrentPlatform)(void) = (Platform *(*)(void))address;
    MultiIsolatePlatform *platform = (MultiIsolatePlatform *)GetCurrentPlatform();
    IsolateData *isolate_data = CreateIsolateData(isolate, &thread->getLoop(), platform);

    /* if (track_heap_objects) {
      isolate->GetHeapProfiler()->StartTrackingHeapObjects(true);
    } */
    exit_code = Start(thread, isolate, isolate_data, argc, argv, exec_argc, exec_argv);

    FreeIsolateData(isolate_data);
    // FreePlatform(platform);
  }

  /* {
    Mutex::ScopedLock scoped_lock(node_isolate_mutex);
    CHECK_EQ(node_isolate, isolate);
    node_isolate = nullptr;
  } */

  isolate->Dispose();

  return exit_code;
}

static void *threadFn(void *arg) {
#if !defined(ANDROID) && !defined(LUMIN)
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);
#endif

  Thread *thread = (Thread *)arg;
  Thread::setCurrentThread(thread);

  char argsString[4096];
  int i = 0;

  char *binPathArg = argsString + i;
  const char *binPathString = "node";
  strncpy(binPathArg, binPathString, sizeof(argsString) - i);
  i += strlen(binPathString) + 1;

  char *childJsPathArg = argsString + i;
  strncpy(childJsPathArg, Thread::getChildJsPath().c_str(), sizeof(argsString) - i);
  i += Thread::getChildJsPath().length() + 1;

  char *jsPathArg = argsString + i;
  strncpy(jsPathArg, thread->getJsPath().c_str(), sizeof(argsString) - i);
  i += thread->getJsPath().length() + 1;

  char *allowNativesSynax = argsString + i;
  strncpy(allowNativesSynax, "--allow_natives_syntax", sizeof(argsString) - i);
  i += strlen(allowNativesSynax) + 1;

  char *argv[] = {binPathArg, childJsPathArg, jsPathArg, allowNativesSynax};
  int argc = sizeof(argv)/sizeof(argv[0]);
  int retval = Start(thread, argc, argv, argc, argv);

  return new int(retval);
}
void exitAsyncCb(uv_async_t *handle) {
  Thread *thread = Thread::getCurrentThread();
  thread->setLive(false);
}
void messageAsyncInCb(uv_async_t *handle) {
  Nan::HandleScope handleScope;

  Thread *thread = Thread::getCurrentThread();

  queue<QueueEntry> messageQueue(thread->getMessageQueueIn());

  Local<Object> global = thread->getThreadGlobal();
  Local<Value> onthreadmessageValue = global->Get(JS_STR("onthreadmessage"));
  if (onthreadmessageValue->IsFunction()) {
    Local<Function> onthreadmessageFn = Local<Function>::Cast(onthreadmessageValue);

    size_t numMessages = messageQueue.size();
    for (size_t i = 0; i < numMessages; i++) {
      const QueueEntry &queueEntry = messageQueue.front();
      messageQueue.pop();

      char *data = (char *)queueEntry.address;
      size_t size = queueEntry.size;
      Local<ArrayBuffer> message = ArrayBuffer::New(Isolate::GetCurrent(), data, size);

      Local<Value> argv[] = {message};
      onthreadmessageFn->Call(v8::Isolate::GetCurrent()->GetCurrentContext(), global, sizeof(argv)/sizeof(argv[0]), argv);
    }
  }
}
void messageAsyncOutCb(uv_async_t *handle) {
  Nan::HandleScope handleScope;

  Thread *thread = Thread::getThreadByKey((uintptr_t)handle);

  queue<QueueEntry> messageQueue(thread->getMessageQueueOut());

  Local<Object> threadObj = thread->getThreadObject();
  Local<Value> onthreadmessageValue = threadObj->Get(JS_STR("onthreadmessage"));

  if (onthreadmessageValue->IsFunction()) {
    Local<Function> onthreadmessageFn = Local<Function>::Cast(onthreadmessageValue);

    size_t numMessages = messageQueue.size();
    for (size_t i = 0; i < numMessages; i++) {
      const QueueEntry &queueEntry = messageQueue.front();
      messageQueue.pop();

      char *data = (char *)queueEntry.address;
      size_t size = queueEntry.size;
      Local<ArrayBuffer> message = ArrayBuffer::New(Isolate::GetCurrent(), data, size);

      Local<Value> argv[] = {message};
      onthreadmessageFn->Call(v8::Isolate::GetCurrent()->GetCurrentContext(), threadObj, sizeof(argv)/sizeof(argv[0]), argv);
    }
  }
}
void closeHandleFn(uv_handle_t *handle) {
  delete handle;
}
void walkHandleCleanupFn(uv_handle_t *handle, void *arg) {
  if (uv_is_active(handle)) {
    uv_close(handle, closeHandleFn);
  }
}

Local<Object> Thread::Initialize() {
  Nan::EscapableHandleScope scope;

  // constructor
  Local<FunctionTemplate> ctor = Nan::New<FunctionTemplate>(New);
  ctor->InstanceTemplate()->SetInternalFieldCount(1);
  ctor->SetClassName(JS_STR("Thread"));
  Nan::SetPrototypeMethod(ctor, "terminate", Thread::Terminate);
  Nan::SetPrototypeMethod(ctor, "cancel", Thread::Cancel);
  Nan::SetPrototypeMethod(ctor, "postThreadMessage", Thread::PostThreadMessageIn);

  Local<Function> ctorFn = JS_FN(ctor);

  Local<Function> forkFn = Nan::New<Function>(Thread::Fork);
  ctorFn->Set(JS_STR("fork"), forkFn);
  ctorFn->Set(JS_STR("setChildJsPath"), Nan::New<Function>(Thread::SetChildJsPath));
  ctorFn->Set(JS_STR("setNativeRequire"), Nan::New<Function>(Thread::SetNativeRequire));

  return scope.Escape(ctorFn);
}
const string &Thread::getJsPath() const {
  return jsPath;
}
pthread_t &Thread::getThread() {
  return thread;
}
uv_loop_t &Thread::getLoop() {
  return loop;
}
unique_ptr<uv_async_t> &Thread::getExitAsync() {
  return exitAsync;
}
unique_ptr<uv_async_t> &Thread::getMessageAsyncIn() {
  return messageAsyncIn;
}
unique_ptr<uv_async_t> &Thread::getMessageAsyncOut() {
  return messageAsyncOut;
}
uv_mutex_t &Thread::getMutex() {
  return mutex;
}
void Thread::pushMessageIn(const QueueEntry &queueEntry) {
  uv_mutex_lock(&mutex);

  messageQueueIn.push(queueEntry);

  uv_async_send(messageAsyncIn.get());

  uv_mutex_unlock(&mutex);
}
void Thread::pushMessageOut(const QueueEntry &queueEntry) {
  uv_mutex_lock(&mutex);

  messageQueueOut.push(queueEntry);

  uv_async_send(messageAsyncOut.get());

  uv_mutex_unlock(&mutex);
}
queue<QueueEntry> Thread::getMessageQueueIn() {
  uv_mutex_lock(&mutex);

  queue<QueueEntry> result;
  messageQueueIn.swap(result);

  uv_mutex_unlock(&mutex);

  return result;
}
queue<QueueEntry> Thread::getMessageQueueOut() {
  uv_mutex_lock(&mutex);

  queue<QueueEntry> result;
  messageQueueOut.swap(result);

  uv_mutex_unlock(&mutex);

  return result;
}
void Thread::setThreadGlobal(Local<Object> global) {
  this->global.Reset(Isolate::GetCurrent(), global);
}
Local<Object> Thread::getThreadGlobal() {
  return Nan::New(global);
}
void Thread::removeThreadGlobal() {
  global.Reset();
}
bool Thread::getLive() const {
  return live;
}
void Thread::setThreadObject(Local<Object> threadObj) {
  this->threadObj.Reset(Isolate::GetCurrent(), threadObj);
}
Local<Object> Thread::getThreadObject() {
  return Nan::New(threadObj);
}
void Thread::removeThreadObject() {
  threadObj.Reset();
}
void Thread::setLive(bool live) {
  this->live = live;
}

const string &Thread::getChildJsPath() {
  return Thread::childJsPath;
}
void Thread::setChildJsPath(const string &childJsPath) {
  Thread::childJsPath = childJsPath;
}
const vector<pair<string, uintptr_t>> &Thread::getNativeRequires() {
  return nativeRequires;
}
Thread *Thread::getThreadByKey(uintptr_t key) {
  return Thread::threadMap.at(key);
}
void Thread::setThreadByKey(uintptr_t key, Thread *thread) {
  Thread::threadMap[key] = thread;
}
void Thread::removeThreadByKey(uintptr_t key) {
  Thread::threadMap.erase(key);
}
Thread *Thread::getCurrentThread() {
  return (Thread *)uv_key_get(&threadKey);
}
void Thread::setCurrentThread(Thread *thread) {
  uv_key_set(&threadKey, thread);
}
uv_loop_t *Thread::getCurrentEventLoop() {
  Thread *thread = Thread::getCurrentThread();

  if (thread != nullptr) {
    return &thread->loop;
  } else {
    return uv_default_loop();
  }
}

Thread::Thread(const string &jsPath) : jsPath(jsPath) {
  uv_loop_init(&loop);
  uv_mutex_init(&mutex);

  exitAsync = unique_ptr<uv_async_t>(new uv_async_t());
  uv_async_init(&loop, exitAsync.get(), exitAsyncCb);
  messageAsyncIn = unique_ptr<uv_async_t>(new uv_async_t());
  uv_async_init(&loop, messageAsyncIn.get(), messageAsyncInCb);
  messageAsyncOut = unique_ptr<uv_async_t>(new uv_async_t());
  uv_async_init(Thread::getCurrentEventLoop(), messageAsyncOut.get(), messageAsyncOutCb);

  live = true;

  Thread::setThreadByKey((uintptr_t)messageAsyncOut.get(), this);

  pthread_create(&thread, nullptr, threadFn, this);
}
Thread::~Thread() {}
NAN_METHOD(Thread::New) {
  Nan::HandleScope scope;

  if (info[0]->IsString()) {
    Local<Object> rawThreadObj = info.This();

    Local<String> jsPathValue = Local<String>::Cast(info[0]);
    Nan::Utf8String jsPathValueUtf8(jsPathValue);
    size_t length = jsPathValueUtf8.length();
    string jsPath(*jsPathValueUtf8, length);

    Thread *thread = new Thread(jsPath);
    thread->Wrap(rawThreadObj);
    thread->setThreadObject(rawThreadObj);

    info.GetReturnValue().Set(rawThreadObj);
  } else {
    return Nan::ThrowError("Invalid arguments");
  }
}
NAN_METHOD(Thread::Fork) {
  if (info[0]->IsString()) {
    Local<Function> threadConstructor = Local<Function>::Cast(info.This());
    Local<Value> argv[] = {
      info[0],
    };
    Local<Value> threadObj = threadConstructor->NewInstance(Isolate::GetCurrent()->GetCurrentContext(), sizeof(argv)/sizeof(argv[0]), argv).ToLocalChecked();

    info.GetReturnValue().Set(threadObj);
  } else {
    Nan::ThrowError("Invalid arguments");
  }
}
NAN_METHOD(Thread::SetChildJsPath) {
  if (info[0]->IsString()) {
    Local<String> childJsPathValue = Local<String>::Cast(info[0]);
    Nan::Utf8String childJsPathValueUtf8(info[0]);
    size_t length = childJsPathValueUtf8.length();
    string childJsPath(*childJsPathValueUtf8, length);

    Thread::setChildJsPath(childJsPath);
  } else {
    Nan::ThrowError("Invalid arguments");
  }
}
NAN_METHOD(Thread::SetNativeRequire) {
  Thread *thread = Thread::getCurrentThread();

  if (info[0]->IsString() && info[1]->IsArray()) {
    Local<String> requireNameValue = Local<String>::Cast(info[0]);
    Nan::Utf8String requireNameUtf8(requireNameValue);
    string requireName(*requireNameUtf8, requireNameUtf8.length());

    Local<Array> requireAddressValue = Local<Array>::Cast(info[1]);
    uintptr_t requireAddress = ((uint64_t)TO_UINT32(requireAddressValue->Get(0)) << 32) | ((uint64_t)TO_UINT32(requireAddressValue->Get(1)) & 0xFFFFFFFF);

    if (requireAddress) {
      nativeRequires.emplace_back(requireName, requireAddress);
    } else {
      Nan::ThrowError("init function address cannot be null");
    }
  } else {
    Nan::ThrowError("invalid arguments");
  }
}
NAN_METHOD(Thread::Terminate) {
  Thread *thread = ObjectWrap::Unwrap<Thread>(info.This());

  // signal exit to thread
  uv_async_send(thread->getExitAsync().get());

  // wait for thread to exit
  void *retval;
  int result = pthread_join(thread->getThread(), &retval);

  // close local message handle
  uv_async_t *messageAsyncOut = thread->getMessageAsyncOut().release();
  uv_close((uv_handle_t *)messageAsyncOut, closeHandleFn);

  // release ownership of handles
  thread->getExitAsync().release();
  thread->getMessageAsyncIn().release();

  // clean up remote message handles
  uv_walk(&thread->getLoop(), walkHandleCleanupFn, nullptr);

  // clean up local thread references
  Thread::removeThreadByKey((uintptr_t)messageAsyncOut);
  thread->removeThreadObject();

  info.GetReturnValue().Set(Nan::New<Integer>(result == 0 ? (*(int *)retval) : 1));
}
NAN_METHOD(Thread::Cancel) {
  Thread *thread = ObjectWrap::Unwrap<Thread>(info.This());

  // forcefully cancel thread
#if !defined(ANDROID) && !defined(LUMIN)
  pthread_cancel(thread->getThread());
#endif

  // wait for thread to exit
  // pthread_join(thread->getThread(), nullptr);

  // close local message handle
  uv_async_t *messageAsyncOut = thread->getMessageAsyncOut().release();
  uv_close((uv_handle_t *)messageAsyncOut, closeHandleFn);

  // release ownership of handles
  // thread->getExitAsync().release();
  // thread->getMessageAsyncIn().release();

  // clean up remote message handles
  // uv_walk(&thread->getLoop(), walkHandleCleanupFn, nullptr);

  // clean up local thread references
  Thread::removeThreadByKey((uintptr_t)messageAsyncOut);
  thread->removeThreadObject();
}
NAN_METHOD(Thread::RequireNative) {
  Thread *thread = Thread::getCurrentThread();

  Local<String> requireNameValue = Local<String>::Cast(info[0]);
  Nan::Utf8String requireNameUtf8(requireNameValue);
  string requireName(*requireNameUtf8, requireNameUtf8.length());

  const vector<pair<string, uintptr_t>> &requires = Thread::getNativeRequires();
  for (size_t i = 0; i < requires.size(); i++) {
    const pair<string, uintptr_t> &require = requires[i];
    const string &name = require.first;
    const uintptr_t address = require.second;

    if (name == requireName) {
      void (*Init)(Local<Object> exports) = (void (*)(Local<Object>))address;
      Local<Object> exportsObj = Nan::New<Object>();
      Init(exportsObj);

      return info.GetReturnValue().Set(exportsObj);
    }
  }

  return Nan::ThrowError("Native module not found");
}
NAN_METHOD(Thread::PostThreadMessageIn) {
  if (info[0]->IsArrayBuffer()) {
    Thread *thread = ObjectWrap::Unwrap<Thread>(info.This());

    Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(info[0]);

    if (arrayBuffer->IsExternal()) {
      QueueEntry queueEntry((uintptr_t)arrayBuffer->GetContents().Data(), arrayBuffer->ByteLength());

      thread->pushMessageIn(queueEntry);
    } else {
      return Nan::ThrowError("ArrayBuffer is not external");
    }
  } else {
    return Nan::ThrowError("invalid arguments");
  }
}
NAN_METHOD(Thread::PostThreadMessageOut) {
  if (info[0]->IsArrayBuffer()) {
    Thread *thread = Thread::getCurrentThread();

    Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(info[0]);

    if (arrayBuffer->IsExternal()) {
      QueueEntry queueEntry((uintptr_t)arrayBuffer->GetContents().Data(), arrayBuffer->ByteLength());

      thread->pushMessageOut(queueEntry);
    } else {
      return Nan::ThrowError("ArrayBuffer is not external");
    }
  } else {
    return Nan::ThrowError("invalid arguments");
  }
}

uv_sem_t sem;
bool locked;

void SemCb(const FunctionCallbackInfo<Value> &info) {
  Isolate *isolate = Isolate::GetCurrent();

  HandleScope scope(isolate);

  isolate->Exit();
  locked = false;
  uv_sem_post(&sem);
}

NAN_METHOD(Run) {
  Thread *thread = Thread::getCurrentThread();
  uv_run(&thread->getLoop(), UV_RUN_ONCE);
}

NAN_METHOD(Pipe) {
  int fds[2];
#ifndef _WIN32
  pipe(fds);
#else
  HANDLE handles[2];
  CreatePipe(&handles[0], &handles[1], nullptr, 0);
  fds[0] = _open_osfhandle((intptr_t)(handles[0]), 0);
  fds[1] = _open_osfhandle((intptr_t)(handles[1]), 0);
#endif

  Local<Array> array = Nan::New<Array>(2);
  array->Set(0, JS_NUM(fds[0]));
  array->Set(1, JS_NUM(fds[1]));
  info.GetReturnValue().Set(array);
}

void InitFunction(Local<Object> exports) {
  exports->Set(JS_STR("Thread"), Thread::Initialize());
  exports->Set(JS_STR("run"), Nan::New<Function>(Run));
  exports->Set(JS_STR("pipe"), Nan::New<Function>(Pipe));

  uintptr_t initFunctionAddress = (uintptr_t)InitFunction;
  Local<Array> initFunctionAddressArray = Nan::New<Array>(2);
  initFunctionAddressArray->Set(0, Nan::New<Integer>((uint32_t)(initFunctionAddress >> 32)));
  initFunctionAddressArray->Set(1, Nan::New<Integer>((uint32_t)(initFunctionAddress & 0xFFFFFFFF)));
  exports->Set(JS_STR("initFunctionAddress"), initFunctionAddressArray);
}
void Init(Local<Object> exports) {
  uv_key_create(&threadKey);
  uv_key_set(&threadKey, nullptr);

  uv_sem_init(&sem, 0);

  InitFunction(exports);
}
string Thread::childJsPath;
map<uintptr_t, Thread*> Thread::threadMap;
vector<pair<string, uintptr_t>> Thread::nativeRequires;

}

#if !defined(ANDROID) && !defined(LUMIN)
NODE_MODULE(NODE_GYP_MODULE_NAME, childProcessThread::Init)
#else
extern "C" {
  void node_register_module_child_process_thread(Local<Object> exports, Local<Value> module, Local<Context> context) {
    childProcessThread::Init(exports);
  }
}
#endif
