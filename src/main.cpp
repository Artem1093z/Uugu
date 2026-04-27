#include "zygisk.hpp"
#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <string.h>
#include <sstream>
#include <iomanip>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Manes", __VA_ARGS__)

// 🔥 ИСПРАВЛЕНИЕ БАГА КОМПИЛЯТОРА NDK r26b (Clang 17) 🔥

// Безопасное чтение памяти через ядро
bool SafeRead(uintptr_t addr, void* buf, size_t size) {
    struct iovec local = {buf, size};
    struct iovec remote = {(void*)addr, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}

// Конвертер байтов в красивый HEX текст
std::string ToHexString(uint8_t* data, size_t len) {
    std::stringstream ss;
    for(size_t i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        if ((i + 1) % 16 == 0) ss << "\n";
    }
    return ss.str();
}

// =========================================================================
// 🔥 ЛОВУШКА ДЛЯ АНТИЧИТА (JNIEnv HOOK) 🔥
// =========================================================================
typedef jint (*RegisterNatives_t)(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint nMethods);
RegisterNatives_t orig_RegisterNatives = nullptr;

jint hk_RegisterNatives(JNIEnv* env, jclass clazz, const JNINativeMethod* methods, jint nMethods) {
    // Античит регистрирует свои функции. Проверяем каждую!
    for (int i = 0; i < nMethods; i++) {
        if (methods[i].name) {
            if (strcmp(methods[i].name, "NG_onCreate") == 0 || strcmp(methods[i].name, "NG_attachBaseContext") == 0) {
                
                LOGI("==================================================");
                LOGI("🔥 BINGO! JNI ANTI-CHEAT METHOD CAPTURED 🔥");
                LOGI("[+] Method Name: %s", methods[i].name);
                LOGI("[+] C++ Function Address: %p", methods[i].fnPtr);
                
                // ДАМПИМ 128 БАЙТ МАШИННОГО КОДА!
                uint8_t asmCode[128];
                if (SafeRead((uintptr_t)methods[i].fnPtr, asmCode, sizeof(asmCode))) {
                    LOGI("================ ASSEMBLY HEX DUMP ================");
                    std::string hex = ToHexString(asmCode, sizeof(asmCode));
                    
                    std::istringstream stream(hex);
                    std::string line;
                    while (std::getline(stream, line)) {
                        LOGI("%s", line.c_str());
                    }
                    LOGI("==================================================");
                } else {
                    LOGI("[-] Failed to read function memory!");
                }
            }
        }
    }
    
    // Вызываем оригинальную функцию, чтобы игра не крашнулась и работала дальше
    return orig_RegisterNatives(env, clazz, methods, nMethods);
}

// =========================================================================
// 🔥 ИНЖЕКТОР ZYGISK 🔥
// =========================================================================
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

        // Взламываем внутреннюю таблицу функций Android (JNIEnv)
        JNINativeInterface* table = (JNINativeInterface*)env->functions;
        
        uintptr_t page_size = sysconf(_SC_PAGESIZE);
        uintptr_t page_start = ((uintptr_t)table) & ~(page_size - 1);
        
        // Открываем память таблицы на запись через системный вызов ядра
        if (syscall(__NR_mprotect, (void*)page_start, page_size * 2, PROT_READ | PROT_WRITE) == 0) {
            
            // Устанавливаем ловушку
            orig_RegisterNatives = table->RegisterNatives;
            table->RegisterNatives = hk_RegisterNatives;
            
            // Закрываем память обратно (чтобы сканеры ничего не заподозрили)
            syscall(__NR_mprotect, (void*)page_start, page_size * 2, PROT_READ); 
            
            LOGI("[+] JNIEnv->RegisterNatives successfully hooked!");
        } else {
            LOGI("[-] Failed to hook JNIEnv table!");
        }
    }

private:
    zygisk::Api *api;
    JNIEnv *env;
    bool isTarget = false;
};

REGISTER_ZYGISK_MODULE(ManesModule)
