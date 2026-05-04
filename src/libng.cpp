#include "utils.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <new>

#include <signal.h>
#include <ucontext.h>
#include <android/native_window_jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "logo.h" 

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <link.h>
#include <dlfcn.h>
#include <sys/mman.h>

// Подключаем Dobby для инлайн-хуков
#include <dobby.h>

#define LOG_TAG "Manes_TrojanNG"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXPORT extern "C" __attribute__((visibility("default")))

// =====================================================================
// 🔥 СТРУКТУРЫ ЭМУЛЯТОРА АНТИЧИТА 🔥
// =====================================================================
struct Il2CppStringKey { uint64_t meta; uint64_t size; char *data; };
struct InternalCallNode { InternalCallNode *left; InternalCallNode *right; InternalCallNode *parent; uint64_t flags; Il2CppStringKey name; uint64_t method; };
struct PendingInternalCallNode { PendingInternalCallNode *left; PendingInternalCallNode *right; PendingInternalCallNode *parent; uint64_t flags; Il2CppStringKey name; uint64_t replacement; uint64_t *target_slot; };
struct InternalCallMap { uint64_t header; InternalCallNode *root; };
struct PendingInternalCallMap { uint64_t header; PendingInternalCallNode *root; };

extern "C" InternalCallMap g_internalCalls = {};
extern "C" PendingInternalCallMap g_pendingInternalCalls = {};

static inline InternalCallNode *internal_call_end(InternalCallMap *map) { return reinterpret_cast<InternalCallNode *>(&map->root); }
static inline PendingInternalCallNode *pending_internal_call_end(PendingInternalCallMap *map) { return reinterpret_cast<PendingInternalCallNode *>(&map->root); }

extern "C" void rb_tree_insert_and_rebalance(void *tree, void *parent, void *slot, void *node) {
    (void)tree; auto **slot_ptr = static_cast<InternalCallNode **>(slot); auto *parent_node = static_cast<InternalCallNode *>(parent); auto *new_node = static_cast<InternalCallNode *>(node);
    *slot_ptr = new_node; new_node->left = nullptr; new_node->right = nullptr; new_node->parent = parent_node; new_node->flags = 0;
}
extern "C" int initialize_internal_call_context(void *ctx) { (void)ctx; return 1; }
extern "C" void finalize_internal_call_context(void *ctx, void *arg) { (void)ctx; (void)arg; }

static inline bool string_is_heap(const Il2CppStringKey *s) { return (s->meta & 1) != 0; }
static inline size_t string_length(const Il2CppStringKey *s) { return string_is_heap(s) ? s->size : s->meta >> 1; }
static inline char *string_data(Il2CppStringKey *s) { return string_is_heap(s) ? s->data : reinterpret_cast<char *>(s) + 1; }
static inline const char *string_data_const(const Il2CppStringKey *s) { return string_is_heap(s) ? s->data : reinterpret_cast<const char *>(s) + 1; }
static inline void free_string(Il2CppStringKey *s) { if (string_is_heap(s)) free(s->data); s->meta = 0; s->size = 0; s->data = nullptr; }

extern "C" void throw_length_error(void *) { std::abort(); }
extern "C" void throw_out_of_range(void *) { std::abort(); }

extern "C" void *allocate_or_throw(size_t size) {
    size_t real_size = size ? size : 1;
    for (;;) { void *p = malloc(real_size); if (p) return p; std::new_handler handler = std::get_new_handler(); if (!handler) break; handler(); }
    throw std::bad_alloc();
}

extern "C" void build_string_from_cstr(Il2CppStringKey *dst, const char *src) {
    size_t len = strlen(src);
    if (len >= 0xFFFFFFFFFFFFFFF0ULL) throw_length_error(dst);
    char *buf;
    if (len > 0x16) { uint64_t cap = len | 0xFULL; buf = static_cast<char *>(allocate_or_throw(cap + 1)); dst->meta = cap + 2; dst->size = len; dst->data = buf;
    } else { dst->meta = len * 2; dst->size = 0; dst->data = nullptr; buf = reinterpret_cast<char *>(dst) + 1; }
    memmove(buf, src, len); buf[len] = 0;
}

extern "C" int compare_memory_with_length(const void *a, size_t a_len, const void *b, size_t b_len) {
    size_t n = a_len < b_len ? a_len : b_len;
    if (n) { int r = memcmp(a, b, n); if (r) return r; }
    if (a_len == b_len) return 0;
    return a_len < b_len ? -1 : 1;
}

extern "C" int compare_string_keys(const Il2CppStringKey *a, const Il2CppStringKey *b) {
    return compare_memory_with_length(string_data_const(a), string_length(a), string_data_const(b), string_length(b));
}

extern "C" InternalCallNode *find_internal_call_node(InternalCallMap *map, const Il2CppStringKey *name) {
    InternalCallNode *end = internal_call_end(map); InternalCallNode *cur = map->root; if (!cur) return end; InternalCallNode *candidate = end;
    while (cur) { int cmp = compare_string_keys(&cur->name, name); if (cmp >= 0) { candidate = cur; cur = cur->left; } else { cur = cur->right; } }
    if (candidate == end) return end;
    if (compare_string_keys(name, &candidate->name) < 0) return end;
    return candidate;
}

extern "C" PendingInternalCallNode *find_pending_internal_call_node(PendingInternalCallMap *map, const Il2CppStringKey *name) {
    PendingInternalCallNode *end = pending_internal_call_end(map); PendingInternalCallNode *cur = map->root; if (!cur) return end; PendingInternalCallNode *candidate = end;
    while (cur) { int cmp = compare_string_keys(&cur->name, name); if (cmp >= 0) { candidate = cur; cur = cur->left; } else { cur = cur->right; } }
    if (candidate == end) return end;
    if (compare_string_keys(name, &candidate->name) < 0) return end;
    return candidate;
}

extern "C" InternalCallNode **find_internal_call_insert_slot(InternalCallMap *map, InternalCallNode **parent_out, const Il2CppStringKey *name) {
    InternalCallNode **slot = &map->root; InternalCallNode *cur = map->root; InternalCallNode *parent = internal_call_end(map);
    while (cur) {
        parent = cur; int cmp_left = compare_string_keys(name, &cur->name);
        if (cmp_left < 0) { slot = &cur->left; cur = cur->left; continue; }
        int cmp_right = compare_string_keys(&cur->name, name);
        if (cmp_right < 0) { slot = &cur->right; cur = cur->right; continue; }
        *parent_out = cur; return slot;
    }
    *parent_out = parent; return slot;
}

extern "C" InternalCallNode *get_or_insert_internal_call_node(InternalCallMap *map, const Il2CppStringKey *name, Il2CppStringKey **name_to_move) {
    InternalCallNode *parent = nullptr; InternalCallNode **slot = find_internal_call_insert_slot(map, &parent, name); InternalCallNode *node = *slot;
    if (!node) {
        node = static_cast<InternalCallNode *>(allocate_or_throw(sizeof(InternalCallNode))); Il2CppStringKey *src = *name_to_move;
        node->left = nullptr; node->right = nullptr; node->parent = nullptr; node->flags = 0;
        node->name.meta = src->meta; node->name.size = src->size; node->name.data = src->data;
        src->meta = 0; src->size = 0; src->data = nullptr; node->method = 0; rb_tree_insert_and_rebalance(map, parent, slot, node);
    }
    return node;
}

extern "C" int64_t string_find_char(const Il2CppStringKey *str, unsigned char ch, uint64_t start) {
    size_t len = string_length(str); if (len <= start) return -1; const char *data = string_data_const(str);
    const void *p = memchr(data + start, ch, len - start); if (!p) return -1; return static_cast<const char *>(p) - data;
}

extern "C" void string_substr(Il2CppStringKey *dst, const Il2CppStringKey *src, uint64_t start, size_t count) {
    size_t src_len = string_length(src); if (start > src_len) throw_out_of_range(dst);
    size_t available = src_len - start; size_t len = available >= count ? count : available; if (len >= 0xFFFFFFFFFFFFFFF0ULL) throw_length_error(dst);
    char *buf;
    if (len > 0x16) { uint64_t cap = len | 0xFULL; buf = static_cast<char *>(allocate_or_throw(cap + 1)); dst->meta = cap + 2; dst->size = len; dst->data = buf;
    } else { dst->meta = len * 2; dst->size = 0; dst->data = nullptr; buf = reinterpret_cast<char *>(dst) + 1; }
    const char *src_data = string_data_const(src); memmove(buf, src_data + start, len); buf[len] = 0;
}

extern "C" int il2cpp_add_internal_call_impl(char *name, uint64_t method) {
    if (!name) return -1;
    Il2CppStringKey key1{}; build_string_from_cstr(&key1, name); PendingInternalCallNode *pending = find_pending_internal_call_node(&g_pendingInternalCalls, &key1); free_string(&key1);
    if (pending != pending_internal_call_end(&g_pendingInternalCalls)) { if (pending->target_slot) *pending->target_slot = method; method = pending->replacement; }
    Il2CppStringKey key2{}; build_string_from_cstr(&key2, name); Il2CppStringKey *insert_name = &key2;
    InternalCallNode *node = get_or_insert_internal_call_node(&g_internalCalls, &key2, &insert_name); node->method = method; free_string(&key2);
    return 0;
}

extern "C" int il2cpp_resolve_icall_impl(char *name, uint64_t *out_method) {
    if (!name || !out_method) return -1;
    Il2CppStringKey key{}; build_string_from_cstr(&key, name); InternalCallNode *node = find_internal_call_node(&g_internalCalls, &key); free_string(&key);
    if (node != internal_call_end(&g_internalCalls)) { *out_method = node->method; return 0; }
    Il2CppStringKey full{}; build_string_from_cstr(&full, name); int64_t paren_pos = string_find_char(&full, '(', 0);
    if (paren_pos == -1) { free_string(&full); return -1; }
    Il2CppStringKey short_name{}; string_substr(&short_name, &full, 0, static_cast<size_t>(paren_pos)); free_string(&full);
    node = find_internal_call_node(&g_internalCalls, &short_name);
    if (node == internal_call_end(&g_internalCalls)) { free_string(&short_name); return -1; }
    *out_method = node->method; free_string(&short_name); return 0;
}

// =====================================================================
// 🔥 СИСТЕМНЫЕ ПЕРЕМЕННЫЕ ЧИТА 🔥
// =====================================================================
JavaVM* g_JVM = nullptr; 
jobject g_Activity = nullptr; 
jobject g_SurfaceView = nullptr; 
bool g_SurfaceReady = false; 
pid_t g_Pid = 0; 
int g_ScreenW = 0, g_ScreenH = 0;
uintptr_t g_Il2CppBase_RXP = 0;
void* g_StringClass = nullptr; 
void* g_LocalPlayerManager = nullptr; 

// ======================== ОФФСЕТЫ ========================
#define RVA_UPDATE oxorany((uintptr_t)0x9032A18) 
#define RVA_RAYCAST oxorany((uintptr_t)0x9D8DA30)  
#define RVA_ON_SCREEN_MOUSE_TYPEINFO oxorany((uintptr_t)0xAC51C10) // <-- Прямой оффсет TypeInfo!

#define RVA_GET_CAM_COUNT oxorany((uintptr_t)0x9C56068)
#define RVA_GET_MAIN_CAM oxorany((uintptr_t)0x9C54E38)
#define RVA_FIND_OF_TYPE oxorany((uintptr_t)0x9CDF0D0)
#define RVA_GO_GET_TRANS oxorany((uintptr_t)0x9CD71F4)
#define RVA_COMP_GET_TRANS oxorany((uintptr_t)0x9CD21AC)
#define RVA_GET_POS oxorany((uintptr_t)0x9CE980C)
#define RVA_GET_TYPE oxorany((uintptr_t)0xA314D94)
#define RVA_GET_BONE oxorany((uintptr_t)0x9C2C3B4)
#define RVA_GET_NAME oxorany((uintptr_t)0x9CDDE54)
#define RVA_INPUT_MOUSEPOS oxorany((uintptr_t)0x9D5E30C)
#define RVA_INPUT_MOUSEBTN oxorany((uintptr_t)0x9D5E2E8)
#define RVA_LINECAST oxorany((uintptr_t)0x9D862C0) 
#define RVA_SET_FPS oxorany((uintptr_t)0x9C4A8F8)
#define RVA_SET_NEARCLIP oxorany((uintptr_t)0x9C4DA60)

#define OFF_CACHED_PTR oxorany((uintptr_t)0x10)
#define OFF_ARRAY_LEN oxorany((uintptr_t)0x18)
#define OFF_ARRAY_DATA oxorany((uintptr_t)0x20)
#define OFF_STRING_LEN oxorany((uintptr_t)0x10)
#define OFF_STRING_CHARS oxorany((uintptr_t)0x14)
#define OFF_MAT_VIEW oxorany((uintptr_t)0xF0)
#define OFF_FOV oxorany((uintptr_t)0x1F0)
#define OFF_IS_FRIEND oxorany((uintptr_t)0x90)
#define OFF_IS_TEAM oxorany((uintptr_t)0x91)
#define OFF_CLAN_STR oxorany((uintptr_t)0xA8)
#define OFF_NAME_STR oxorany((uintptr_t)0xB0)
#define OFF_NICK_LABEL oxorany((uintptr_t)0x130)
#define OFF_ANIMATOR oxorany((uintptr_t)0x180)
#define OFF_MOUSE_LOOK oxorany((uintptr_t)0x70)
#define OFF_ANGLES oxorany((uintptr_t)0x60)
#define OFF_LOOT_CAPACITY oxorany((uintptr_t)0xC8)
#define OFF_JUMP oxorany((uintptr_t)0x58)
#define OFF_SPRINT_FLOAT oxorany((uintptr_t)0x44) 
#define OFF_SPRINT oxorany((uintptr_t)0x59)       
#define OFF_CROUCH oxorany((uintptr_t)0x5A)       
#define OFF_IS_AIMING oxorany((uintptr_t)0x5B) 
#define OFF_SIGMA oxorany((uintptr_t)0x60)        

#define S_PM oxorany("Oxide.PlayerManager, Assembly-CSharp")
#define S_LOOT oxorany("Oxide.LootObject, Assembly-CSharp")
#define S_PICKUP oxorany("Oxide.ItemPickup, Assembly-CSharp")
#define S_INPUT oxorany("Oxide.PlayerInputHandler, Assembly-CSharp") 
#define S_ORE oxorany("Oxide.MineableStone, Assembly-CSharp") 
#define S_SCRAP oxorany("Oxide.LootDestroyable, Assembly-CSharp") 
#define S_TREE oxorany("Oxide.MineableTree, Assembly-CSharp")
#define S_CUPBOARD oxorany("Oxide.Cupboard, Assembly-CSharp")
#define S_SLEEPER oxorany("HyperHug.Games.Oxide.Features.Sleepers.DisconnectedSleeper, Assembly-CSharp")

#define MAX_L 150
#define MAX_P 150
#define MAX_PL 100
#define MAX_O 150 
#define MAX_S 150 
#define MAX_T 150 
#define MAX_C 150
#define MAX_SL 150

// ======================== ДАННЫЕ ========================
struct LData { void* o; void* on; void* ok; Vec3 p; char n[32]; Vec3 wp; };
struct PData { void* o; void* on; void* ok; Vec3 p; char n[64]; Vec3 wp; };
struct OData { void* o; void* on; void* ok; Vec3 p; char n[64]; Vec3 wp; int type; }; 
struct SData { void* o; void* on; void* ok; Vec3 p; Vec3 wp; };
struct TData { void* o; void* on; void* ok; Vec3 p; Vec3 wp; };
struct CData { void* o; void* on; void* ok; Vec3 p; Vec3 wp; };
struct SlData { void* o; void* on; void* ok; Vec3 p; char n[64]; Vec3 wp; };
struct PlData { 
    void* pm; void* pmn; void* pmk; void* t; void* tn; void* tk; 
    void* hb; void* hbn; void* hbk; void* bb; void* bbn; void* bbk; 
    char n[64]; char c[64]; char espName[128]; bool iT; bool iF; 
    Vec3 wF, wH, wPred; bool validForESP, vF, vH, vPred, hB, isVis; float di; 
    Vec3 lastPos, dir; 
};

typedef int(*g_cc_t)(void* m); typedef void*(*g_mc_t)(void* m); typedef void*(*f_obj_t)(void* t, void* m); typedef void*(*g_tr_t)(void* c, void* m); 
typedef Vec3(*g_pos_t)(void* t, void* m); typedef void*(*g_type_t)(void* n, void* m); typedef void*(*g_bone_t)(void* a, int b, void* m); 
typedef void*(*g_name_t)(void* o, void* m); typedef Vec3(*g_mpos_t)(void* m); typedef bool(*g_mbtn_t)(int b, void* m);
typedef bool(*physics_linecast_t)(Vec3 start, Vec3 end, int layerMask, void* method);
typedef void(*set_fps_t)(int);

// ======================== MAGIC BULLET (DOBBY INLINE HOOK) ========================

using Ray = struct { Vec3 m_orig; Vec3 m_dir; };
using RaycastHit = struct { Vec3 m_point; Vec3 m_normal; uint32_t m_face; float m_distance; float m_uv[2]; int m_collider; };
using InternalRaycastFn = bool(*)(void* scene, Ray* ray, float distance, RaycastHit* hit, int layer, int query);

InternalRaycastFn orig_Raycast = nullptr;
std::atomic<int> g_RaycastCallCount(0);

bool g_MB_HasTarget = false;
Vec3 g_MB_TargetPos = {0,0,0};
void* g_PInputObj = nullptr;

bool hk_Raycast(void* scene, Ray* ray, float distance, RaycastHit* hit, int layer, int query) {
    g_RaycastCallCount++;
    extern bool cfg_magic_bullet;
    if (cfg_magic_bullet && g_MB_HasTarget && distance > 50.0f) {
        bool isShooting = false;
        if (g_PInputObj) { FastRead((void*)((uintptr_t)g_PInputObj + OFF_SIGMA), &isShooting); }
        if (isShooting) {
            Vec3 target = g_MB_TargetPos;
            Vec3 dir = target - ray->m_orig;
            float dist = sqrtf(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
            if (dist > 0.001f) { ray->m_dir = {dir.x / dist, dir.y / dist, dir.z / dist}; }
        }
    }
    return orig_Raycast(scene, ray, distance, hit, layer, query);
}

// AUTH GLOBALS
bool g_Authenticated = false;
uint64_t g_AuthValidationHash = 0; 
char g_AuthKey[64] = "";
std::string g_AuthStatusMsg = "";
std::string g_AuthDaysLeft = "0";

// НАСТРОЙКИ
bool g_IsMenuMinimized = false; ImVec2 g_MinimizedPos = ImVec2(100, 100); float g_MenuScale = 1.0f; float g_TempMenuScale = 1.0f; double g_LastScaleEditTime = 0.0; bool g_IsScrolling = false; 
bool cfg_esp_enable=true, cfg_esp_line=true, cfg_esp_box=true, cfg_esp_name=true, cfg_esp_clan=true, cfg_esp_dist=true, cfg_esp_count=true;
bool cfg_loot_enable=false, cfg_pickup_enable=false, cfg_ore_enable=false, cfg_scrap_enable=false, cfg_tree_enable=false, cfg_cupboard_enable=false, cfg_sleeper_enable=false;
bool cfg_aim_enable=true, cfg_aim_vis_check=true, cfg_aim_draw_fov=true, cfg_aim_scope_only=true;
float cfg_aim_fov=120.0f, cfg_aim_max_dist=250.0f; 
float cfg_aim_smooth = 1.5f; 
int cfg_aim_bone=0, cfg_line_pos=0;

// MAGIC BULLET НАСТРОЙКИ
bool cfg_magic_bullet=false, cfg_mb_draw_fov=true, cfg_mb_draw_line=true; float cfg_mb_fov=150.0f; 

// WEAPON MODS НАСТРОЙКИ
bool cfg_fast_heal=false;
bool cfg_no_recoil=false, cfg_fast_reload=false, cfg_fast_shoot=false;

bool cfg_unlock_fps=false, cfg_xray_enable=false; float cfg_xray_dist=2.0f;
bool cfg_auto_farm=false, cfg_farm_stone=true, cfg_farm_ferum=true, cfg_farm_sulfur=true, cfg_farm_scrap=true, cfg_farm_tree=true; 
bool farm_sprint=false, farm_attack=false, farm_crouch=false; 

// КЭШ И ГЛОБАЛЫ ДЛЯ ESP
static uint64_t g_FCnt = 0; static bool s_BFCap = false; static float s_BFov = 0.0f; bool g_IsScoped = false;
static LData g_LCache[MAX_L]; static int g_LCount=0; static PData g_PCache[MAX_P]; static int g_PCount=0; static OData g_OCache[MAX_O]; static int g_OCount=0; static SData g_SCache[MAX_S]; static int g_SCount=0; static TData g_TCache[MAX_T]; static int g_TCount=0; static PlData g_PlCache[MAX_PL]; static int g_PlCount=0; static CData g_CCache[MAX_C]; static int g_CCount=0; static SlData g_SlCache[MAX_SL]; static int g_SlCount=0;
std::mutex g_PlMtx, g_LMtx, g_PMtx, g_OMtx, g_SMtx, g_TMtx, g_CMtx, g_SlMtx;
static void* g_TPMgr=nullptr; static void* g_TLObj=nullptr; static void* g_TPObj=nullptr; static void* g_TInput=nullptr; static void* g_TOre=nullptr; static void* g_TScrap=nullptr; static void* g_TTree=nullptr; static void* g_TCupboard=nullptr; static void* g_TSleeper=nullptr;
static void* g_CCamTr = nullptr; static void* g_CCamTrN = nullptr; static void* g_CCamTrK = nullptr; static void* g_CNatCam = nullptr;
GLuint g_LogoTex = 0; int g_LogoW = 0, g_LogoH = 0, g_VisibleEnemies = 0;
Mat4x4 g_vM; Vec3 g_cP;
volatile int g_RenderCount = 0; 

void UpdCam(); void UpdPl(); void UpdLoot(); void UpdPickup(); void UpdInput(); void UpdOre(); void UpdScrap(); void UpdTree(); void UpdCupboard(); void UpdSleeper(); void InitCache();

// ======================== IMGUI УТИЛИТЫ ========================
void DrwTxt(ImDrawList* d, const char* t, float x, float y, ImU32 c, float s) { ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(s, FLT_MAX, 0.0f, t); float px = x - ts.x / 2.0f; d->AddText(ImGui::GetFont(), s, ImVec2(px + 1, y + 1), IM_COL32(0,0,0,255), t); d->AddText(ImGui::GetFont(), s, ImVec2(px, y), c, t); }
bool DPanel(const char* l, const char* d, bool* v) { ImGuiWindow* w=ImGui::GetCurrentWindow(); if(w->SkipItems)return false; ImGuiContext& g=*GImGui; ImGuiID id=w->GetID(l); ImVec2 p=w->DC.CursorPos; ImVec2 s(ImGui::GetContentRegionAvail().x,65.0f*g_MenuScale); ImRect b(p,ImVec2(p.x+s.x,p.y+s.y)); ImGui::ItemSize(b,g.Style.FramePadding.y); if(!ImGui::ItemAdd(b,id))return false; bool hv,hl; bool pr=ImGui::ButtonBehavior(b,id,&hv,&hl); if(g_IsScrolling)pr=false; if(pr)*v=!*v; float da=ImClamp(g.IO.DeltaTime*14.0f,0.0f,1.0f); float dh=ImClamp(g.IO.DeltaTime*12.0f,0.0f,1.0f); float* av=w->StateStorage.GetFloatRef(id,0.0f); *av=ImLerp(*av,*v?1.0f:0.0f,da); float* hvv=w->StateStorage.GetFloatRef(id+1,0.0f); *hvv=ImLerp(*hvv,(hv&&!g_IsScrolling)?1.0f:0.0f,dh); ImU32 bg=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.12f,0.12f,0.12f,1.0f),ImVec4(0.17f,0.17f,0.17f,1.0f),*hvv)); w->DrawList->AddRectFilled(b.Min,b.Max,bg,10.0f*g_MenuScale); w->DrawList->AddText(ImGui::GetFont(),24.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+12*g_MenuScale),IM_COL32(240,240,240,255),l); if(d)w->DrawList->AddText(ImGui::GetFont(),18.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+38*g_MenuScale),IM_COL32(140,140,140,255),d); float sw=44.0f*g_MenuScale, sh=24.0f*g_MenuScale; ImVec2 sp(b.Max.x-sw-15*g_MenuScale,b.Min.y+(s.y-sh)/2.0f); ImU32 sbg=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.3f,0.3f,0.3f,1.0f),ImVec4(1.0f,1.0f,1.0f,1.0f),*av)); w->DrawList->AddRectFilled(sp,ImVec2(sp.x+sw,sp.y+sh),sbg,sh/2.0f); ImU32 cc=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.9f,0.9f,0.9f,1.0f),ImVec4(0.1f,0.1f,0.1f,1.0f),*av)); float cx=ImLerp(sp.x+sh/2.0f,sp.x+sw-sh/2.0f,*av); w->DrawList->AddCircleFilled(ImVec2(cx,sp.y+sh/2.0f),sh/2.0f-3.0f*g_MenuScale,cc); return pr; }
bool DSlider(const char* l, const char* d, float* v, float mn, float mx, const char* f) { ImGuiWindow* w=ImGui::GetCurrentWindow(); if(w->SkipItems)return false; ImGuiContext& g=*GImGui; ImGuiID id=w->GetID(l); ImVec2 p=w->DC.CursorPos; ImVec2 s(ImGui::GetContentRegionAvail().x,65.0f*g_MenuScale); ImRect b(p,ImVec2(p.x+s.x,p.y+s.y)); ImGui::ItemSize(b,g.Style.FramePadding.y); if(!ImGui::ItemAdd(b,id))return false; bool hv,hl; ImGui::ButtonBehavior(b,id,&hv,&hl); if(hl&&!g_IsScrolling){ float t=ImClamp((g.IO.MousePos.x-b.Min.x)/s.x,0.0f,1.0f); *v=mn+t*(mx-mn); } float t=(*v-mn)/(mx-mn); float dh=ImClamp(g.IO.DeltaTime*12.0f,0.0f,1.0f); float* hvv=w->StateStorage.GetFloatRef(id+1,0.0f); *hvv=ImLerp(*hvv,(hv&&!g_IsScrolling)?1.0f:0.0f,dh); ImU32 bg=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.12f,0.12f,0.12f,1.0f),ImVec4(0.17f,0.17f,0.17f,1.0f),*hvv)); w->DrawList->AddRectFilled(b.Min,b.Max,bg,10.0f*g_MenuScale); ImU32 fc=IM_COL32(70,70,70,255); ImVec2 fm(b.Min.x+s.x*t,b.Max.y); w->DrawList->AddRectFilled(b.Min,fm,fc,10.0f*g_MenuScale,t<0.99f?ImDrawFlags_RoundCornersLeft:ImDrawFlags_RoundCornersAll); w->DrawList->AddText(ImGui::GetFont(),24.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+12*g_MenuScale),IM_COL32(240,240,240,255),l); if(d)w->DrawList->AddText(ImGui::GetFont(),18.0f*g_MenuScale,ImVec2(b.Min.x+15*g_MenuScale,b.Min.y+38*g_MenuScale),IM_COL32(180,180,180,255),d); char vb[64]; snprintf(vb,sizeof(vb),f,*v); ImVec2 vsz=ImGui::GetFont()->CalcTextSizeA(20.0f*g_MenuScale,FLT_MAX,0.0f,vb); w->DrawList->AddText(ImGui::GetFont(),20.0f*g_MenuScale,ImVec2(b.Max.x-vsz.x-15*g_MenuScale,b.Min.y+22*g_MenuScale),IM_COL32(255,255,255,255),vb); return hl&&!g_IsScrolling; }

void LoadLogoTexture() { if (g_LogoTex != 0) return; int c; unsigned char* d = stbi_load_from_memory(logo_data, logo_len, &g_LogoW, &g_LogoH, &c, 4); if (d) { glGenTextures(1, &g_LogoTex); glBindTexture(GL_TEXTURE_2D, g_LogoTex); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_LogoW, g_LogoH, 0, GL_RGBA, GL_UNSIGNED_BYTE, d); stbi_image_free(d); } }

// ======================== АВТОРИЗАЦИЯ И ЛОГИКА ========================
void PerformAuth(std::string key) {
    JNIEnv* env; int getEnvStat = g_JVM->GetEnv((void**)&env, JNI_VERSION_1_6); bool didAttach = false;
    if (getEnvStat == JNI_EDETACHED) { if (g_JVM->AttachCurrentThread(&env, NULL) != 0) { g_AuthStatusMsg = oxorany("JNI Attach failed!"); return; } didAttach = true; }
    std::string hwid = JNI_GetHWID(env); std::string url = std::string(oxorany("https://volgavpn.qzz.io/?key=")) + key + std::string(oxorany("&hwid=")) + hwid; std::string resp = JNI_HttpGet(env, url); if (didAttach) g_JVM->DetachCurrentThread();
    if (resp.empty()) { g_AuthStatusMsg = oxorany("Connection error!"); return; }
    std::string status = ExtractJSONValue(resp, oxorany("status"));
    if (status == oxorany("success")) {
        std::string sig = ExtractJSONValue(resp, oxorany("signature")); std::string days = ExtractJSONValue(resp, oxorany("days_left")); std::string expected_sig = Crypto::hmac_sha256(oxorany("TetoIsTheBestBeaverInTheWorld"), key);
        if (sig == expected_sig) { g_AuthDaysLeft = days; g_Authenticated = true; g_AuthValidationHash = 0x41BA0EA2; g_AuthStatusMsg = ""; } else { g_AuthStatusMsg = oxorany("Security validation failed!"); }
    } else { std::string msg = ExtractJSONValue(resp, oxorany("message")); g_AuthStatusMsg = msg.empty() ? oxorany("Unknown error") : msg; }
}

void DrawAuthMenu() {
    ImGui::SetNextWindowSize(ImVec2(400 * g_MenuScale, 220 * g_MenuScale), ImGuiCond_Always); ImGui::SetNextWindowPos(ImVec2(g_ScreenW / 2.0f, g_ScreenH / 2.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f)); ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f)); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * g_MenuScale); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f); ImGui::Begin("Auth", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings); ImDrawList* d = ImGui::GetWindowDrawList(); ImVec2 p = ImGui::GetWindowPos(); d->AddRectFilled(p, ImVec2(p.x + 400 * g_MenuScale, p.y + 60 * g_MenuScale), IM_COL32(25, 25, 25, 255), 12.0f * g_MenuScale, ImDrawFlags_RoundCornersTop);
    if (g_LogoTex) { d->AddImage((void*)(intptr_t)g_LogoTex, ImVec2(p.x + 15 * g_MenuScale, p.y + 10 * g_MenuScale), ImVec2(p.x + 55 * g_MenuScale, p.y + 50 * g_MenuScale)); } d->AddText(ImGui::GetFont(), 28.0f * g_MenuScale, ImVec2(p.x + 70 * g_MenuScale, p.y + 16 * g_MenuScale), IM_COL32(240, 240, 240, 255), "Authorization"); ImGui::SetCursorPos(ImVec2(40 * g_MenuScale, 90 * g_MenuScale)); ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.12f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.17f, 0.17f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("Paste & Login", ImVec2(320 * g_MenuScale, 60 * g_MenuScale))) { std::string clip = JNI_GetClipboard(); if (!clip.empty()) { strncpy(g_AuthKey, clip.c_str(), sizeof(g_AuthKey) - 1); g_AuthKey[sizeof(g_AuthKey) - 1] = '\0'; g_AuthStatusMsg = "Wait!"; std::string k(g_AuthKey); std::thread([k]() { PerformAuth(k); }).detach(); } else { g_AuthStatusMsg = "Clipboard is empty!"; } } ImGui::PopStyleColor(3);
    if (!g_AuthStatusMsg.empty()) { float msgW = ImGui::GetFont()->CalcTextSizeA(22.0f * g_MenuScale, FLT_MAX, 0.0f, g_AuthStatusMsg.c_str()).x; ImGui::SetCursorPos(ImVec2((400 * g_MenuScale - msgW) / 2.0f, 175 * g_MenuScale)); ImU32 col = (g_AuthStatusMsg.find("Connecting") != std::string::npos) ? IM_COL32(180, 180, 180, 255) : IM_COL32(255, 100, 100, 255); d->AddText(ImGui::GetFont(), 22.0f * g_MenuScale, ImGui::GetCursorScreenPos(), col, g_AuthStatusMsg.c_str()); } ImGui::End(); ImGui::PopStyleVar(3); ImGui::PopStyleColor();
}

void DrawMenu() {
    if (g_IsMenuMinimized) { ImGui::SetNextWindowPos(g_MinimizedPos,ImGuiCond_FirstUseEver); ImGui::SetNextWindowSize(ImVec2(70*g_MenuScale,70*g_MenuScale)); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0f); ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0,0,0,0)); ImGui::Begin("F",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar); ImVec2 p=ImGui::GetWindowPos(); g_MinimizedPos=p; if(g_LogoTex){ImGui::GetWindowDrawList()->AddImage((void*)(intptr_t)g_LogoTex,p,ImVec2(p.x+70*g_MenuScale,p.y+70*g_MenuScale)); if(ImGui::InvisibleButton("O",ImVec2(70*g_MenuScale,70*g_MenuScale)))g_IsMenuMinimized=false;} ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2); return; }
    static int c_t=0; static float t_a[4]={1.0f,0.0f,0.0f,0.0f}; ImGui::SetNextWindowSize(ImVec2(800*g_MenuScale,520*g_MenuScale),ImGuiCond_Always); ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(0.08f,0.08f,0.08f,1.0f)); ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,12.0f*g_MenuScale); ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.0f); ImGui::Begin("M",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoSavedSettings); ImDrawList* d=ImGui::GetWindowDrawList(); ImVec2 p=ImGui::GetWindowPos(); ImVec2 s=ImGui::GetWindowSize(); d->AddRectFilled(p,ImVec2(p.x+220*g_MenuScale,p.y+s.y),IM_COL32(25,25,25,255),12.0f*g_MenuScale,ImDrawFlags_RoundCornersLeft); ImVec2 lm=ImVec2(p.x+35*g_MenuScale,p.y+30*g_MenuScale); if(g_LogoTex){d->AddImage((void*)(intptr_t)g_LogoTex,lm,ImVec2(lm.x+150*g_MenuScale,lm.y+150*g_MenuScale)); ImGui::SetCursorPos(ImVec2(35*g_MenuScale,30*g_MenuScale)); if(ImGui::InvisibleButton("H",ImVec2(150*g_MenuScale,150*g_MenuScale)))g_IsMenuMinimized=true;}
    char subBuf[64]; snprintf(subBuf, sizeof(subBuf), "%s Days", g_AuthDaysLeft.c_str()); ImGui::SetCursorPos(ImVec2(35*g_MenuScale, 190*g_MenuScale)); ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", subBuf);
    const char* tb[]={"Visuals","Aim","Magic","Misc"}; ImGui::SetCursorPos(ImVec2(0,220*g_MenuScale)); ImGui::BeginChild("T",ImVec2(220*g_MenuScale,s.y-220*g_MenuScale),false,ImGuiWindowFlags_NoScrollbar); for(int i=0;i<4;i++){ImGui::SetCursorPosX(15*g_MenuScale); bool sel=(c_t==i); ImVec2 cp=ImGui::GetCursorScreenPos(); if(ImGui::InvisibleButton(tb[i],ImVec2(190*g_MenuScale,50*g_MenuScale)))c_t=i; float da=ImClamp(ImGui::GetIO().DeltaTime*14.0f,0.0f,1.0f); t_a[i]=ImLerp(t_a[i],sel?1.0f:0.0f,da); ImU32 bc=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0,0,0,0),ImVec4(1,1,1,0.1f),t_a[i])); ImGui::GetWindowDrawList()->AddRectFilled(cp,ImVec2(cp.x+190*g_MenuScale,cp.y+50*g_MenuScale),bc,8.0f*g_MenuScale); if(t_a[i]>0.01f)ImGui::GetWindowDrawList()->AddRectFilled(cp,ImVec2(cp.x+4*g_MenuScale,cp.y+50*g_MenuScale),IM_COL32(255,255,255,(int)(255*t_a[i])),4.0f*g_MenuScale); ImU32 tc=ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(0.5f,0.5f,0.5f,1.0f),ImVec4(1,1,1,1),t_a[i])); ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),22.0f*g_MenuScale,ImVec2(cp.x+20*g_MenuScale,cp.y+14*g_MenuScale),tc,tb[i]); ImGui::SetCursorPosY(ImGui::GetCursorPosY()+5*g_MenuScale);} ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(240*g_MenuScale,20*g_MenuScale)); ImGui::BeginChild("C",ImVec2(s.x-260*g_MenuScale,s.y-40*g_MenuScale),false,ImGuiWindowFlags_NoScrollbar); ImGuiWindow* w=ImGui::GetCurrentWindow(); bool ih=ImGui::IsMouseHoveringRect(w->Pos,ImVec2(w->Pos.x+w->Size.x,w->Pos.y+w->Size.y)); static ImVec2 dsp; static float ssy; static bool ita=false; if(ih&&ImGui::IsMouseClicked(0)){dsp=ImGui::GetIO().MousePos; ssy=w->Scroll.y; ita=true; g_IsScrolling=false;} if(ita){if(ImGui::IsMouseDown(0)){float dy=ImGui::GetIO().MousePos.y-dsp.y; if(std::abs(dy)>5.0f*g_MenuScale){g_IsScrolling=true; w->Scroll.y=ssy-dy;}}else{ita=false; g_IsScrolling=false;}} if(w->Scroll.y<0.0f)w->Scroll.y=0.0f; if(w->Scroll.y>w->ScrollMax.y)w->Scroll.y=w->ScrollMax.y; if(ih&&ImGui::GetIO().MouseWheel!=0.0f)w->Scroll.y-=ImGui::GetIO().MouseWheel*30.0f*g_MenuScale; d->AddText(ImGui::GetFont(),32.0f*g_MenuScale,ImGui::GetCursorScreenPos(),IM_COL32(255,255,255,255),tb[c_t]); ImGui::Dummy(ImVec2(0,40*g_MenuScale));
    if(c_t==0){ DPanel("Enable ESP","On/Off",&cfg_esp_enable); DPanel("Box","Draw Box",&cfg_esp_box); DPanel("Line","Draw Line",&cfg_esp_line); DPanel("Name","Draw Name",&cfg_esp_name); DPanel("Clan","Draw Clan",&cfg_esp_clan); DPanel("Distance","Draw Distance",&cfg_esp_dist); DPanel("Count","Draw Count",&cfg_esp_count); DPanel("Lootboxes","Draw Lootboxes",&cfg_loot_enable); DPanel("Pickups","Draw Pickups",&cfg_pickup_enable); DPanel("Ore","Draw Ore",&cfg_ore_enable); DPanel("Trees","Draw Trees",&cfg_tree_enable); DPanel("Scrap","Draw Scrap",&cfg_scrap_enable); DPanel("Cupboard","Draw Cupboard",&cfg_cupboard_enable); DPanel("Sleepers","Draw sleeping players",&cfg_sleeper_enable); }
    else if(c_t==1){ DPanel("Enable Aim","Auto aim",&cfg_aim_enable); DPanel("Vis Check","Aim at visible only",&cfg_aim_vis_check); DPanel("Scope Only","Check Scope",&cfg_aim_scope_only); DSlider("Smooth", "Aim smoothing", &cfg_aim_smooth, 1.5f, 2.0f, "%.1f"); DSlider("Max Dist","Aim distance limit",&cfg_aim_max_dist,50.0f,300.0f,"%.0f m"); DPanel("FOV Circle","Draw FOV",&cfg_aim_draw_fov); DSlider("FOV Size","Circle Size",&cfg_aim_fov,10.0f,360.0f,"%.0f px"); }
    else if(c_t==2){ DPanel("Magic Bullet","Silent Aim", &cfg_magic_bullet); DPanel("MB FOV Circle","Red Circle", &cfg_mb_draw_fov); DPanel("MB Target Line","Red line to target", &cfg_mb_draw_line); DSlider("MB FOV Size", "Magic Bullet FOV", &cfg_mb_fov, 10.0f, 360.0f, "%.0f px"); ImGui::Dummy(ImVec2(0, 10*g_MenuScale)); DPanel("No Recoil & Spread","Remove weapon recoil/spread",&cfg_no_recoil); DPanel("Fast Reload","Instant reload",&cfg_fast_reload); DPanel("Rapid Fire","Shoot very fast",&cfg_fast_shoot); ImGui::Dummy(ImVec2(0, 10*g_MenuScale)); DPanel("Fast Heal","Auto medkit",&cfg_fast_heal); DPanel("Auto Farm", "Enable Auto Farm", &cfg_auto_farm); DPanel(" > Farm Stone", "Target Stone", &cfg_farm_stone); DPanel(" > Farm Ferum", "Target Ferum", &cfg_farm_ferum); DPanel(" > Farm Sulfur", "Target Sulfur", &cfg_farm_sulfur); DPanel(" > Farm Scrap", "Target Scrap", &cfg_farm_scrap); DPanel(" > Farm Tree", "Target Trees", &cfg_farm_tree); }
    else if(c_t==3){ DPanel("Unlock FPS", "t.me/ManesWare", &cfg_unlock_fps); DPanel("Enable XRAY", "See through walls", &cfg_xray_enable); if (cfg_xray_enable) { DSlider("X-ray distance", "X-ray distance", &cfg_xray_dist, 2.0f, 20.0f, "%.2f"); } ImGui::Dummy(ImVec2(0, 10*g_MenuScale)); if(DSlider("Menu Scale","Scale UI",&g_TempMenuScale,0.5f,2.0f,"%.2f"))g_LastScaleEditTime=ImGui::GetTime(); ImGui::Dummy(ImVec2(0,20*g_MenuScale)); 
        extern volatile int g_RenderCount; static double last_fps_time = 0.0; static int last_render_count = 0; static int current_fps = 0;
        double current_time = ImGui::GetTime(); if (current_time - last_fps_time >= 1.0) { current_fps = g_RenderCount - last_render_count; last_render_count = g_RenderCount; last_fps_time = current_time; }
        char rtBuf[128]; snprintf(rtBuf, sizeof(rtBuf), "FPS: %d", current_fps); d->AddText(ImGui::GetFont(), 24.0f * g_MenuScale, ImGui::GetCursorScreenPos(), IM_COL32(255, 255, 255, 255), rtBuf);
    }
    ImGui::EndChild(); ImGui::End(); ImGui::PopStyleVar(3); ImGui::PopStyleColor();
}

void DrawESP() {
    ImDrawList* d = ImGui::GetBackgroundDrawList(); float sW = (float)g_ScreenW; float sH = (float)g_ScreenH;
    if (cfg_aim_enable && cfg_aim_draw_fov) d->AddCircle(ImVec2(sW/2, sH/2), cfg_aim_fov, IM_COL32(255,255,255,100), 40, 1.5f);
    if (cfg_magic_bullet && cfg_mb_draw_fov) d->AddCircle(ImVec2(sW/2, sH/2), cfg_mb_fov, IM_COL32(255,50,50,100), 40, 1.5f);
    if (cfg_magic_bullet && cfg_mb_draw_line && g_MB_HasTarget) { Vec3 wMB; if (W2S(g_MB_TargetPos, wMB, g_vM, sW, sH)) { d->AddLine(ImVec2(sW / 2.0f, sH / 2.0f), ImVec2(wMB.x, wMB.y), IM_COL32(255, 50, 50, 200), 2.0f); d->AddCircleFilled(ImVec2(wMB.x, wMB.y), 5.0f, IM_COL32(255, 50, 50, 255)); } }
    float maxRenderDistSq = 62500.0f;
    if (cfg_loot_enable) { std::lock_guard<std::mutex> lk(g_LMtx); for (int i = 0; i < g_LCount; i++) { if (g_cP.DistSq(g_LCache[i].p) > maxRenderDistSq) continue; if (W2S(g_LCache[i].p, g_LCache[i].wp, g_vM, sW, sH)) DrwTxt(d, g_LCache[i].n, g_LCache[i].wp.x, g_LCache[i].wp.y, IM_COL32(255, 0, 255, 255), 25.0f); } }
    if (cfg_pickup_enable) { std::lock_guard<std::mutex> lk(g_PMtx); for (int i = 0; i < g_PCount; i++) { if (g_cP.DistSq(g_PCache[i].p) > maxRenderDistSq) continue; if (W2S(g_PCache[i].p, g_PCache[i].wp, g_vM, sW, sH)) DrwTxt(d, g_PCache[i].n, g_PCache[i].wp.x, g_PCache[i].wp.y, IM_COL32(255, 255, 255, 255), 25.0f); } }
    if (cfg_ore_enable) { std::lock_guard<std::mutex> lk(g_OMtx); for (int i = 0; i < g_OCount; i++) { if (g_cP.DistSq(g_OCache[i].p) > maxRenderDistSq) continue; if (W2S(g_OCache[i].p, g_OCache[i].wp, g_vM, sW, sH)) { ImU32 col = IM_COL32(180, 180, 180, 255); if (g_OCache[i].type == 3) col = IM_COL32(220, 220, 50, 255); else if (g_OCache[i].type == 2) col = IM_COL32(200, 120, 50, 255); DrwTxt(d, g_OCache[i].n, g_OCache[i].wp.x, g_OCache[i].wp.y, col, 25.0f); } } }
    if (cfg_scrap_enable) { std::lock_guard<std::mutex> lk(g_SMtx); for (int i = 0; i < g_SCount; i++) { if (g_cP.DistSq(g_SCache[i].p) > maxRenderDistSq) continue; if (W2S(g_SCache[i].p, g_SCache[i].wp, g_vM, sW, sH)) DrwTxt(d, oxorany("Scrap"), g_SCache[i].wp.x, g_SCache[i].wp.y, IM_COL32(200, 150, 50, 255), 25.0f); } }
    if (cfg_tree_enable) { std::lock_guard<std::mutex> lk(g_TMtx); for (int i = 0; i < g_TCount; i++) { if (g_cP.DistSq(g_TCache[i].p) > maxRenderDistSq) continue; if (W2S(g_TCache[i].p, g_TCache[i].wp, g_vM, sW, sH)) DrwTxt(d, oxorany("Tree"), g_TCache[i].wp.x, g_TCache[i].wp.y, IM_COL32(50, 200, 50, 255), 25.0f); } }
    if (cfg_cupboard_enable) { std::lock_guard<std::mutex> lk(g_CMtx); for (int i = 0; i < g_CCount; i++) { if (g_cP.DistSq(g_CCache[i].p) > maxRenderDistSq) continue; if (W2S(g_CCache[i].p, g_CCache[i].wp, g_vM, sW, sH)) DrwTxt(d, oxorany("Cupboard"), g_CCache[i].wp.x, g_CCache[i].wp.y, IM_COL32(0, 0, 139, 255), 25.0f); } }
    if (cfg_sleeper_enable) { std::lock_guard<std::mutex> lk(g_SlMtx); for (int i = 0; i < g_SlCount; i++) { if (g_cP.DistSq(g_SlCache[i].p) > maxRenderDistSq) continue; if (W2S(g_SlCache[i].p, g_SlCache[i].wp, g_vM, sW, sH)) { char slBuf[128]; snprintf(slBuf, sizeof(slBuf), "Sleeper: %s", g_SlCache[i].n[0] ? g_SlCache[i].n : "Unknown"); DrwTxt(d, slBuf, g_SlCache[i].wp.x, g_SlCache[i].wp.y, IM_COL32(255, 140, 0, 255), 25.0f); } } }

    { 
        std::lock_guard<std::mutex> lk(g_PlMtx);
        for (int i = 0; i < g_PlCount; i++) {
            if (!g_PlCache[i].validForESP) continue;
            if (cfg_esp_enable && g_PlCache[i].vF && g_PlCache[i].vH) { 
                Vec3 wF = g_PlCache[i].wF, wH = g_PlCache[i].wH; if (g_PlCache[i].hB) wH.y += (wH.y - wF.y) * 0.15f; 
                float h = abs(wH.y - wF.y), w = h / 2.0f, x = wF.x, y = wF.y, hy = wH.y; ImU32 bC = IM_COL32(200, 150, 0, 255); 
                if (g_PlCache[i].iT || g_PlCache[i].iF) bC = IM_COL32(0, 255, 0, 255); else if (g_PlCache[i].isVis) bC = IM_COL32(255, 0, 0, 255); 
                if (cfg_esp_box) d->AddRect(ImVec2(x - w / 2, hy), ImVec2(x + w / 2, y), bC, 0, 0, 2.0f); 
                float fS = 25.0f; if (cfg_esp_name) DrwTxt(d, g_PlCache[i].espName, x, hy - fS - 2, IM_COL32(255, 255, 255, 255), fS);
                if (cfg_esp_dist) { char dB[16]; snprintf(dB, 16, "%.0fm", g_PlCache[i].di); DrwTxt(d, dB, x, y + 2, IM_COL32(255, 255, 255, 255), fS); } 
                if (cfg_esp_line) { float lY = (cfg_line_pos == 1) ? sH / 2 : (cfg_line_pos == 2) ? sH : 0; d->AddLine(ImVec2(sW / 2, lY), ImVec2(x, hy), IM_COL32(255, 255, 255, 180), 1.5f); } 
            }
            if (cfg_aim_enable && g_PlCache[i].vPred && g_PlCache[i].isVis && !g_PlCache[i].iT && !g_PlCache[i].iF) {
                d->AddCircleFilled(ImVec2(g_PlCache[i].wPred.x, g_PlCache[i].wPred.y), 4.0f, IM_COL32(255, 0, 0, 255));
                d->AddLine(ImVec2(g_PlCache[i].wH.x, g_PlCache[i].wH.y), ImVec2(g_PlCache[i].wPred.x, g_PlCache[i].wPred.y), IM_COL32(255, 0, 0, 150), 1.5f);
            }
        }
    }
    if (cfg_esp_count && g_VisibleEnemies > 0) { char cB[16]; snprintf(cB, 16, "%d", g_VisibleEnemies); DrwTxt(ImGui::GetForegroundDrawList(), cB, sW / 2, 80, IM_COL32(255, 255, 255, 255), 60.0f); }
}

void InitCache() {
    static bool i_d = false; if (i_d) return; 
    static uint64_t l_t = 0; if (g_FCnt - l_t < 120) return; l_t = g_FCnt; 
    
    if (!g_StringClass) {
        auto gmc = (g_mc_t)(g_Il2CppBase_RXP + RVA_GET_MAIN_CAM); auto gn = (g_name_t)(g_Il2CppBase_RXP + RVA_GET_NAME);
        if (IsMemValid((void*)gmc) && IsMemValid((void*)gn)) {
            void* cam = gmc(nullptr); if (IsValidObj(cam)) { void* camName = gn(cam, nullptr); if (IsValidObj(camName)) { void* klass = nullptr; if (FastRead(camName, &klass) && IsValidPtr(klass)) g_StringClass = klass; } }
        }
    }
    if (!g_StringClass) return; 
    auto gt = (g_type_t)(g_Il2CppBase_RXP + RVA_GET_TYPE); if (!IsValidPtr((void*)gt)) return;
    if (!g_TPMgr) { void* s = CStr(S_PM); if (s) { g_TPMgr = gt(s, nullptr); free(s); } } 
    if (!g_TLObj) { void* s = CStr(S_LOOT); if (s) { g_TLObj = gt(s, nullptr); free(s); } } 
    if (!g_TPObj) { void* s = CStr(S_PICKUP); if (s) { g_TPObj = gt(s, nullptr); free(s); } }
    if (!g_TInput) { void* s = CStr(S_INPUT); if (s) { g_TInput = gt(s, nullptr); free(s); } }
    if (!g_TOre) { void* s = CStr(S_ORE); if (s) { g_TOre = gt(s, nullptr); free(s); } }
    if (!g_TScrap) { void* s = CStr(S_SCRAP); if (s) { g_TScrap = gt(s, nullptr); free(s); } } 
    if (!g_TTree) { void* s = CStr(S_TREE); if (s) { g_TTree = gt(s, nullptr); free(s); } }
    if (!g_TCupboard) { void* s = CStr(S_CUPBOARD); if (s) { g_TCupboard = gt(s, nullptr); free(s); } }
    if (!g_TSleeper) { void* s = CStr(S_SLEEPER); if (s) { g_TSleeper = gt(s, nullptr); free(s); } }
    if (g_TPMgr) i_d = true; 
}

void UpdCam() { auto gmc = (g_mc_t)(g_Il2CppBase_RXP + RVA_GET_MAIN_CAM); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); if (!IsMemValid((void*)gmc) || !IsMemValid((void*)cgt)) { g_CCamTr = nullptr; g_CCamTrN = nullptr; g_CCamTrK = nullptr; g_CNatCam = nullptr; return; } void* c = gmc(nullptr); if(!IsValidObj(c)){ g_CCamTr = nullptr; g_CCamTrN = nullptr; g_CCamTrK = nullptr; g_CNatCam = nullptr; return; } void* cn = GetNative(c); void* ck = GetKlass(c); if (cn && ck) { void* t = cgt(c, nullptr); if(IsValidObj(t)){ g_CCamTr = t; g_CCamTrN = GetNative(t); g_CCamTrK = GetKlass(t); g_CNatCam = cn; if (!s_BFCap) { Read((void*)((uintptr_t)cn + OFF_FOV), &s_BFov); if (s_BFov > 20.0f && s_BFov < 120.0f) s_BFCap = true; } } } else { g_CCamTr = nullptr; g_CCamTrN = nullptr; g_CCamTrK = nullptr; g_CNatCam = nullptr; } }
void UpdInput() { if (!IsValidObj(g_TInput)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); if (!IsMemValid((void*)fo)) return; void* a = fo(g_TInput, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 1000) return; void* p = nullptr; if (Read((void*)((uintptr_t)a + OFF_ARRAY_DATA), &p) && IsValidObj(p)) g_PInputObj = p; else g_PInputObj = nullptr; }
void UpdPl() { if (!IsValidObj(g_TPMgr)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gb = (g_bone_t)(g_Il2CppBase_RXP + RVA_GET_BONE); if (!IsMemValid((void*)fo) || !IsMemValid((void*)cgt) || !IsMemValid((void*)gb)) return; void* a = fo(g_TPMgr, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; PlData nC[MAX_PL]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_PL) break; void* p = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &p) || !IsValidObj(p)) continue; void* pn = GetNative(p); void* pk = GetKlass(p); if (!pn || !pk) continue; bool f = false; g_PlMtx.lock(); for (int j = 0; j < g_PlCount; j++) { if (g_PlCache[j].pm == p && g_PlCache[j].pmn == pn && g_PlCache[j].pmk == pk) { nC[nCnt] = g_PlCache[j]; f = true; break; } } g_PlMtx.unlock(); if (!f) { void* t = cgt(p, nullptr); if(!IsValidObj(t)) continue; void* tn = GetNative(t); void* tk = GetKlass(t); if (!tn || !tk) continue; nC[nCnt].pm=p; nC[nCnt].pmn=pn; nC[nCnt].pmk=pk; nC[nCnt].t=t; nC[nCnt].tn=tn; nC[nCnt].tk=tk; nC[nCnt].n[0]='\0'; nC[nCnt].c[0]='\0'; nC[nCnt].espName[0]='\0'; nC[nCnt].iT=false; nC[nCnt].iF=false; nC[nCnt].hb=nullptr; nC[nCnt].hbn=nullptr; nC[nCnt].hbk=nullptr; nC[nCnt].bb=nullptr; nC[nCnt].bbn=nullptr; nC[nCnt].bbk=nullptr; nC[nCnt].lastPos={0,0,0}; nC[nCnt].dir={0,0,0}; nC[nCnt].validForESP=false; void* nL = nullptr; if (Read((void*)((uintptr_t)p + OFF_NICK_LABEL), &nL) && IsValidObj(nL)) { Read((void*)((uintptr_t)nL + OFF_IS_FRIEND), &nC[nCnt].iF); Read((void*)((uintptr_t)nL + OFF_IS_TEAM), &nC[nCnt].iT); void* nO=nullptr; void* cO=nullptr; if (Read((void*)((uintptr_t)nL + OFF_NAME_STR), &nO) && IsValidObj(nO)) MyStrCopy(nC[nCnt].n, ReadStr(nO), 63); if (Read((void*)((uintptr_t)nL + OFF_CLAN_STR), &cO) && IsValidObj(cO)) MyStrCopy(nC[nCnt].c, ReadStr(cO), 63); } CleanRT(nC[nCnt].n); CleanRT(nC[nCnt].c); if (cfg_esp_clan && nC[nCnt].c[0] != '\0') snprintf(nC[nCnt].espName, 128, "%s %s", nC[nCnt].c, (nC[nCnt].n[0] ? nC[nCnt].n : "P")); else snprintf(nC[nCnt].espName, 128, "%s", (nC[nCnt].n[0] ? nC[nCnt].n : "P")); void* an = nullptr; if (Read((void*)((uintptr_t)p + OFF_ANIMATOR), &an) && IsValidObj(an)) { void* hb = gb(an, 10, nullptr); if (IsValidObj(hb)) { nC[nCnt].hb=hb; nC[nCnt].hbn=GetNative(hb); nC[nCnt].hbk=GetKlass(hb); } void* bb = gb(an, 7, nullptr); if (IsValidObj(bb)) { nC[nCnt].bb=bb; nC[nCnt].bbn=GetNative(bb); nC[nCnt].bbk=GetKlass(bb); } } } nCnt++; } std::lock_guard<std::mutex> lock(g_PlMtx); for (int i = 0; i < nCnt; i++) g_PlCache[i] = nC[i]; g_PlCount = nCnt; }
void UpdLoot() { if (!IsValidObj(g_TLObj)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gn = (g_name_t)(g_Il2CppBase_RXP + RVA_GET_NAME); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TLObj, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; LData nC[MAX_L]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_L) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_LMtx.lock(); for (int j = 0; j < g_LCount; j++) { if (g_LCache[j].o == o && g_LCache[j].on == on && g_LCache[j].ok == ok) { nC[nCnt] = g_LCache[j]; f = true; break; } } g_LMtx.unlock(); if (!f) { int cap = 0; if (!Read((void*)((uintptr_t)o + OFF_LOOT_CAPACITY), &cap) || cap < 5 || cap > 50) continue; void* nP = gn(o, nullptr); const char* rN = ""; if (IsValidObj(nP)) rN = ReadStr(nP); if (rN[0] == '\0') continue; if (strstr(rN, oxorany("Military"))) { MyStrCopy(nC[nCnt].n, oxorany("Military"), 31); } else if (strstr(rN, oxorany("ToolBox"))) { MyStrCopy(nC[nCnt].n, oxorany("ToolBox"), 31); } else if (strstr(rN, oxorany("Crate"))) { MyStrCopy(nC[nCnt].n, oxorany("Crate"), 31); } else continue; void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; } nCnt++; } std::lock_guard<std::mutex> lock(g_LMtx); for (int i = 0; i < nCnt; i++) g_LCache[i] = nC[i]; g_LCount = nCnt; }
void UpdPickup() { if (!IsValidObj(g_TPObj)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gn = (g_name_t)(g_Il2CppBase_RXP + RVA_GET_NAME); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TPObj, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; PData nC[MAX_P]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_P) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_PMtx.lock(); for (int j = 0; j < g_PCount; j++) { if (g_PCache[j].o == o && g_PCache[j].on == on && g_PCache[j].ok == ok) { nC[nCnt] = g_PCache[j]; f = true; break; } } g_PMtx.unlock(); if (!f) { void* nP = gn(o, nullptr); const char* rN = ""; if (IsValidObj(nP)) rN = ReadStr(nP); if (rN[0] == '\0') continue; void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; MyStrCopy(nC[nCnt].n, rN, 63); CleanP(nC[nCnt].n); CleanRT(nC[nCnt].n); } nCnt++; } std::lock_guard<std::mutex> lock(g_PMtx); for (int i = 0; i < nCnt; i++) g_PCache[i] = nC[i]; g_PCount = nCnt; }
void UpdOre() { if (!IsValidObj(g_TOre)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gn = (g_name_t)(g_Il2CppBase_RXP + RVA_GET_NAME); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TOre, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; OData nC[MAX_O]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_O) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_OMtx.lock(); for (int j = 0; j < g_OCount; j++) { if (g_OCache[j].o == o && g_OCache[j].on == on && g_OCache[j].ok == ok) { nC[nCnt] = g_OCache[j]; f = true; break; } } g_OMtx.unlock(); if (!f) { void* nP = gn(o, nullptr); const char* rN = ""; if (IsValidObj(nP)) rN = ReadStr(nP); if (rN[0] == '\0') continue; bool isStone = strstr(rN, oxorany("Stone")) != nullptr, isFerum = strstr(rN, oxorany("Ferum")) != nullptr, isOre = strstr(rN, oxorany("Ore")) != nullptr; bool isSulfur = strstr(rN, oxorany("Sulfur")) != nullptr || (isOre && !isStone && !isFerum); if (!isStone && !isFerum && !isSulfur) continue; void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; if (isSulfur) nC[nCnt].type = 3; else if (isFerum) nC[nCnt].type = 2; else if (isStone) nC[nCnt].type = 1; else nC[nCnt].type = 0; if (isSulfur && !strstr(rN, oxorany("Sulfur"))) MyStrCopy(nC[nCnt].n, oxorany("OreSulfur"), 63); else { MyStrCopy(nC[nCnt].n, rN, 63); CleanP(nC[nCnt].n); CleanRT(nC[nCnt].n); } } nCnt++; } std::lock_guard<std::mutex> lock(g_OMtx); for (int i = 0; i < nCnt; i++) g_OCache[i] = nC[i]; g_OCount = nCnt; }
void UpdScrap() { if (!IsValidObj(g_TScrap)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TScrap, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; SData nC[MAX_S]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_S) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_SMtx.lock(); for (int j = 0; j < g_SCount; j++) { if (g_SCache[j].o == o && g_SCache[j].on == on && g_SCache[j].ok == ok) { nC[nCnt] = g_SCache[j]; f = true; break; } } g_SMtx.unlock(); if (!f) { void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; } nCnt++; } std::lock_guard<std::mutex> lock(g_SMtx); for (int i = 0; i < nCnt; i++) g_SCache[i] = nC[i]; g_SCount = nCnt; }
void UpdTree() { if (!IsValidObj(g_TTree)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TTree, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; TData nC[MAX_T]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_T) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_TMtx.lock(); for (int j = 0; j < g_TCount; j++) { if (g_TCache[j].o == o && g_TCache[j].on == on && g_TCache[j].ok == ok) { nC[nCnt] = g_TCache[j]; f = true; break; } } g_TMtx.unlock(); if (!f) { void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; } nCnt++; } std::lock_guard<std::mutex> lock(g_TMtx); for (int i = 0; i < nCnt; i++) g_TCache[i] = nC[i]; g_TCount = nCnt; }
void UpdCupboard() { if (!IsValidObj(g_TCupboard)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TCupboard, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; CData nC[MAX_C]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_C) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_CMtx.lock(); for (int j = 0; j < g_CCount; j++) { if (g_CCache[j].o == o && g_CCache[j].on == on && g_CCache[j].ok == ok) { nC[nCnt] = g_CCache[j]; f = true; break; } } g_CMtx.unlock(); if (!f) { void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; } nCnt++; } std::lock_guard<std::mutex> lock(g_CMtx); for (int i = 0; i < nCnt; i++) g_CCache[i] = nC[i]; g_CCount = nCnt; }
void UpdSleeper() { if (!IsValidObj(g_TSleeper)) return; auto fo = (f_obj_t)(g_Il2CppBase_RXP + RVA_FIND_OF_TYPE); auto cgt = (g_tr_t)(g_Il2CppBase_RXP + RVA_COMP_GET_TRANS); auto gn = (g_name_t)(g_Il2CppBase_RXP + RVA_GET_NAME); auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); if (!IsMemValid((void*)fo) || !IsMemValid((void*)gp)) return; void* a = fo(g_TSleeper, nullptr); if (!IsValidObj(a)) return; uint32_t c = 0; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_LEN), &c) || c == 0 || c > 10000) return; SlData nC[MAX_SL]; int nCnt = 0; for (uint32_t i = 0; i < c; i++) { if (nCnt >= MAX_SL) break; void* o = nullptr; if (!Read((void*)((uintptr_t)a + OFF_ARRAY_DATA + (i * 8)), &o) || !IsValidObj(o)) continue; void* on = GetNative(o); void* ok = GetKlass(o); if (!on || !ok) continue; bool f = false; g_SlMtx.lock(); for (int j = 0; j < g_SlCount; j++) { if (g_SlCache[j].o == o && g_SlCache[j].on == on && g_SlCache[j].ok == ok) { nC[nCnt] = g_SlCache[j]; f = true; break; } } g_SlMtx.unlock(); if (!f) { void* t = cgt(o, nullptr); if(!IsValidObj(t)) continue; Vec3 p = gp(t, nullptr); if(isnan(p.x)) continue; nC[nCnt].o=o; nC[nCnt].on=on; nC[nCnt].ok=ok; nC[nCnt].p=p; void* nP = gn(o, nullptr); if (IsValidObj(nP)) { MyStrCopy(nC[nCnt].n, ReadStr(nP), 63); CleanRT(nC[nCnt].n); } else { nC[nCnt].n[0] = '\0'; } } nCnt++; } std::lock_guard<std::mutex> lock(g_SlMtx); for (int i = 0; i < nCnt; i++) g_SlCache[i] = nC[i]; g_SlCount = nCnt; }

void MainThreadUpdate() {
    if (!g_Authenticated || g_AuthValidationHash != 0x41BA0EA2) return;
    g_FCnt++; if (g_Il2CppBase_RXP == 0) return; InitCache();

    if (IsValidPtr(g_LocalPlayerManager)) {
        void* fpManager = nullptr;
        if (FastRead((void*)((uintptr_t)g_LocalPlayerManager + oxorany((uintptr_t)0x90)), &fpManager) && IsValidObj(fpManager)) {
            void* currentWeapon = nullptr; 
            if (FastRead((void*)((uintptr_t)fpManager + oxorany((uintptr_t)0x58)), &currentWeapon) && IsValidObj(currentWeapon)) {
                void* cwKlass = GetKlass(currentWeapon);
                if (IsValidPtr(cwKlass)) {
                    char* className = nullptr;
                    if (FastRead((void*)((uintptr_t)cwKlass + oxorany((uintptr_t)0x10)), &className) && IsValidPtr(className)) {
                        char buf[32] = {0}; SafeRead(className, buf, 31);
                        if (strstr(buf, oxorany("FPHitscan"))) {
                            void* weaponConfig = nullptr; void* hitscanAnimator = nullptr;
                            FastRead((void*)((uintptr_t)currentWeapon + oxorany((uintptr_t)0x138)), &weaponConfig);
                            FastRead((void*)((uintptr_t)currentWeapon + oxorany((uintptr_t)0x1F0)), &hitscanAnimator);
                            if (IsValidObj(weaponConfig)) {
                                if (cfg_fast_shoot) {
                                    float origFr = 0;
                                    if (FastRead((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x34)), &origFr)) {
                                        if (origFr < 5.0f && origFr > 0.001f) { float val = 0.02f; SafeWrite((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x34)), &val, 4); } 
                                        else if (origFr >= 5.0f && origFr < 10000.0f) { float val = 4000.0f; SafeWrite((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x34)), &val, 4); }
                                    }
                                }
                                if (cfg_fast_reload) { float val = 0.05f; SafeWrite((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x48)), &val, 4); SafeWrite((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x4C)), &val, 4); }
                                if (cfg_no_recoil) {
                                    void* recoilConfig = nullptr;
                                    if (FastRead((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x60)), &recoilConfig) && IsValidObj(recoilConfig)) { Vec2 zero = {0, 0}; SafeWrite((void*)((uintptr_t)recoilConfig + oxorany((uintptr_t)0x18)), &zero, 8); SafeWrite((void*)((uintptr_t)recoilConfig + oxorany((uintptr_t)0x20)), &zero, 8); }
                                    void* dispConfig = nullptr;
                                    if (FastRead((void*)((uintptr_t)weaponConfig + oxorany((uintptr_t)0x58)), &dispConfig) && IsValidObj(dispConfig)) { float zeroF = 0.0f; SafeWrite((void*)((uintptr_t)dispConfig + oxorany((uintptr_t)0x18)), &zeroF, 4); SafeWrite((void*)((uintptr_t)dispConfig + oxorany((uintptr_t)0x1C)), &zeroF, 4); SafeWrite((void*)((uintptr_t)dispConfig + oxorany((uintptr_t)0x20)), &zeroF, 4); SafeWrite((void*)((uintptr_t)dispConfig + oxorany((uintptr_t)0x24)), &zeroF, 4); SafeWrite((void*)((uintptr_t)dispConfig + oxorany((uintptr_t)0x28)), &zeroF, 4); }
                                }
                            }
                            if (IsValidObj(hitscanAnimator)) {
                                if (cfg_fast_shoot) { bool customSpeed = true; float speedVal = 10.0f; SafeWrite((void*)((uintptr_t)hitscanAnimator + oxorany((uintptr_t)0x5C)), &customSpeed, 1); SafeWrite((void*)((uintptr_t)hitscanAnimator + oxorany((uintptr_t)0x60)), &speedVal, 4); }
                                if (cfg_fast_reload) { bool hasAnim = false; SafeWrite((void*)((uintptr_t)hitscanAnimator + oxorany((uintptr_t)0x80)), &hasAnim, 1); }
                            }
                        }
                    }
                }
            }
        }
    }

    static bool fps_unlocked_state = false;
    if (cfg_unlock_fps && !fps_unlocked_state) { auto set_fps = (set_fps_t)(g_Il2CppBase_RXP + RVA_SET_FPS); if (IsMemValid((void*)set_fps)) { set_fps(300); fps_unlocked_state = true; } }
    else if (!cfg_unlock_fps && fps_unlocked_state) { auto set_fps = (set_fps_t)(g_Il2CppBase_RXP + RVA_SET_FPS); if (IsMemValid((void*)set_fps)) { set_fps(60); fps_unlocked_state = false; } }

    static bool last_xray_state = false;
    if (g_FCnt % 30 == 0) {
        auto gmc = (g_mc_t)(g_Il2CppBase_RXP + RVA_GET_MAIN_CAM);
        if (IsMemValid((void*)gmc)) { void* c = gmc(nullptr); if (IsValidObj(c)) { auto set_clip = (void(*)(void*, float, void*))(g_Il2CppBase_RXP + RVA_SET_NEARCLIP); if (IsMemValid((void*)set_clip)) { if (cfg_xray_enable) { set_clip(c, cfg_xray_dist, nullptr); last_xray_state = true; } else if (last_xray_state) { set_clip(c, 0.15f, nullptr); last_xray_state = false; } } } }
    }

    if (!IsSafeToRead(g_CCamTr, g_CCamTrN, g_CCamTrK) || !IsValidPtr(g_CNatCam)) { UpdCam(); if (!IsSafeToRead(g_CCamTr, g_CCamTrN, g_CCamTrK) || !IsValidPtr(g_CNatCam)) return; }
    Mat4x4 vM; if (!FastRead((void*)((uintptr_t)g_CNatCam + OFF_MAT_VIEW), &vM)) { g_CNatCam = nullptr; return; } g_vM = vM;
    if (IsValidPtr(g_PInputObj)) { bool isAiming = false; FastRead((void*)((uintptr_t)g_PInputObj + OFF_IS_AIMING), &isAiming); g_IsScoped = isAiming; } else g_IsScoped = false;
    
    auto gp = (g_pos_t)(g_Il2CppBase_RXP + RVA_GET_POS); auto pLinecast = (physics_linecast_t)(g_Il2CppBase_RXP + RVA_LINECAST); if (!IsMemValid((void*)gp)) return;
    Vec3 cP = gp(g_CCamTr, nullptr); g_cP = cP;

    if (g_FCnt % 120 == 0) { UpdPl(); UpdInput(); }
    if (g_FCnt % 120 == 15 && cfg_pickup_enable) UpdPickup(); 
    if (g_FCnt % 120 == 30 && cfg_loot_enable) UpdLoot(); 
    if (g_FCnt % 120 == 45 && (cfg_tree_enable || cfg_auto_farm)) UpdTree();
    if (g_FCnt % 120 == 60 && (cfg_ore_enable || cfg_auto_farm)) UpdOre(); 
    if (g_FCnt % 120 == 75 && cfg_sleeper_enable) UpdSleeper(); 
    if (g_FCnt % 120 == 90 && (cfg_scrap_enable || cfg_auto_farm)) UpdScrap(); 
    if (g_FCnt % 120 == 105 && cfg_cupboard_enable) UpdCupboard();

    Vec3 bestTargetPos = {0, 0, 0}; float minTargetDistSq = 999999.0f; bool bestTargetValid = false, bestTargetIsNarrow = false; 

    if (cfg_auto_farm) {
        if (cfg_ore_enable || cfg_auto_farm) { std::lock_guard<std::mutex> lk(g_OMtx); for (int i = 0; i < g_OCount; i++) { bool validTarget = (cfg_farm_stone && g_OCache[i].type == 1) || (cfg_farm_ferum && g_OCache[i].type == 2) || (cfg_farm_sulfur && g_OCache[i].type == 3); if (validTarget) { float distSq = cP.DistSq(g_OCache[i].p); if (distSq < minTargetDistSq) { minTargetDistSq = distSq; bestTargetPos = g_OCache[i].p; bestTargetValid = true; bestTargetIsNarrow = false; } } } }
        if (cfg_scrap_enable || cfg_auto_farm) { std::lock_guard<std::mutex> lk(g_SMtx); for (int i = 0; i < g_SCount; i++) { if (cfg_farm_scrap) { float distSq = cP.DistSq(g_SCache[i].p); if (distSq < minTargetDistSq) { minTargetDistSq = distSq; bestTargetPos = g_SCache[i].p; bestTargetValid = true; bestTargetIsNarrow = true; } } } }
        if (cfg_tree_enable || cfg_auto_farm) { std::lock_guard<std::mutex> lk(g_TMtx); for (int i = 0; i < g_TCount; i++) { if (cfg_farm_tree) { float distSq = cP.DistSq(g_TCache[i].p); if (distSq < minTargetDistSq) { minTargetDistSq = distSq; bestTargetPos = g_TCache[i].p; bestTargetValid = true; bestTargetIsNarrow = true; } } } }
    }

    int vE = 0; Vec3 bT = {0,0,0}; float bFD = 9999.0f; Vec3 bT_MB = {0,0,0}; float bFD_MB = 9999.0f; void* lML = nullptr;
    
    { 
        std::lock_guard<std::mutex> lk(g_PlMtx);
        for (int i = 0; i < g_PlCount; i++) {
            g_PlCache[i].validForESP = false; g_PlCache[i].vF = false; g_PlCache[i].vH = false; g_PlCache[i].vPred = false;
            if (!IsSafeToRead(g_PlCache[i].pm, g_PlCache[i].pmn, g_PlCache[i].pmk) || !IsSafeToRead(g_PlCache[i].t, g_PlCache[i].tn, g_PlCache[i].tk)) continue;
            Vec3 rP = gp(g_PlCache[i].t, nullptr); float diSq = rP.DistSq(cP);
            if (diSq > 250000.0f || diSq < 0.01f) continue;
            g_PlCache[i].di = sqrtf(diSq);
            
            if (g_PlCache[i].di < 4.0f) { FastRead((void*)((uintptr_t)g_PlCache[i].pm + OFF_MOUSE_LOOK), &lML); g_LocalPlayerManager = g_PlCache[i].pm; continue; } 
            
            g_PlCache[i].validForESP = true;
            if (g_FCnt % 5 == 0) { if (g_PlCache[i].lastPos.x != 0 || g_PlCache[i].lastPos.z != 0) { Vec3 delta = { rP.x - g_PlCache[i].lastPos.x, 0.0f, rP.z - g_PlCache[i].lastPos.z }; float mDistSq = delta.x*delta.x + delta.z*delta.z; if (mDistSq > 0.0025f) { float mDist = sqrtf(mDistSq); g_PlCache[i].dir = {delta.x / mDist, 0.0f, delta.z / mDist}; } else g_PlCache[i].dir = {0.0f, 0.0f, 0.0f}; } g_PlCache[i].lastPos = rP; }

            // БЕРЕМ ЧИСТЫЙ BONE 10 (ДЛЯ МАГИИ И АИМА) - НИКАКОЙ ДОП. МАТЕМАТИКИ!
            Vec3 hP = rP, bP = rP; bool hB = false;
            if (g_PlCache[i].hb && IsSafeToRead(g_PlCache[i].hb, g_PlCache[i].hbn, g_PlCache[i].hbk)) { hP = gp(g_PlCache[i].hb, nullptr); hB = true; }
            if (!hB) hP.y += 1.8f; 
            if (fabs(hP.y - rP.y) < 0.9f) continue; vE++;

            g_PlCache[i].hB = hB;
            g_PlCache[i].vF = W2S(rP, g_PlCache[i].wF, vM, g_ScreenW, g_ScreenH);
            g_PlCache[i].vH = W2S(hP, g_PlCache[i].wH, vM, g_ScreenW, g_ScreenH);

            bool isVis = g_PlCache[i].isVis; 
            if (g_FCnt % 6 == i % 6) { if (g_PlCache[i].vH && g_PlCache[i].di <= cfg_aim_max_dist + 50.0f) { int mask = (1 << 0) | (1 << 12) | (1 << 15) | (1 << 17) | (1 << 19); isVis = !pLinecast(cP, hP, mask, nullptr); } else isVis = false; }
            g_PlCache[i].isVis = isVis; 

            // LOGIC AIMBOT
            bool needAim = (cfg_aim_enable && !g_PlCache[i].iT && !g_PlCache[i].iF && !cfg_auto_farm && (!cfg_aim_scope_only || g_IsScoped) && g_PlCache[i].di <= cfg_aim_max_dist);
            if (needAim && cfg_aim_vis_check && !isVis) needAim = false;
            if (needAim && cfg_aim_bone == 1 && g_PlCache[i].bb && IsSafeToRead(g_PlCache[i].bb, g_PlCache[i].bbn, g_PlCache[i].bbk)) bP = gp(g_PlCache[i].bb, nullptr);

            if (needAim) { 
                Vec3 aP = (cfg_aim_bone == 0) ? hP : bP; // Чистая кость
                
                float current_predict = 0.12f * cfg_aim_smooth;
                Vec3 pP = aP; pP.x += g_PlCache[i].dir.x * current_predict; pP.z += g_PlCache[i].dir.z * current_predict; 
                
                Vec3 wA; 
                if (W2S(pP, wA, vM, g_ScreenW, g_ScreenH)) { 
                    g_PlCache[i].wPred = wA; g_PlCache[i].vPred = true; 
                    float fD = GetFDist(wA.x, wA.y); 
                    if (fD < cfg_aim_fov && fD < bFD) { bFD = fD; bT = pP; } 
                } 
            }
            
            // LOGIC MAGIC BULLET
            bool needMB = (cfg_magic_bullet && !g_PlCache[i].iT && !g_PlCache[i].iF && !cfg_auto_farm && g_PlCache[i].di <= cfg_aim_max_dist);
            if (needMB && cfg_aim_vis_check && !isVis) needMB = false;
            if (needMB) {
                Vec3 aP = (cfg_aim_bone == 0) ? hP : bP; // Чистая кость
                Vec3 wA;
                if (W2S(aP, wA, vM, g_ScreenW, g_ScreenH)) {
                    float fD = GetFDist(wA.x, wA.y);
                    if (fD < cfg_mb_fov && fD < bFD_MB) { bFD_MB = fD; bT_MB = aP; }
                }
            }
        }
    }
    g_VisibleEnemies = vE;
    g_MB_HasTarget = (bFD_MB < 9999.0f); if (g_MB_HasTarget) g_MB_TargetPos = bT_MB;

    // ======================================================================================
    // ИНЖЕКТ АИМБОТА И ФАРМ-БОТА В ВИДЕ "ФЕЙКОВЫХ СВАЙПОВ" (БЕЗ ЗАПИСИ НАПРЯМУЮ В КАМЕРУ)
    // ======================================================================================
    if (cfg_auto_farm && IsValidPtr(lML)) {
        if (bestTargetValid) { 
            Vec2 tA = CalcAng(cP, bestTargetPos); void* aPt = (void*)((uintptr_t)lML + OFF_ANGLES); Vec2 cA; 
            if (FastRead(aPt, &cA) && !isnan(cA.x) && !isnan(cA.y)) {
                float delta_pitch = tA.x - cA.x; float delta_yaw = tA.y - cA.y;
                while (delta_yaw > 180.0f) delta_yaw -= 360.0f; while (delta_yaw < -180.0f) delta_yaw += 360.0f;
                
                void* osMouseClass = nullptr;
                if (FastRead((void*)(g_Il2CppBase_RXP + RVA_ON_SCREEN_MOUSE_TYPEINFO), &osMouseClass) && IsValidPtr(osMouseClass)) {
                    void* staticFields = nullptr;
                    if (FastRead((void*)((uintptr_t)osMouseClass + 0xB8), &staticFields) && IsValidPtr(staticFields)) {
                        Vec2 swipe = {0, 0};
                        if (FastRead((void*)((uintptr_t)staticFields + 0x0), &swipe)) {
                            swipe.x += (delta_yaw * 40.0f);
                            swipe.y += (-delta_pitch * 40.0f);
                            SafeWrite((void*)((uintptr_t)staticFields + 0x0), &swipe, sizeof(Vec2));
                        }
                    }
                }
            }
            float requiredDist = bestTargetIsNarrow ? 1.8f : 2.5f; 
            if (minTargetDistSq > requiredDist * requiredDist) { farm_sprint = true; farm_attack = false; farm_crouch = false; } 
            else { farm_sprint = false; farm_attack = true; farm_crouch = true; } 
        } else { farm_sprint = false; farm_attack = false; farm_crouch = false; }
    } 
    else { 
        farm_sprint = false; farm_attack = false; farm_crouch = false; 
        if (cfg_aim_enable && IsValidPtr(lML) && bFD < 9999.0f) { 
            void* aPt = (void*)((uintptr_t)lML + OFF_ANGLES); Vec2 cA; 
            if (FastRead(aPt, &cA) && !isnan(cA.x) && !isnan(cA.y)) { 
                Vec2 tA = CalcAng(cP, bT); 
                
                float delta_pitch = tA.x - cA.x;
                float delta_yaw = tA.y - cA.y;
                while (delta_yaw > 180.0f) delta_yaw -= 360.0f;
                while (delta_yaw < -180.0f) delta_yaw += 360.0f;
                
                // 🔥 ТАЙНЫЙ АИМБОТ: ОТПРАВЛЯЕМ СВАЙПЫ ПО ЭКРАНУ (ЧЕРЕЗ TYPEINFO) 🔥
                void* osMouseClass = nullptr;
                if (FastRead((void*)(g_Il2CppBase_RXP + RVA_ON_SCREEN_MOUSE_TYPEINFO), &osMouseClass) && IsValidPtr(osMouseClass)) {
                    void* staticFields = nullptr;
                    // 0xB8 - стандартный оффсет для указателя static_fields в 64-битном IL2CPP
                    if (FastRead((void*)((uintptr_t)osMouseClass + 0xB8), &staticFields) && IsValidPtr(staticFields)) {
                        float speedMultiplier = 60.0f / cfg_aim_smooth; 
                        Vec2 swipe = {0, 0};
                        
                        // 0x0 - оффсет поля Delta (как было указано в твоем дампе: "public static Vector2 Delta; // 0x0")
                        if (FastRead((void*)((uintptr_t)staticFields + 0x0), &swipe)) {
                            swipe.x += (delta_yaw * speedMultiplier);
                            swipe.y += (-delta_pitch * speedMultiplier); // Инвертируем Y для правильного направления
                            SafeWrite((void*)((uintptr_t)staticFields + 0x0), &swipe, sizeof(Vec2));
                        }
                    }
                }
            } 
        } 
    }
}

void (*orig_Update)(void*);
void hk_Update(void* instance) {
    g_RenderCount++;
    MainThreadUpdate();
    if (orig_Update) orig_Update(instance);
}

void FreezeThread() {
    bool wS = false, wA = false, wC = false;
    while (true) {
        if (!g_Authenticated || g_AuthValidationHash != 0x41BA0EA2) { wS = false; wA = false; wC = false; std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
        uintptr_t pInput = (uintptr_t)g_PInputObj;
        if (pInput < 0x100000 || pInput > 0x00007FFFFFFFFFFF) { wS = false; wA = false; wC = false; std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }

        bool nS = farm_sprint; bool nA = farm_attack; bool nC = farm_crouch; 
        if (nS) { *(bool*)(pInput + OFF_SPRINT) = true; *(float*)(pInput + OFF_SPRINT_FLOAT) = 1.0f; wS = true; } else if (wS) { *(bool*)(pInput + OFF_SPRINT) = false; *(float*)(pInput + OFF_SPRINT_FLOAT) = 0.0f; wS = false; }
        if (nA) { *(bool*)(pInput + OFF_SIGMA) = true; wA = true; } else if (wA) { *(bool*)(pInput + OFF_SIGMA) = false; wA = false; }
        if (nC) { *(bool*)(pInput + OFF_CROUCH) = true; wC = true; } else if (wC) { *(bool*)(pInput + OFF_CROUCH) = false; wC = false; }

        if (!nS && !nA && !nC) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void r_thread() {
    g_Pid = getpid(); while (!g_SurfaceReady) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    JNIEnv* e; if (g_JVM->AttachCurrentThread(&e, NULL) != JNI_OK) return; 
    
    jclass svC=e->FindClass(oxorany("android/view/SurfaceView")); jobject h=e->CallObjectMethod(g_SurfaceView,e->GetMethodID(svC,oxorany("getHolder"),oxorany("()Landroid/view/SurfaceHolder;"))); jclass hC=e->FindClass(oxorany("android/view/SurfaceHolder")); jmethodID gs=e->GetMethodID(hC,oxorany("getSurface"),oxorany("()Landroid/view/Surface;")); jclass sC=e->FindClass(oxorany("android/view/Surface")); jmethodID iv=e->GetMethodID(sC,oxorany("isValid"),oxorany("()Z"));
    
    while (true) {
        jobject s=nullptr; while(true){s=e->CallObjectMethod(h,gs); if(s){if(e->CallBooleanMethod(s,iv))break; e->DeleteLocalRef(s);} std::this_thread::sleep_for(std::chrono::milliseconds(200));}
        ANativeWindow* w=ANativeWindow_fromSurface(e,s); EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(d,0,0); EGLint a[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_BLUE_SIZE,8,EGL_GREEN_SIZE,8,EGL_RED_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE}; EGLConfig c; EGLint nc; eglChooseConfig(d,a,&c,1,&nc); EGLint f; eglGetConfigAttrib(d,c,EGL_NATIVE_VISUAL_ID,&f); ANativeWindow_setBuffersGeometry(w,0,0,f); EGLSurface es=eglCreateWindowSurface(d,c,w,NULL); EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE}; EGLContext ctx=eglCreateContext(d,c,NULL,ca); eglMakeCurrent(d,es,es,ctx); eglQuerySurface(d,es,EGL_WIDTH,&g_ScreenW); eglQuerySurface(d,es,EGL_HEIGHT,&g_ScreenH);
        ImGui::CreateContext(); ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2((float)g_ScreenW,(float)g_ScreenH); ImVector<ImWchar> r; ImFontGlyphRangesBuilder bd; bd.AddRanges(io.Fonts->GetGlyphRangesCyrillic()); bd.BuildRanges(&r); io.Fonts->AddFontFromFileTTF(oxorany("/system/fonts/Roboto-Regular.ttf"),26.0f,NULL,r.Data); ImGui_ImplOpenGL3_Init(oxorany("#version 300 es")); LoadLogoTexture(); ImGuiStyle& st=ImGui::GetStyle(); ImGui::StyleColorsDark(&st); ImGuiStyle bst=st; bool sl=false; float lsc=1.0f;
        while (!sl) {
            if(!e->CallBooleanMethod(s,iv)){sl=true;break;} glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            if(g_MenuScale!=g_TempMenuScale&&(ImGui::GetTime()-g_LastScaleEditTime>0.3)) g_MenuScale=g_TempMenuScale;
            if(lsc!=g_MenuScale){ImGui::GetStyle()=bst; ImGui::GetStyle().ScaleAllSizes(g_MenuScale); ImGui::GetIO().FontGlobalScale=g_MenuScale; lsc=g_MenuScale;}
            if(g_Il2CppBase_RXP){ auto gm=(g_mpos_t)(g_Il2CppBase_RXP+RVA_INPUT_MOUSEPOS); auto gb=(g_mbtn_t)(g_Il2CppBase_RXP+RVA_INPUT_MOUSEBTN); if(IsMemValid((void*)gm)&&IsMemValid((void*)gb)){ Vec3 p=gm(nullptr); io.MousePos=ImVec2(p.x,(float)g_ScreenH-p.y); io.MouseDown[0]=gb(0,nullptr); } }
            
            ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame(); 
            if (!g_Authenticated || g_AuthValidationHash != 0x41BA0EA2) { DrawAuthMenu(); } else { DrawMenu(); DrawESP(); }
            ImGui::Render(); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); if(eglSwapBuffers(d,es)==EGL_FALSE)sl=true; std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        ImGui_ImplOpenGL3_Shutdown(); ImGui::DestroyContext(); eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(d,ctx); eglDestroySurface(d,es); eglTerminate(d); ANativeWindow_release(w); e->DeleteLocalRef(s); g_LogoTex = 0;
    }
}

void m_thread() { 
    g_Pid = getpid(); 
    while(!g_Il2CppBase_RXP) { 
        g_Il2CppBase_RXP = get_lib_rxp(oxorany("libil2cpp.so")); 
        std::this_thread::sleep_for(std::chrono::seconds(1)); 
    } 
    std::this_thread::sleep_for(std::chrono::seconds(3)); 

    DobbyHook((void*)(g_Il2CppBase_RXP + RVA_UPDATE), (void*)hk_Update, (void**)&orig_Update);
    DobbyHook((void*)(g_Il2CppBase_RXP + RVA_RAYCAST), (void*)hk_Raycast, (void**)&orig_Raycast);
    
    std::thread(c_thread).detach(); 
    std::thread(r_thread).detach(); 
    std::thread(FreezeThread).detach(); 
}

// =====================================================================
// 🔥 ТОЧКА ВХОДА (Вызывается самой игрой при распаковке) 🔥
// =====================================================================
EXPORT void JNICALL Java_com_ng_application_NGApplication_NG_1attachBaseContext(JNIEnv *env, jobject thiz, jobject context) {
    LOGI("[+] Fake NG_attachBaseContext executed! Hijacking the process...");
    
    env->GetJavaVM(&g_JVM); 
    
    Dl_info info;
    if (dladdr((void*)m_thread, &info)) {
        uintptr_t base = (uintptr_t)info.dli_fbase;
        size_t page_size = sysconf(_SC_PAGESIZE);
        if (mprotect((void*)base, page_size, PROT_READ | PROT_WRITE) == 0) {
            *(uint32_t*)base = 0x00000000; 
            mprotect((void*)base, page_size, PROT_READ | PROT_EXEC);
        }
    }
    
    std::thread(m_thread).detach();
}

EXPORT void JNICALL Java_com_ng_application_NGApplication_NG_1onCreate(JNIEnv *env, jobject thiz) {
    LOGI("[+] Fake NG_onCreate executed! Watchdog neutralized.");
}

EXPORT int ng_ioctl(uint32_t cmd, void *arg, uint64_t extra) {
    switch (cmd) {
        case 1: return -1;
        case 2: {
            uint8_t ctx[0x80];
            memset(ctx, 0x00, 0x20); memset(ctx + 0x20, 0xFF, 0x50); ctx[0x70] = 0;
            if ((initialize_internal_call_context(ctx) & 1) == 0) return -1;
            lrand48(); finalize_internal_call_context(ctx, arg); return 0;
        }
        case 3: return il2cpp_add_internal_call_impl(static_cast<char *>(arg), extra);
        case 4: return il2cpp_resolve_icall_impl(static_cast<char *>(arg), reinterpret_cast<uint64_t *>(extra));
        default: return -1;
    }
}

EXPORT int is_emulator() { return 0; }
EXPORT void register_response_callback(void* callback) {}
EXPORT int64_t on_player_disconnected() { return 0; }
EXPORT int64_t on_application_paused() { return 0; }
EXPORT int64_t on_application_resumed() { return 0; }
EXPORT int64_t set_player_token(const char* token) { return 0; }
EXPORT void register_callback(void* callback) {}
EXPORT int on_received_response() { return 0; }
