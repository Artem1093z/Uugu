#include "zygisk.hpp"
#include "utils.h"

#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <thread>
#include <chrono>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "logo.h"

// ОПРЕДЕЛЕНИЕ СИСТЕМНЫХ ГЛОБАЛЬНЫХ ПЕРЕМЕННЫХ
JavaVM* g_JVM = nullptr; 
jobject g_SurfaceView = nullptr; 
bool g_SurfaceReady = false; 
pid_t g_Pid = 0; 
int g_ScreenW = 0, g_ScreenH = 0;

// AUTH GLOBALS (АВТОВХОД ДЛЯ ТЕСТА)
bool g_Authenticated = true; // Сразу пускаем в меню!
uint64_t g_AuthValidationHash = 0x41BA0EA2; 
std::string g_AuthDaysLeft = "TEST";

// НАСТРОЙКИ (Пустышки для отрисовки меню)
bool g_IsMenuMinimized = false; ImVec2 g_MinimizedPos = ImVec2(100, 100); float g_MenuScale = 1.0f; float g_TempMenuScale = 1.0f; double g_LastScaleEditTime = 0.0; bool g_IsScrolling = false; 
bool cfg_esp_enable=true, cfg_esp_line=true, cfg_esp_box=true, cfg_esp_name=true, cfg_esp_clan=true, cfg_esp_dist=true, cfg_esp_count=true;
bool cfg_loot_enable=false, cfg_pickup_enable=false, cfg_ore_enable=false, cfg_scrap_enable=false, cfg_tree_enable=false, cfg_cupboard_enable=false, cfg_sleeper_enable=false;
bool cfg_aim_enable=true, cfg_aim_vis_check=true, cfg_aim_draw_fov=true, cfg_aim_scope_only=true;
float cfg_aim_fov=120.0f, cfg_aim_max_dist=250.0f; float cfg_aim_smooth = 1.5f; int cfg_aim_bone=0, cfg_line_pos=0;
bool cfg_magic_bullet=false, cfg_mb_draw_fov=true, cfg_mb_draw_line=true; float cfg_mb_fov=150.0f; 
bool cfg_fast_heal=false; bool cfg_no_recoil=false, cfg_fast_reload=false, cfg_fast_shoot=false;
bool cfg_unlock_fps=false, cfg_xray_enable=false; float cfg_xray_dist=2.0f;
bool cfg_auto_farm=false, cfg_farm_stone=true, cfg_farm_ferum=true, cfg_farm_sulfur=true, cfg_farm_scrap=true, cfg_farm_tree=true; 

GLuint g_LogoTex = 0; int g_LogoW = 0, g_LogoH = 0;

// ======================== IMGUI УТИЛИТЫ ========================
bool DPanel(const char* l, const char* d, bool* v) { ImGuiWindow* w=ImGui::GetCurrentWindow(); if(w->SkipItems)return false; ImGuiContext& g=*GImGui; ImGuiID id=w->GetID(l); ImVec2 p=w->DC.CursorPos; ImVec2 s(ImGui::GetContentRegionAvail().x,65.0f*g_MenuScale); ImRect b(p,ImVec2(p.x+s.x,p.y+s.y)); ImGui::ItemSize(b,g.Style.FramePadding.y); if(!ImGui::ItemAdd(b,id))return false; bool hv,hl; bool pr=ImGui::ButtonBehavior(b,id,&hv,&hl); if(g_IsScrolling)pr=false; if(pr)*v=!*v; float da=ImClamp(g.IO.DeltaTime*14.0f,0.0f,1.0f); float dh=ImClamp(g.IO.DeltaTime*12.0f,0.0f,1.0f); float* av=w->StateStorage.GetFloatRef(id,0.0f); *av=ImLerp(*av,*v?1.0f:0.0f,da); float* hvv=w->StateStorage.GetFloatRef(id+1,0.0f); *hvv=ImLerp(*hvv,(hv&&!g_IsScrolling)?1.0f:0.0f,dh); ImU32 bg=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.12f,0.12f,0.12f,1.0f),ImVec4(0.17f,0.17f,0.17f,1.0f),*hvv)); w->DrawList->AddRectFilled(b.Min,b.Max,bg,10.0f*g_MenuScale); w->DrawList->AddText(ImGui::GetFont(),24.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+12*g_MenuScale),IM_COL32(240,240,240,255),l); if(d)w->DrawList->AddText(ImGui::GetFont(),18.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+38*g_MenuScale),IM_COL32(140,140,140,255),d); float sw=44.0f*g_MenuScale, sh=24.0f*g_MenuScale; ImVec2 sp(b.Max.x-sw-15*g_MenuScale,b.Min.y+(s.y-sh)/2.0f); ImU32 sbg=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.3f,0.3f,0.3f,1.0f),ImVec4(1.0f,1.0f,1.0f,1.0f),*av)); w->DrawList->AddRectFilled(sp,ImVec2(sp.x+sw,sp.y+sh),sbg,sh/2.0f); ImU32 cc=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.9f,0.9f,0.9f,1.0f),ImVec4(0.1f,0.1f,0.1f,1.0f),*av)); float cx=ImLerp(sp.x+sh/2.0f,sp.x+sw-sh/2.0f,*av); w->DrawList->AddCircleFilled(ImVec2(cx,sp.y+sh/2.0f),sh/2.0f-3.0f*g_MenuScale,cc); return pr; }
bool DSlider(const char* l, const char* d, float* v, float mn, float mx, const char* f) { ImGuiWindow* w=ImGui::GetCurrentWindow(); if(w->SkipItems)return false; ImGuiContext& g=*GImGui; ImGuiID id=w->GetID(l); ImVec2 p=w->DC.CursorPos; ImVec2 s(ImGui::GetContentRegionAvail().x,65.0f*g_MenuScale); ImRect b(p,ImVec2(p.x+s.x,p.y+s.y)); ImGui::ItemSize(b,g.Style.FramePadding.y); if(!ImGui::ItemAdd(b,id))return false; bool hv,hl; ImGui::ButtonBehavior(b,id,&hv,&hl); if(hl&&!g_IsScrolling){ float t=ImClamp((g.IO.MousePos.x-b.Min.x)/s.x,0.0f,1.0f); *v=mn+t*(mx-mn); } float t=(*v-mn)/(mx-mn); float dh=ImClamp(g.IO.DeltaTime*12.0f,0.0f,1.0f); float* hvv=w->StateStorage.GetFloatRef(id+1,0.0f); *hvv=ImLerp(*hvv,(hv&&!g_IsScrolling)?1.0f:0.0f,dh); ImU32 bg=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.12f,0.12f,0.12f,1.0f),ImVec4(0.17f,0.17f,0.17f,1.0f),*hvv)); w->DrawList->AddRectFilled(b.Min,b.Max,bg,10.0f*g_MenuScale); ImU32 fc=IM_COL32(70,70,70,255); ImVec2 fm(b.Min.x+s.x*t,b.Max.y); w->DrawList->AddRectFilled(b.Min,fm,fc,10.0f*g_MenuScale,t<0.99f?ImDrawFlags_RoundCornersLeft:ImDrawFlags_RoundCornersAll); w->DrawList->AddText(ImGui::GetFont(),24.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+12*g_MenuScale),IM_COL32(240,240,240,255),l); if(d)w->DrawList->AddText(ImGui::GetFont(),18.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+38*g_MenuScale),IM_COL32(180,180,180,255),d); char vb[64]; snprintf(vb,sizeof(vb),f,*v); ImVec2 vsz=ImGui::GetFont()->CalcTextSizeA(20.0f*g_MenuScale,FLT_MAX,0.0f,vb); w->DrawList->AddText(ImGui::GetFont(),20.0f*g_MenuScale,ImVec2(b.Max.x-vsz.x-15*g_MenuScale,b.Min.y+22*g_MenuScale),IM_COL32(255,255,255,255),vb); return hl&&!g_IsScrolling; }

void LoadLogoTexture() { if (g_LogoTex != 0) return; int c; unsigned char* d = stbi_load_from_memory(logo_data, logo_len, &g_LogoW, &g_LogoH, &c, 4); if (d) { glGenTextures(1, &g_LogoTex); glBindTexture(GL_TEXTURE_2D, g_LogoTex); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_LogoW, g_LogoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, d); stbi_image_free(d); } }

void DrawMenu() {
    if (g_IsMenuMinimized) { ImGui::SetNextWindowPos(g_MinimizedPos,ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize(ImVec2(70*g_MenuScale,70*g_MenuScale)); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0f); ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0,0,0,0)); ImGui::Begin("F",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar); ImVec2 p=ImGui::GetWindowPos(); g_MinimizedPos=p; if(g_LogoTex){ImGui::GetWindowDrawList()->AddImage((void*)(intptr_t)g_LogoTex,p,ImVec2(p.x+70*g_MenuScale,p.y+70*g_MenuScale));} ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2); return; }
    static int c_t=0; static float t_a[4]={1.0f,0.0f,0.0f,0.0f}; ImGui::SetNextWindowSize(ImVec2(800*g_MenuScale,520*g_MenuScale),ImGuiCond_Always); ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0.08f,0.08f,0.08f,1.0f)); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,12.0f*g_MenuScale); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0f); ImGui::Begin("M",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings); ImDrawList* d=ImGui::GetWindowDrawList(); ImVec2 p=ImGui::GetWindowPos(); ImVec2 s=ImGui::GetWindowSize(); d->AddRectFilled(p,ImVec2(p.x+220*g_MenuScale,p.y+s.y),IM_COL32(25,25,25,255),12.0f*g_MenuScale,ImDrawFlags_RoundCornersLeft); ImVec2 lm=ImVec2(p.x+35*g_MenuScale,p.y+30*g_MenuScale); if(g_LogoTex){d->AddImage((void*)(intptr_t)g_LogoTex,lm,ImVec2(lm.x+150*g_MenuScale,lm.y+150*g_MenuScale)); }
    
    char subBuf[64]; snprintf(subBuf, sizeof(subBuf), "%s Days", g_AuthDaysLeft.c_str()); ImGui::SetCursorPos(ImVec2(35*g_MenuScale, 190*g_MenuScale)); ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", subBuf);

    const char* tb[]={"Visuals","Aim","Magic","Misc"}; ImGui::SetCursorPos(ImVec2(0,220*g_MenuScale)); ImGui::BeginChild("T",ImVec2(220*g_MenuScale,s.y-220*g_MenuScale),false,ImGuiWindowFlags_NoScrollbar); for(int i=0;i<4;i++){ImGui::SetCursorPosX(15*g_MenuScale); bool sel=(c_t==i); ImVec2 cp=ImGui::GetCursorScreenPos(); if(ImGui::InvisibleButton(tb[i],ImVec2(190*g_MenuScale,50*g_MenuScale)))c_t=i; float da=ImClamp(ImGui::GetIO().DeltaTime*14.0f,0.0f,1.0f); t_a[i]=ImLerp(t_a[i],sel?1.0f:0.0f,da); ImU32 bc=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0,0,0,0),ImVec4(1,1,1,0.1f),t_a[i])); ImGui::GetWindowDrawList()->AddRectFilled(cp,ImVec2(cp.x+190*g_MenuScale,cp.y+50*g_MenuScale),bc,8.0f*g_MenuScale); if(t_a[i]>0.01f)ImGui::GetWindowDrawList()->AddRectFilled(cp,ImVec2(cp.x+4*g_MenuScale,cp.y+50*g_MenuScale),IM_COL32(255,255,255,(int)(255*t_a[i])),4.0f*g_MenuScale); ImU32 tc=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.5f,0.5f,0.5f,1.0f),ImVec4(1,1,1,1),t_a[i])); ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),22.0f*g_MenuScale,ImVec2(cp.x+20*g_MenuScale,cp.y+14*g_MenuScale),tc,tb[i]); ImGui::SetCursorPosY(ImGui::GetCursorPosY()+5*g_MenuScale);} ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(240*g_MenuScale,20*g_MenuScale)); ImGui::BeginChild("C",ImVec2(s.x-260*g_MenuScale,s.y-40*g_MenuScale),false,ImGuiWindowFlags_NoScrollbar); ImGuiWindow* w=ImGui::GetCurrentWindow(); bool ih=ImGui::IsMouseHoveringRect(w->Pos,ImVec2(w->Pos.x+w->Size.x,w->Pos.y+w->Size.y)); static ImVec2 dsp; static float ssy; static bool ita=false; if(ih&&ImGui::IsMouseClicked(0)){dsp=ImGui::GetIO().MousePos; ssy=w->Scroll.y; ita=true; g_IsScrolling=false;} if(ita){if(ImGui::IsMouseDown(0)){float dy=ImGui::GetIO().MousePos.y-dsp.y; if(std::abs(dy)>5.0f*g_MenuScale){g_IsScrolling=true; w->Scroll.y=ssy-dy;}}else{ita=false; g_IsScrolling=false;}} if(w->Scroll.y<0.0f)w->Scroll.y=0.0f; if(w->Scroll.y>w->ScrollMax.y)w->Scroll.y=w->ScrollMax.y; if(ih&&ImGui::GetIO().MouseWheel!=0.0f)w->Scroll.y-=ImGui::GetIO().MouseWheel*30.0f*g_MenuScale; d->AddText(ImGui::GetFont(),32.0f*g_MenuScale,ImGui::GetCursorScreenPos(),IM_COL32(255,255,255,255),tb[c_t]); ImGui::Dummy(ImVec2(0,40*g_MenuScale));
    
    if(c_t==0){ DPanel("Enable ESP","On/Off",&cfg_esp_enable); DPanel("Box","Draw Box",&cfg_esp_box); DPanel("Line","Draw Line",&cfg_esp_line); DPanel("Name","Draw Name",&cfg_esp_name); }
    else if(c_t==1){ DPanel("Enable Aim","Auto aim",&cfg_aim_enable); DSlider("FOV Size","Circle Size",&cfg_aim_fov,10.0f,360.0f,"%.0f px"); }
    else if(c_t==2){ DPanel("Magic Bullet","Silent Aim", &cfg_magic_bullet); DPanel("Fast Reload","Instant reload",&cfg_fast_reload); }
    else if(c_t==3){ DPanel("Unlock FPS", "TEST BUILD", &cfg_unlock_fps); }
    
    ImGui::EndChild(); ImGui::End(); ImGui::PopStyleVar(3); ImGui::PopStyleColor();
}

// ======================== ГЛАВНЫЙ ПОТОК РЕНДЕРА ========================
void r_thread() {
    g_Pid = getpid(); 
    // Ждем получения SurfaceView от Android
    while (!g_SurfaceReady) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    JNIEnv* e; if (g_JVM->AttachCurrentThread(&e, NULL) != JNI_OK) return; 

    jclass svC=e->FindClass("android/view/SurfaceView"); 
    jobject h=e->CallObjectMethod(g_SurfaceView,e->GetMethodID(svC,"getHolder","()Landroid/view/SurfaceHolder;")); 
    jclass hC=e->FindClass("android/view/SurfaceHolder"); 
    jmethodID gs=e->GetMethodID(hC,"getSurface","()Landroid/view/Surface;"); 
    jclass sC=e->FindClass("android/view/Surface"); 
    jmethodID iv=e->GetMethodID(sC,"isValid","()Z");
    
    while (true) {
        jobject s=nullptr; 
        while(true){
            s=e->CallObjectMethod(h,gs); 
            if(s){ if(e->CallBooleanMethod(s,iv)) break; e->DeleteLocalRef(s); } 
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        ANativeWindow* w=ANativeWindow_fromSurface(e,s); 
        EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d,0,0); 
        EGLint a[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_BLUE_SIZE,8,EGL_GREEN_SIZE,8,EGL_RED_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE}; 
        EGLConfig c; EGLint nc; eglChooseConfig(d,a,&c,1,&nc); EGLint f; eglGetConfigAttrib(d,c,EGL_NATIVE_VISUAL_ID,&f); 
        ANativeWindow_setBuffersGeometry(w,0,0,f); EGLSurface es=eglCreateWindowSurface(d,c,w,NULL); 
        EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; EGLContext ctx=eglCreateContext(d,c,NULL,ca); 
        eglMakeCurrent(d,es,es,ctx); eglQuerySurface(d,es,EGL_WIDTH,&g_ScreenW); eglQuerySurface(d,es,EGL_HEIGHT,&g_ScreenH);
        
        ImGui::CreateContext(); ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr; 
        io.DisplaySize=ImVec2((float)g_ScreenW,(float)g_ScreenH); 
        ImVector<ImWchar> r; ImFontGlyphRangesBuilder bd; bd.AddRanges(io.Fonts->GetGlyphRangesCyrillic()); bd.BuildRanges(&r); 
        io.Fonts->AddFontFromFileTTF("/system/fonts/Roboto-Regular.ttf",26.0f,NULL,r.Data); 
        ImGui_ImplOpenGL3_Init("#version 300 es"); LoadLogoTexture(); 
        ImGuiStyle& st=ImGui::GetStyle(); ImGui::StyleColorsDark(&st); ImGuiStyle bst=st; bool sl=false; float lsc=1.0f;
        
        while (!sl) {
            if(!e->CallBooleanMethod(s,iv)){sl=true;break;} 
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            
            // ПУСТЫЕ ИНПУТЫ (КЛИКАТЬ НЕЛЬЗЯ, ТОЛЬКО СМОТРЕТЬ)
            io.MousePos = ImVec2(-1, -1);
            io.MouseDown[0] = false;
            
            ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame(); 
            DrawMenu(); // Рисуем меню сразу (т.к. мы сделали автовход)
            ImGui::Render(); 
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); 
            if(eglSwapBuffers(d,es)==EGL_FALSE)sl=true; 
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext(); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(d,ctx); eglDestroySurface(d,es); eglTerminate(d); ANativeWindow_release(w); e->DeleteLocalRef(s); g_LogoTex = 0;
    }
}

void m_thread() { 
    // Ждем 5 секунд, чтобы игра прогрузилась
    std::this_thread::sleep_for(std::chrono::seconds(5)); 
    
    // Запускаем перехватчик EGL SurfaceView (Он находится в utils.h, поэтому мы его вызываем)
    std::thread(c_thread).detach(); 
    
    // Запускаем рендер ImGui
    std::thread(r_thread).detach(); 
}

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
        
        // НИКАКОГО ВМЕШАТЕЛЬСТВА В ПАМЯТЬ.
        // Мы не стираем ELF, не делаем mprotect. Просто запускаем поток.
        std::thread(m_thread).detach();
    }

private:
    zygisk::Api *api;
    JNIEnv *env;
    bool isTarget = false;
};

REGISTER_ZYGISK_MODULE(ManesModule)
