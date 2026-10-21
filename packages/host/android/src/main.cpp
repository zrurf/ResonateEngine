/* The Android shell: a NativeActivity that hands the engine's startup the
 * activity's window. The APK carries this library as libmain.so, next to the
 * module plugins and their manifests. */

#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <resonate/startup/startup.hpp>
#include <resonate/window/android.hpp>

namespace
{

void logFrame(const char* kind, int index, void* address)
{
    Dl_info info{};
    if (dladdr(address, &info) != 0 && info.dli_fbase != nullptr)
    {
        const auto base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
        const auto offset = reinterpret_cast<std::uintptr_t>(address) - base;
        __android_log_print(ANDROID_LOG_FATAL, "resonate", "%s %02d 0x%llx %s", kind, index,
                            static_cast<unsigned long long>(offset),
                            info.dli_fname != nullptr ? info.dli_fname : "?");
    }
    else
    {
        __android_log_print(ANDROID_LOG_FATAL, "resonate", "%s %02d %p", kind, index, address);
    }
}

/* debuggerd mis-unwinds this build's release frames, so the handler reports the
   fault and walks the frame chain itself before the process goes. */
void crashHandler(int number, siginfo_t* info, void* context)
{
    __android_log_print(ANDROID_LOG_FATAL, "resonate", "fatal signal %d at %p", number,
                        info != nullptr ? info->si_addr : nullptr);

    auto* state = static_cast<ucontext_t*>(context);
    logFrame("pc", 0, reinterpret_cast<void*>(state->uc_mcontext.pc));

#if defined(__aarch64__)
    uintptr_t frame = state->uc_mcontext.regs[29];
    for (int index = 0; index < 32 && frame != 0U; ++index)
    {
        const auto* words = reinterpret_cast<const uintptr_t*>(frame);
        const uintptr_t next = words[0];
        const uintptr_t return_address = words[1];
        logFrame("fp", index, reinterpret_cast<void*>(return_address));
        if (next <= frame || (next % 16U) != 0U)
        {
            break;
        }
        frame = next;
    }
#endif

    _exit(1);
}

void installCrashHandler()
{
    struct sigaction action = {};
    action.sa_sigaction = &crashHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGABRT, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
}

/* A NativeActivity has no console, and the engine's reports go to stderr. The
   pipe forwards complete lines to logcat, so a device run leaves the same trail
   as a desktop one. */
void* pumpLogcat(void* user_data)
{
    const int descriptor = *static_cast<int*>(user_data);
    std::string pending;
    char buffer[512];
    for (;;)
    {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count <= 0)
        {
            break;
        }
        pending.append(buffer, static_cast<std::size_t>(count));
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos)
        {
            __android_log_write(ANDROID_LOG_INFO, "resonate", pending.substr(0, newline).c_str());
            pending.erase(0, newline + 1);
        }
        /* A line that never ends must not grow without bound. */
        if (pending.size() > 2048U)
        {
            pending.clear();
        }
    }
    return nullptr;
}

void redirectStdioToLogcat()
{
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
    {
        return;
    }
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    close(pipe_fds[1]);

    static int read_end = pipe_fds[0];
    pthread_t thread;
    if (pthread_create(&thread, nullptr, &pumpLogcat, &read_end) == 0)
    {
        pthread_detach(thread);
    }
}

void logError(const std::string& message)
{
    __android_log_print(ANDROID_LOG_ERROR, "resonate", "%s", message.c_str());
}

/* The APK's native library directory holds the plugins. The package manager
   owns it, so the manifests are staged in the app's own files instead. */
std::string nativeLibraryDirectory(android_app* app)
{
    ANativeActivity* activity = app->activity;
    if (activity == nullptr || activity->vm == nullptr || activity->clazz == nullptr)
    {
        return {};
    }

    JavaVM* vm = activity->vm;
    JNIEnv* env = nullptr;
    const bool already_attached =
        vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK;
    if (!already_attached && vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    {
        return {};
    }

    std::string result;
    jclass activity_class = env->GetObjectClass(activity->clazz);
    jmethodID get_info = env->GetMethodID(activity_class, "getApplicationInfo",
                                          "()Landroid/content/pm/ApplicationInfo;");
    jobject info = get_info != nullptr ? env->CallObjectMethod(activity->clazz, get_info) : nullptr;
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        info = nullptr;
    }
    if (info != nullptr)
    {
        jclass info_class = env->GetObjectClass(info);
        jfieldID field = env->GetFieldID(info_class, "nativeLibraryDir", "Ljava/lang/String;");
        auto value = field != nullptr
                         ? static_cast<jstring>(env->GetObjectField(info, field))
                         : nullptr;
        if (value != nullptr)
        {
            const char* chars = env->GetStringUTFChars(value, nullptr);
            if (chars != nullptr)
            {
                result = chars;
                env->ReleaseStringUTFChars(value, chars);
            }
        }
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
            result.clear();
        }
    }

    if (!already_attached)
    {
        vm->DetachCurrentThread();
    }
    return result;
}

/* Copies one directory of packaged assets into app storage once per launch.
   The plugin libraries stay where the package manager put them; their manifests
   and the SPIR-V shaders come out of the APK, which is read-only. */
std::string stageAssets(android_app* app, const char* asset_directory)
{
    ANativeActivity* activity = app->activity;
    if (activity == nullptr || activity->internalDataPath == nullptr ||
        activity->assetManager == nullptr)
    {
        return {};
    }

    const std::string directory = std::string(activity->internalDataPath) + "/" + asset_directory;
    mkdir(directory.c_str(), 0700);

    AAssetDir* assets = AAssetManager_openDir(activity->assetManager, asset_directory);
    if (assets == nullptr)
    {
        return {};
    }

    const char* name = nullptr;
    while ((name = AAssetDir_getNextFileName(assets)) != nullptr)
    {
        const std::string asset_name = std::string(asset_directory) + "/" + name;
        AAsset* asset =
            AAssetManager_open(activity->assetManager, asset_name.c_str(), AASSET_MODE_BUFFER);
        if (asset == nullptr)
        {
            continue;
        }

        const std::string path = directory + "/" + name;
        FILE* file = std::fopen(path.c_str(), "wb");
        if (file != nullptr)
        {
            char buffer[4096];
            int read = 0;
            while ((read = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
            {
                std::fwrite(buffer, 1, static_cast<std::size_t>(read), file);
            }
            std::fclose(file);
        }
        AAsset_close(asset);
    }
    AAssetDir_close(assets);
    return directory;
}

} // namespace

void android_main(android_app* app)
{
    installCrashHandler();
    redirectStdioToLogcat();
    resonate::window::attachAndroidApp(app);

    std::vector<std::string> arguments{"resonate"};
    const std::string libraries = nativeLibraryDirectory(app);
    if (libraries.empty())
    {
        logError("cannot resolve the native library directory");
    }
    else
    {
        arguments.push_back("--plugin-dir");
        arguments.push_back(libraries);
    }

    const std::string manifests = stageAssets(app, "plugins");
    if (!manifests.empty())
    {
        arguments.push_back("--plugin-manifest-dir");
        arguments.push_back(manifests);
    }

    const std::string shaders = stageAssets(app, "shaders");
    if (!shaders.empty())
    {
        setenv("RESONATE_SHADER_PATH", shaders.c_str(), 1);
    }

    resonate::startup::run(arguments, "Resonate");
}
