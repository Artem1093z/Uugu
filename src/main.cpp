#include "zygisk.hpp"
#include "utils.h"

#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <chrono>
#include <unistd.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_opengl3.h"

// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ДЛЯ EGL И ZYGISK
JavaVM* g_JVM = nullptr; 
jobject g_Activity = nullptr;    // <--- ВЕРНУЛ СПЕЦИАЛЬНО ДЛЯ utils.h
jobject g_SurfaceView = nullptr; 
bool g_SurfaceReady = false; 
pid_t g_Pid = 0;                 // <--- ВЕРНУЛ ДЛЯ ПОИСКА ПАМЯТИ
int g_ScreenW = 0, g_ScreenH = 0;
uintptr_t g_Il2CppBase_RXP = 0;

extern void c_thread(); // Поток из utils.h, который ищет SurfaceView игры

// ======================== ПОТОК ОТРИСОВКИ IMGUI ========================
void r_thread() {
    // Ждем пока c_thread найдет SurfaceView игры
    while (!g_SurfaceReady) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    JNIEnv* e; 
    if (g_JVM->AttachCurrentThread(&e, NULL) != JNI_OK) return; 
    
    jclass svC = e->FindClass("android/view/SurfaceView"); 
    jobject h = e->CallObjectMethod(g_SurfaceView, e->GetMethodID(svC, "getHolder", "()Landroid/view/SurfaceHolder;")); 
    jclass hC = e->FindClass("android/view/SurfaceHolder"); 
    jmethodID gs = e->GetMethodID(hC, "getSurface", "()Landroid/view/Surface;"); 
    jclass sC = e->FindClass("android/view/Surface"); 
    jmethodID iv = e->GetMethodID(sC, "isValid", "()Z");
    
    while (true) {
        jobject s = nullptr; 
        while(true) {
            s = e->CallObjectMethod(h, gs); 
            if(s) {
                if(e->CallBooleanMethod(s, iv)) break; 
                e->DeleteLocalRef(s);
            } 
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        
        ANativeWindow* w = ANativeWindow_fromSurface(e, s); 
        EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY); 
        eglInitialize(d, 0, 0); 
        EGLint a[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE}; 
        EGLConfig c; EGLint nc; eglChooseConfig(d, a, &c, 1, &nc); 
        EGLint f; eglGetConfigAttrib(d, c, EGL_NATIVE_VISUAL_ID, &f); 
        ANativeWindow_setBuffersGeometry(w, 0, 0, f); 
        EGLSurface es = eglCreateWindowSurface(d, c, w, NULL); 
        EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}; 
        EGLContext ctx = eglCreateContext(d, c, NULL, ca); 
        eglMakeCurrent(d, es, es, ctx); 
        eglQuerySurface(d, es, EGL_WIDTH, &g_ScreenW); 
        eglQuerySurface(d, es, EGL_HEIGHT, &g_ScreenH);
        
        ImGui::CreateContext(); 
        ImGuiIO& io = ImGui::GetIO(); 
        io.IniFilename = nullptr; 
        io.DisplaySize = ImVec2((float)g_ScreenW, (float)g_ScreenH); 
        
        ImGui_ImplOpenGL3_Init("#version 300 es"); 
        ImGui::StyleColorsDark();
        
        bool sl = false; 
        while (!sl) {
            if(!e->CallBooleanMethod(s, iv)) { sl = true; break; } 
            glClearColor(0, 0, 0, 0); 
            glClear(GL_COLOR_BUFFER_BIT);
            
            ImGui_ImplOpenGL3_NewFrame(); 
            ImGui::NewFrame(); 
            
            // ======================== ПУСТОЕ МЕНЮ ========================
            ImGui::SetNextWindowSize(ImVec2(600, 350), ImGuiCond_FirstUseEver);
            ImGui::Begin("Manes Zygisk [TEST BUILD]", nullptr, ImGuiWindowFlags_NoCollapse);
            
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[+] Zygisk Injection: OK");
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "[+] EGL Hook: OK");
            ImGui::Separator();
            
            ImGui::TextWrapped("Если ты видишь это меню и игра не крашится, значит античит реагировал на mprotect, хуки (PageGuard) или чтение памяти.");
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "ВНИМАНИЕ: Инпуты (нажатия) отключены для стерильности теста. Вкладки кликать нельзя!");
            
            ImGui::Dummy(ImVec2(0, 20));
            if (ImGui::BeginTabBar("TestTabs")) {
                if (ImGui::BeginTabItem("Visuals")) { ImGui::Text("Здесь будет ESP"); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Aimbot")) { ImGui::Text("Здесь будет Aim"); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Misc")) { ImGui::Text("Здесь будут моды"); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            
            ImGui::End();
            // =============================================================
            
            ImGui::Render(); 
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); 
            if(eglSwapBuffers(d, es) == EGL_FALSE) sl = true; 
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext(); eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglDestroyContext(d, ctx); eglDestroySurface(d, es); eglTerminate(d); ANativeWindow_release(w); e->DeleteLocalRef(s);
    }
}

// ======================== ГЛАВНЫЙ ПОТОК ========================
void m_thread() { 
    g_Pid = getpid(); // Инициализируем PID для utils.h
    
    // Ждем загрузки игрового движка
    while(!g_Il2CppBase_RXP) { 
        g_Il2CppBase_RXP = get_lib_rxp("libil2cpp.so"); 
        std::this_thread::sleep_for(std::chrono::seconds(1)); 
    } 
    
    // Запускаем поиск SurfaceView и отрисовку ImGui
    std::thread(c_thread).detach(); 
    std::thread(r_thread).detach(); 
}

// ======================== ИНЖЕКТОР ZYGISK ========================
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

        env->GetJavaVM(&g_JVM);
        
        // ❌ MPROTECT УДАЛЕН ПОЛНОСТЬЮ ❌
        // Мы НЕ стираем ELF заголовок в этом тесте.

        std::thread(m_thread).detach();
    }

private:
    zygisk::Api *api;
    JNIEnv *env;
    bool isTarget = false;
};

REGISTER_ZYGISK_MODULE(ManesModule)
