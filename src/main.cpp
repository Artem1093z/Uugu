#include "zygisk.hpp"
#include <android/log.h>
#include <string.h>

// =========================================================================
// 👻 GHOST TEST (ТЕСТ ПРИЗРАКА) 👻
// =========================================================================
// В этом тесте нет НИКАКИХ потоков (ни r_thread, ни c_thread).
// Нет ImGui. Нет mprotect. Нет оффсетов.
// Мы просто внедряемся в память игры и ничего не делаем.

class ManesModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!args || !args->nice_name) return;
        const char *process = env->GetStringUTFChars(args->nice_name, nullptr);
        
        isTarget = (strcmp(process, "com.catsbit.oxidesurvivalisland") == 0);
        env->ReleaseStringUTFChars(args->nice_name, process);

        if (!isTarget) {
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        } else {
            api->setOption(zygisk::Option::FORCE_DENYLIST_UNMOUNT);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (!isTarget) return;

        // Отправляем секретное сообщение в системный лог (logcat), чтобы убедиться, что мы внутри
        __android_log_print(ANDROID_LOG_INFO, "Manes", "[+] GHOST TEST: Чит успешно внедрен, но спит.");
        
        // Мы НЕ создаем std::thread.
        // Мы НЕ прячем ELF.
        // Просто замираем.
    }

private:
    zygisk::Api *api;
    JNIEnv *env;
    bool isTarget = false;
};

REGISTER_ZYGISK_MODULE(ManesModule)
