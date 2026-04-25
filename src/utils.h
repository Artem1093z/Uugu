#pragma once

#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <cmath>

#define OXORANY_DISABLE_OBFUSCATION
#define OXORANY_USE_BIT_CAST
#include "oxorany.h"
#ifndef oxorany
#define oxorany obf
#endif

// Глобальные переменные движка
extern JavaVM* g_JVM;
extern jobject g_Activity;
extern jobject g_SurfaceView;
extern bool g_SurfaceReady;
extern pid_t g_Pid;
extern int g_ScreenW;
extern int g_ScreenH;
extern uintptr_t g_Il2CppBase_RXP;
extern void* g_StringClass; // Указатель на System.String

#define PI 3.14159265358979323846f
#define RAD2DEG (180.0f / PI)

// ======================== МАТЕМАТИКА ========================
struct Vec2 { float x, y; };
struct Vec3 {
    float x, y, z; 
    float Dist(const Vec3& b) const { float dx = x - b.x, dy = y - b.y, dz = z - b.z; return sqrtf(dx*dx + dy*dy + dz*dz); } 
    float DistSq(const Vec3& b) const { float dx = x - b.x, dy = y - b.y, dz = z - b.z; return dx*dx + dy*dy + dz*dz; } 
    Vec3 operator-(const Vec3& b) const { return {x - b.x, y - b.y, z - b.z}; }
};
struct Mat4x4 { float m[16]; };

inline bool W2S(const Vec3& p, Vec3& s, const Mat4x4& m, int w, int h) { float cw = m.m[3]*p.x + m.m[7]*p.y + m.m[11]*p.z + m.m[15]; if (cw < 0.001f) return false; float cx = m.m[0]*p.x + m.m[4]*p.y + m.m[8]*p.z + m.m[12]; float cy = m.m[1]*p.x + m.m[5]*p.y + m.m[9]*p.z + m.m[13]; s.x = (w / 2.0f) * (1.0f + cx / cw); s.y = (h / 2.0f) * (1.0f - cy / cw); s.z = cw; return true; }
inline Vec2 CalcAng(Vec3 s, Vec3 d) { Vec3 dl = d - s; float dst = sqrtf(dl.x*dl.x + dl.z*dl.z); return {-atan2(dl.y, dst)*RAD2DEG, atan2(dl.x, dl.z)*RAD2DEG}; }
inline float NormAng(float a) { while(a > 180.0f) a -= 360.0f; while(a < -180.0f) a += 360.0f; return a; }
inline float GetFDist(float x, float y) { float dx = x - g_ScreenW / 2.0f; float dy = y - g_ScreenH / 2.0f; return sqrtf(dx*dx + dy*dy); }

// ======================== ПАМЯТЬ ========================
inline bool SafeRead(void* a, void* b, size_t s) { if((uintptr_t)a < 0x100000) return false; struct iovec l = {b, s}, r = {a, s}; return process_vm_readv(g_Pid, &l, 1, &r, 1, 0) == (ssize_t)s; }
inline bool SafeWrite(void* a, void* b, size_t s) { if((uintptr_t)a < 0x100000) return false; struct iovec l = {b, s}, r = {a, s}; return process_vm_writev(g_Pid, &l, 1, &r, 1, 0) == (ssize_t)s; }
template<typename T> inline bool Read(void* a, T* o) { return SafeRead(a, o, sizeof(T)); }
template<typename T> inline bool FastRead(void* a, T* o) { if((uintptr_t)a < 0x100000 || (uintptr_t)a > 0x00007FFFFFFFFFFF) return false; *o = *reinterpret_cast<T*>(a); return true; }
inline bool IsValidPtr(void* a) { return (uintptr_t)a >= 0x100000 && (uintptr_t)a < 0x00007FFFFFFFFFFF; }
inline bool IsMemValid(void* p) { if(!IsValidPtr(p)) return false; char b; return SafeRead(p, &b, 1); }
inline bool IsValidObj(void* o) { if(!IsValidPtr(o)) return false; void* k = nullptr; if(!Read(o, &k) || !IsValidPtr(k)) return false; return true; }
inline void* GetNative(void* o) { if(!IsValidObj(o)) return nullptr; void* c = nullptr; if(!Read((void*)((uintptr_t)o + oxorany((uintptr_t)0x10)), &c)) return nullptr; return IsValidPtr(c) ? c : nullptr; }
inline void* GetKlass(void* o) { if(!IsValidObj(o)) return nullptr; void* k = nullptr; if(!Read(o, &k)) return nullptr; return IsValidPtr(k) ? k : nullptr; }
inline bool IsSafeToRead(void* o, void* cn, void* ck) { if(!IsValidPtr(o)) return false; void* k=nullptr; if(!Read(o,&k) || k!=ck) return false; void* n=nullptr; if(!FastRead((void*)((uintptr_t)o + oxorany((uintptr_t)0x10)), &n) || n!=cn || !IsValidPtr(n)) return false; return true; }

// ======================== IL2CPP FAKE STRINGS ========================
struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    uint16_t chars[1]; 
};

inline void MyStrCopy(char* d, const char* s, int m) { if(!s||!d)return; int i=0; while(s[i]!='\0'&&i<m-1){d[i]=s[i]; i++;} d[i]='\0'; }
inline void CleanRT(char* s) { if(!s||s[0]=='\0')return; char t[128]; int j=0; bool iT=false; for(int i=0;s[i]!='\0'&&i<127&&j<127;i++){ if(s[i]=='<'){iT=true;continue;} if(s[i]=='>'){iT=false;continue;} if(!iT)t[j++]=s[i]; } t[j]='\0'; MyStrCopy(s,t,127); }
inline void CleanP(char* s) { if(!s||s[0]=='\0')return; char* p=strstr(s,oxorany("_pooled")); if(p)*p='\0'; p=strstr(s,oxorany("(Clone)")); if(p)*p='\0'; int i = 0, j = 0; while(s[i] != '\0') { if(s[i] != '_') { s[j++] = s[i]; } i++; } s[j] = '\0'; }

// ПОИСК r-xp БАЗЫ (без парсинга ELF)
inline uintptr_t get_lib_rxp(const char* n, float minSizeMB = 40.0f) {
    FILE* f = fopen(oxorany("/proc/self/maps"), oxorany("r"));
    if (!f) return 0;
    
    char l[512];
    uintptr_t b = 0;
    
    // Переводим мегабайты в байты
    uint64_t minBytes = (uint64_t)(minSizeMB * 1024.0f * 1024.0f);
    uint64_t maxBytes = 100ULL * 1024ULL * 1024ULL; // Лимит 100 МБ для отсечения кэша (170+ МБ)
    
    while (fgets(l, sizeof(l), f)) {
        uintptr_t s, e, offset = 0;
        char p_str[8] = {0};
        char path[256] = {0};
        
        // ВАЖНО: добавили %lx чтобы вытащить смещение (offset)
        int scanned = sscanf(l, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &s, &e, p_str, &offset, path);
        if (scanned < 4) continue;
        if (scanned == 4) path[0] = '\0'; 
        
        if (p_str[0] == 'r') {
            char* p = path; while (*p == ' ') p++;
            bool isAnon = (strlen(p) == 0);
            bool isNamed = (strlen(p) > 0 && strstr(p, n));
            uint64_t size = e - s;
            
            // 1. Идеальное совпадение: имя совпадает, смещение 0, размер меньше 100 МБ (наша база 63 МБ)
            if (isNamed && offset == 0 && size < maxBytes) {
                b = s; 
                break; // Нашли идеальную базу как в IDA!
            }
            
            // 2. Бронебойный фолбэк для античита: если имя стерто (Anon), размер подходит, 
            // смещение 0, и там лежит сигнатура ELF (0x464C457F)
            if (b == 0 && isAnon && offset == 0 && size >= minBytes && size < maxBytes) {
                // В интернале мы можем безопасно читать 'r' память напрямую!
                if (*(uint32_t*)s == 0x464C457F) {
                    b = s; 
                    // Не делаем break на случай, если дальше найдем регион с нормальным именем
                }
            }
        }
    }
    fclose(f);
    return b;
}
// СОЗДАНИЕ ФЕЙКОВОЙ СТРОКИ ЧЕРЕЗ MALLOC (Замена il2cpp_string_new)
inline void* CStr(const char* text) { 
    if (!g_StringClass || !text) return nullptr;
    size_t len = strlen(text);
    // Размер: Header(16) + Length(4) + Chars(len*2) + Null terminator(2) = 22 + len*2
    Il2CppString* str = (Il2CppString*)malloc(22 + len * 2);
    if (!str) return nullptr;
    
    str->klass = g_StringClass;
    str->monitor = nullptr;
    str->length = (int32_t)len;
    
    for(size_t i = 0; i < len; i++) {
        str->chars[i] = (uint16_t)text[i];
    }
    str->chars[len] = 0;
    
    return (void*)str;
}

inline const char* ReadStr(void* o) { static char b[8][256]; static int i=0; i=(i+1)%8; char* r=b[i]; r[0]='\0'; if(!IsValidPtr(o)) return r; int32_t l=0; if(!Read((void*)((uintptr_t)o + oxorany((uintptr_t)0x10)), &l) || l <= 0 || l >= 255) return r; static uint16_t u[256]; if(!SafeRead((void*)((uintptr_t)o + oxorany((uintptr_t)0x14)), u, l * 2)) return r; int p=0; for(int j=0;j<l&&p<250;j++){ uint16_t w=u[j]; if(w<0x80) r[p++]=(char)w; else if(w<0x800){ r[p++]=(char)(0xC0|(w>>6)); r[p++]=(char)(0x80|(w&0x3F)); } } r[p]='\0'; return r; }

// ======================== КРИПТОГРАФИЯ ========================
namespace Crypto {
    struct SHA256_CTX { uint8_t data[64]; uint32_t datalen; uint64_t bitlen; uint32_t state[8]; };
    #define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
    #define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
    #define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
    #define BSIG0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
    #define BSIG1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
    #define SSIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
    #define SSIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))
    inline const uint32_t K[64] = { 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5, 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174, 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da, 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967, 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85, 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070, 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3, 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    inline void sha256_init(SHA256_CTX *ctx) { ctx->datalen=0; ctx->bitlen=0; ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85; ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a; ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c; ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19; }
    inline void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) { uint32_t a,b,c,d,e,f,g,h,i,j,t1,t2,m[64]; for(i=0,j=0;i<16;++i,j+=4) m[i]=(data[j]<<24)|(data[j+1]<<16)|(data[j+2]<<8)|(data[j+3]); for(;i<64;++i) m[i]=SSIG1(m[i-2])+m[i-7]+SSIG0(m[i-15])+m[i-16]; a=ctx->state[0];b=ctx->state[1];c=ctx->state[2];d=ctx->state[3];e=ctx->state[4];f=ctx->state[5];g=ctx->state[6];h=ctx->state[7]; for(i=0;i<64;++i) { t1=h+BSIG1(e)+CH(e,f,g)+K[i]+m[i]; t2=BSIG0(a)+MAJ(a,b,c); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; } ctx->state[0]+=a;ctx->state[1]+=b;ctx->state[2]+=c;ctx->state[3]+=d;ctx->state[4]+=e;ctx->state[5]+=f;ctx->state[6]+=g;ctx->state[7]+=h; }
    inline void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) { for(size_t i=0;i<len;++i) { ctx->data[ctx->datalen]=data[i]; ctx->datalen++; if(ctx->datalen==64) { sha256_transform(ctx,ctx->data); ctx->bitlen+=512; ctx->datalen=0; } } }
    inline void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) { uint32_t i=ctx->datalen; if(ctx->datalen<56) { ctx->data[i++]=0x80; while(i<56) ctx->data[i++]=0x00; } else { ctx->data[i++]=0x80; while(i<64) ctx->data[i++]=0x00; sha256_transform(ctx,ctx->data); memset(ctx->data,0,56); } ctx->bitlen+=ctx->datalen*8; ctx->data[63]=ctx->bitlen; ctx->data[62]=ctx->bitlen>>8; ctx->data[61]=ctx->bitlen>>16; ctx->data[60]=ctx->bitlen>>24; ctx->data[59]=ctx->bitlen>>32; ctx->data[58]=ctx->bitlen>>40; ctx->data[57]=ctx->bitlen>>48; ctx->data[56]=ctx->bitlen>>56; sha256_transform(ctx,ctx->data); for(i=0;i<4;++i) { hash[i]=(ctx->state[0]>>(24-i*8))&0xFF; hash[i+4]=(ctx->state[1]>>(24-i*8))&0xFF; hash[i+8]=(ctx->state[2]>>(24-i*8))&0xFF; hash[i+12]=(ctx->state[3]>>(24-i*8))&0xFF; hash[i+16]=(ctx->state[4]>>(24-i*8))&0xFF; hash[i+20]=(ctx->state[5]>>(24-i*8))&0xFF; hash[i+24]=(ctx->state[6]>>(24-i*8))&0xFF; hash[i+28]=(ctx->state[7]>>(24-i*8))&0xFF; } }
    inline std::string hmac_sha256(std::string key, std::string msg) { SHA256_CTX ctx; uint8_t k[64]={0}, res[32], inner[64], outer[64]; if(key.length()>64) { sha256_init(&ctx); sha256_update(&ctx,(uint8_t*)key.data(),key.length()); sha256_final(&ctx,k); } else memcpy(k,key.data(),key.length()); for(int i=0;i<64;i++) { inner[i]=k[i]^0x36; outer[i]=k[i]^0x5c; } sha256_init(&ctx); sha256_update(&ctx,inner,64); sha256_update(&ctx,(uint8_t*)msg.data(),msg.length()); sha256_final(&ctx,res); sha256_init(&ctx); sha256_update(&ctx,outer,64); sha256_update(&ctx,res,32); sha256_final(&ctx,res); std::stringstream ss; for(int i=0;i<32;i++) ss<<std::hex<<std::setw(2)<<std::setfill('0')<<(unsigned int)res[i]; return ss.str(); }
}

// ======================== JNI ANDROID UTILS ========================
inline jclass SFindClass(JNIEnv* e, const char* n) { jclass r=e->FindClass(n); if(e->ExceptionCheck()){ e->ExceptionClear(); jclass ac=e->FindClass(oxorany("android/app/ActivityThread")); if(!ac){e->ExceptionClear();return nullptr;} jmethodID cm=e->GetStaticMethodID(ac,oxorany("currentActivityThread"),oxorany("()Landroid/app/ActivityThread;")); if(!cm){e->ExceptionClear();return nullptr;} jobject a=e->CallStaticObjectMethod(ac,cm); if(!a){e->ExceptionClear();return nullptr;} jmethodID gm=e->GetMethodID(ac,oxorany("getApplication"),oxorany("()Landroid/app/Application;")); if(!gm){e->ExceptionClear();return nullptr;} jobject ap=e->CallObjectMethod(a,gm); if(!ap){e->ExceptionClear();return nullptr;} jclass cc=e->FindClass(oxorany("android/content/Context")); jobject cl=e->CallObjectMethod(ap,e->GetMethodID(cc,oxorany("getClassLoader"),oxorany("()Ljava/lang/ClassLoader;"))); if(!cl){e->ExceptionClear();return nullptr;} jclass clc=e->FindClass(oxorany("java/lang/ClassLoader")); std::string jn=n; for(char& c:jn)if(c=='/')c='.'; jstring s=e->NewStringUTF(jn.c_str()); r=(jclass)e->CallObjectMethod(cl,e->GetMethodID(clc,oxorany("loadClass"),oxorany("(Ljava/lang/String;)Ljava/lang/Class;")),s); if(e->ExceptionCheck()){e->ExceptionClear();r=nullptr;} } return r; }

inline std::string JNI_GetHWID(JNIEnv* env) {
    if (!g_Activity) return oxorany("UNKNOWN_HWID");
    jclass secureClass = SFindClass(env, oxorany("android/provider/Settings$Secure"));
    if(!secureClass) { env->ExceptionClear(); return oxorany("UNKNOWN_HWID"); }
    jmethodID getString = env->GetStaticMethodID(secureClass, oxorany("getString"), oxorany("(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;"));
    if(!getString) { env->ExceptionClear(); return oxorany("UNKNOWN_HWID"); }
    jclass contextClass = SFindClass(env, oxorany("android/content/Context"));
    if(!contextClass) { env->ExceptionClear(); return oxorany("UNKNOWN_HWID"); }
    jmethodID getCR = env->GetMethodID(contextClass, oxorany("getContentResolver"), oxorany("()Landroid/content/ContentResolver;"));
    if(!getCR) { env->ExceptionClear(); return oxorany("UNKNOWN_HWID"); }

    jobject cr = env->CallObjectMethod(g_Activity, getCR);
    if(env->ExceptionCheck()) { env->ExceptionClear(); return oxorany("UNKNOWN_HWID"); }

    jstring idStr = env->NewStringUTF(oxorany("android_id"));
    jstring hwidStr = (jstring)env->CallStaticObjectMethod(secureClass, getString, cr, idStr);
    if(env->ExceptionCheck() || !hwidStr) { env->ExceptionClear(); return oxorany("UNKNOWN_HWID"); }

    const char* cHwid = env->GetStringUTFChars(hwidStr, nullptr);
    std::string hwid = cHwid;
    env->ReleaseStringUTFChars(hwidStr, cHwid);
    env->DeleteLocalRef(idStr);
    env->DeleteLocalRef(hwidStr);
    return hwid;
}

inline std::string JNI_HttpGet(JNIEnv* env, const std::string& urlString) {
    std::string result = "";
    jclass urlClass = SFindClass(env, oxorany("java/net/URL"));
    if(!urlClass) { env->ExceptionClear(); return ""; }

    jstring jUrl = env->NewStringUTF(urlString.c_str());
    jmethodID urlInit = env->GetMethodID(urlClass, oxorany("<init>"), oxorany("(Ljava/lang/String;)V"));
    jobject urlObj = env->NewObject(urlClass, urlInit, jUrl);
    if(env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    jmethodID openConn = env->GetMethodID(urlClass, oxorany("openConnection"), oxorany("()Ljava/net/URLConnection;"));
    jobject conn = env->CallObjectMethod(urlObj, openConn);
    if(env->ExceptionCheck() || !conn) { env->ExceptionClear(); return ""; }

    jclass connClass = env->GetObjectClass(conn);
    jmethodID setRequestMethod = env->GetMethodID(connClass, oxorany("setRequestMethod"), oxorany("(Ljava/lang/String;)V"));
    if(setRequestMethod) {
        env->CallVoidMethod(conn, setRequestMethod, env->NewStringUTF(oxorany("GET")));
        if(env->ExceptionCheck()) env->ExceptionClear();
    }

    jmethodID getInputStream = env->GetMethodID(connClass, oxorany("getInputStream"), oxorany("()Ljava/io/InputStream;"));
    jobject inputStream = env->CallObjectMethod(conn, getInputStream);

    if (env->ExceptionCheck() || !inputStream) {
        env->ExceptionClear();
        jmethodID getErrorStream = env->GetMethodID(connClass, oxorany("getErrorStream"), oxorany("()Ljava/io/InputStream;"));
        if(getErrorStream) { inputStream = env->CallObjectMethod(conn, getErrorStream); }
        if (env->ExceptionCheck() || !inputStream) { env->ExceptionClear(); return ""; }
    }

    jclass isrClass = SFindClass(env, oxorany("java/io/InputStreamReader"));
    jobject isrObj = env->NewObject(isrClass, env->GetMethodID(isrClass, oxorany("<init>"), oxorany("(Ljava/io/InputStream;)V")), inputStream);
    if(env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    jclass brClass = SFindClass(env, oxorany("java/io/BufferedReader"));
    jobject brObj = env->NewObject(brClass, env->GetMethodID(brClass, oxorany("<init>"), oxorany("(Ljava/io/Reader;)V")), isrObj);
    if(env->ExceptionCheck()) { env->ExceptionClear(); return ""; }

    jmethodID readLine = env->GetMethodID(brClass, oxorany("readLine"), oxorany("()Ljava/lang/String;"));
    while (true) {
        jstring line = (jstring)env->CallObjectMethod(brObj, readLine);
        if (env->ExceptionCheck() || !line) { env->ExceptionClear(); break; }
        const char* cLine = env->GetStringUTFChars(line, nullptr);
        result += cLine;
        env->ReleaseStringUTFChars(line, cLine);
        env->DeleteLocalRef(line);
    }
    return result;
}

inline std::string JNI_GetClipboard() {
    if (!g_Activity) return "";
    JNIEnv* env;
    int getEnvStat = g_JVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    bool didAttach = false;
    if (getEnvStat == JNI_EDETACHED) {
        if (g_JVM->AttachCurrentThread(&env, NULL) != 0) return "";
        didAttach = true;
    }
    std::string res = "";
    jclass contextClass = SFindClass(env, oxorany("android/content/Context"));
    if(contextClass) {
        jfieldID csField = env->GetStaticFieldID(contextClass, oxorany("CLIPBOARD_SERVICE"), oxorany("Ljava/lang/String;"));
        if(csField) {
            jstring csStr = (jstring)env->GetStaticObjectField(contextClass, csField);
            jmethodID getSystemService = env->GetMethodID(contextClass, oxorany("getSystemService"), oxorany("(Ljava/lang/String;)Ljava/lang/Object;"));
            jobject clipboardManager = env->CallObjectMethod(g_Activity, getSystemService, csStr);
            if (clipboardManager && !env->ExceptionCheck()) {
                jclass cbClass = env->GetObjectClass(clipboardManager);
                jmethodID getClip = env->GetMethodID(cbClass, oxorany("getPrimaryClip"), oxorany("()Landroid/content/ClipData;"));
                if (getClip) {
                    jobject clipData = env->CallObjectMethod(clipboardManager, getClip);
                    if (clipData && !env->ExceptionCheck()) {
                        jclass clipDataClass = env->GetObjectClass(clipData);
                        jmethodID getItemAt = env->GetMethodID(clipDataClass, oxorany("getItemAt"), oxorany("(I)Landroid/content/ClipData$Item;"));
                        jobject item = env->CallObjectMethod(clipData, getItemAt, 0);
                        if (item && !env->ExceptionCheck()) {
                            jclass itemClass = env->GetObjectClass(item);
                            jmethodID getText = env->GetMethodID(itemClass, oxorany("getText"), oxorany("()Ljava/lang/CharSequence;"));
                            jobject charSeq = env->CallObjectMethod(item, getText);
                            if (charSeq && !env->ExceptionCheck()) {
                                jmethodID toString = env->GetMethodID(env->GetObjectClass(charSeq), oxorany("toString"), oxorany("()Ljava/lang/String;"));
                                jstring text = (jstring)env->CallObjectMethod(charSeq, toString);
                                if (text && !env->ExceptionCheck()) {
                                    const char* cText = env->GetStringUTFChars(text, nullptr);
                                    res = cText;
                                    env->ReleaseStringUTFChars(text, cText);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if(env->ExceptionCheck()) env->ExceptionClear();
    if (didAttach) g_JVM->DetachCurrentThread();
    return res;
}

inline std::string ExtractJSONValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos != std::string::npos) {
        pos += search.length();
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) return json.substr(pos, end - pos);
    }
    search = "\"" + key + "\":";
    pos = json.find(search);
    if (pos != std::string::npos) {
        pos += search.length();
        size_t end = json.find_first_of(",}", pos);
        if (end != std::string::npos) {
            std::string val = json.substr(pos, end - pos);
            val.erase(0, val.find_first_not_of(" \t\r\n\""));
            val.erase(val.find_last_not_of(" \t\r\n\"") + 1);
            return val;
        }
    }
    return "";
}

// ======================== СОЗДАНИЕ ПОВЕРХНОСТНОГО ОКНА ANDROID ========================
inline void c_thread() {
    JNIEnv* e; g_JVM->AttachCurrentThread(&e, NULL); jclass lp=e->FindClass(oxorany("android/os/Looper")); e->CallStaticVoidMethod(lp,e->GetStaticMethodID(lp,oxorany("prepare"),oxorany("()V"))); if(e->ExceptionCheck())e->ExceptionClear();
    jclass uc=nullptr; while(!uc){uc=SFindClass(e,oxorany("com/unity3d/player/UnityPlayer")); if(!uc)std::this_thread::sleep_for(std::chrono::milliseconds(500));} jfieldID af=e->GetStaticFieldID(uc,oxorany("currentActivity"),oxorany("Landroid/app/Activity;")); if(!af){e->ExceptionClear();return;} jobject act=nullptr; while(!act){act=e->GetStaticObjectField(uc,af); if(e->ExceptionCheck()){e->ExceptionClear();act=nullptr;} if(!act)std::this_thread::sleep_for(std::chrono::milliseconds(500));}
    
    g_Activity = e->NewGlobalRef(act);

    jclass ac=e->FindClass(oxorany("android/app/Activity")); jmethodID hwf=e->GetMethodID(ac,oxorany("hasWindowFocus"),oxorany("()Z")); while(!e->CallBooleanMethod(act,hwf))std::this_thread::sleep_for(std::chrono::milliseconds(500));
    jclass svC=e->FindClass(oxorany("android/view/SurfaceView")); jobject sv=e->NewObject(svC,e->GetMethodID(svC,oxorany("<init>"),oxorany("(Landroid/content/Context;)V")),act); g_SurfaceView=e->NewGlobalRef(sv); e->CallVoidMethod(sv,e->GetMethodID(svC,oxorany("setZOrderOnTop"),oxorany("(Z)V")),JNI_TRUE); jclass vc=e->FindClass(oxorany("android/view/View")); e->CallVoidMethod(sv,e->GetMethodID(vc,oxorany("setSystemUiVisibility"),oxorany("(I)V")),5894);
    jobject h=e->CallObjectMethod(sv,e->GetMethodID(svC,oxorany("getHolder"),oxorany("()Landroid/view/SurfaceHolder;"))); jclass hc=e->FindClass(oxorany("android/view/SurfaceHolder")); e->CallVoidMethod(h,e->GetMethodID(hc,oxorany("setFormat"),oxorany("(I)V")),-3);
    jclass wc=e->FindClass(oxorany("android/view/WindowManager$LayoutParams")); jobject p=e->NewObject(wc,e->GetMethodID(wc,oxorany("<init>"),oxorany("()V"))); e->SetIntField(p,e->GetFieldID(wc,oxorany("type"),oxorany("I")),1000); e->SetIntField(p,e->GetFieldID(wc,oxorany("flags"),oxorany("I")),0x01000000|0x00000200|0x00000100|0x00000010|0x00000008); e->SetIntField(p,e->GetFieldID(wc,oxorany("format"),oxorany("I")),-3); e->SetIntField(p,e->GetFieldID(wc,oxorany("width"),oxorany("I")),-1); e->SetIntField(p,e->GetFieldID(wc,oxorany("height"),oxorany("I")),-1); jfieldID cf=e->GetFieldID(wc,oxorany("layoutInDisplayCutoutMode"),oxorany("I")); if(cf)e->SetIntField(p,cf,1);
    jobject win=e->CallObjectMethod(act,e->GetMethodID(ac,oxorany("getWindow"),oxorany("()Landroid/view/Window;"))); jobject dec=e->CallObjectMethod(win,e->GetMethodID(e->FindClass(oxorany("android/view/Window")),oxorany("getDecorView"),oxorany("()Landroid/view/View;"))); jmethodID gt=e->GetMethodID(vc,oxorany("getWindowToken"),oxorany("()Landroid/os/IBinder;")); jobject tk=nullptr; while(!tk){tk=e->CallObjectMethod(dec,gt); if(e->ExceptionCheck())e->ExceptionClear(); std::this_thread::sleep_for(std::chrono::milliseconds(100));} e->SetObjectField(p,e->GetFieldID(wc,oxorany("token"),oxorany("Landroid/os/IBinder;")),tk);
    jstring ws=e->NewStringUTF(oxorany("window")); jobject wm=e->CallObjectMethod(act,e->GetMethodID(e->FindClass(oxorany("android/content/Context")),oxorany("getSystemService"),oxorany("(Ljava/lang/String;)Ljava/lang/Object;")),ws); e->CallVoidMethod(wm,e->GetMethodID(e->FindClass(oxorany("android/view/WindowManager")),oxorany("addView"),oxorany("(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V")),sv,p); if(e->ExceptionCheck())e->ExceptionClear(); g_SurfaceReady=true; e->CallStaticVoidMethod(lp,e->GetStaticMethodID(lp,oxorany("loop"),oxorany("()V")));
}