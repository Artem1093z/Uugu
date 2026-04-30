#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <new>
#include <jni.h>
#include <android/log.h>

#define LOG_TAG "libng_bypass"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EXPORT extern "C" __attribute__((visibility("default")))

struct Il2CppStringKey {
    uint64_t meta;
    uint64_t size;
    char *data;
};

struct InternalCallNode {
    InternalCallNode *left;
    InternalCallNode *right;
    InternalCallNode *parent;
    uint64_t flags;
    Il2CppStringKey name;
    uint64_t method;
};

struct PendingInternalCallNode {
    PendingInternalCallNode *left;
    PendingInternalCallNode *right;
    PendingInternalCallNode *parent;
    uint64_t flags;
    Il2CppStringKey name;
    uint64_t replacement;
    uint64_t *target_slot;
};

struct InternalCallMap {
    uint64_t header;
    InternalCallNode *root;
};

struct PendingInternalCallMap {
    uint64_t header;
    PendingInternalCallNode *root;
};

extern "C" InternalCallMap g_internalCalls = {};
extern "C" PendingInternalCallMap g_pendingInternalCalls = {};

static inline InternalCallNode *internal_call_end(InternalCallMap *map) {
    return reinterpret_cast<InternalCallNode *>(&map->root);
}

static inline PendingInternalCallNode *pending_internal_call_end(PendingInternalCallMap *map) {
    return reinterpret_cast<PendingInternalCallNode *>(&map->root);
}

extern "C" void rb_tree_insert_and_rebalance(void *tree, void *parent, void *slot, void *node) {
    (void)tree;
    auto **slot_ptr = static_cast<InternalCallNode **>(slot);
    auto *parent_node = static_cast<InternalCallNode *>(parent);
    auto *new_node = static_cast<InternalCallNode *>(node);

    *slot_ptr = new_node;
    new_node->left = nullptr;
    new_node->right = nullptr;
    new_node->parent = parent_node;
    new_node->flags = 0;
}

extern "C" int initialize_internal_call_context(void *ctx) {
    (void)ctx;
    return 1;
}

extern "C" void finalize_internal_call_context(void *ctx, void *arg) {
    (void)ctx;
    (void)arg;
}

static inline bool string_is_heap(const Il2CppStringKey *s) { return (s->meta & 1) != 0; }
static inline size_t string_length(const Il2CppStringKey *s) { return string_is_heap(s) ? s->size : s->meta >> 1; }
static inline char *string_data(Il2CppStringKey *s) { return string_is_heap(s) ? s->data : reinterpret_cast<char *>(s) + 1; }
static inline const char *string_data_const(const Il2CppStringKey *s) { return string_is_heap(s) ? s->data : reinterpret_cast<const char *>(s) + 1; }

static inline void free_string(Il2CppStringKey *s) {
    if (string_is_heap(s)) free(s->data);
    s->meta = 0; s->size = 0; s->data = nullptr;
}

extern "C" void throw_length_error(void *) { std::abort(); }
extern "C" void throw_out_of_range(void *) { std::abort(); }

extern "C" void *allocate_or_throw(size_t size) {
    size_t real_size = size ? size : 1;
    for (;;) {
        void *p = malloc(real_size);
        if (p) return p;
        std::new_handler handler = std::get_new_handler();
        if (!handler) break;
        handler();
    }
    throw std::bad_alloc();
}

extern "C" void build_string_from_cstr(Il2CppStringKey *dst, const char *src) {
    size_t len = strlen(src);
    if (len >= 0xFFFFFFFFFFFFFFF0ULL) throw_length_error(dst);
    char *buf;
    if (len > 0x16) {
        uint64_t cap = len | 0xFULL;
        buf = static_cast<char *>(allocate_or_throw(cap + 1));
        dst->meta = cap + 2; dst->size = len; dst->data = buf;
    } else {
        dst->meta = len * 2; dst->size = 0; dst->data = nullptr;
        buf = reinterpret_cast<char *>(dst) + 1;
    }
    memmove(buf, src, len);
    buf[len] = 0;
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
    InternalCallNode *end = internal_call_end(map);
    InternalCallNode *cur = map->root;
    if (!cur) return end;
    InternalCallNode *candidate = end;
    while (cur) {
        int cmp = compare_string_keys(&cur->name, name);
        if (cmp >= 0) { candidate = cur; cur = cur->left; } 
        else { cur = cur->right; }
    }
    if (candidate == end) return end;
    if (compare_string_keys(name, &candidate->name) < 0) return end;
    return candidate;
}

extern "C" PendingInternalCallNode *find_pending_internal_call_node(PendingInternalCallMap *map, const Il2CppStringKey *name) {
    PendingInternalCallNode *end = pending_internal_call_end(map);
    PendingInternalCallNode *cur = map->root;
    if (!cur) return end;
    PendingInternalCallNode *candidate = end;
    while (cur) {
        int cmp = compare_string_keys(&cur->name, name);
        if (cmp >= 0) { candidate = cur; cur = cur->left; } 
        else { cur = cur->right; }
    }
    if (candidate == end) return end;
    if (compare_string_keys(name, &candidate->name) < 0) return end;
    return candidate;
}

extern "C" InternalCallNode **find_internal_call_insert_slot(InternalCallMap *map, InternalCallNode **parent_out, const Il2CppStringKey *name) {
    InternalCallNode **slot = &map->root;
    InternalCallNode *cur = map->root;
    InternalCallNode *parent = internal_call_end(map);
    while (cur) {
        parent = cur;
        int cmp_left = compare_string_keys(name, &cur->name);
        if (cmp_left < 0) { slot = &cur->left; cur = cur->left; continue; }
        int cmp_right = compare_string_keys(&cur->name, name);
        if (cmp_right < 0) { slot = &cur->right; cur = cur->right; continue; }
        *parent_out = cur; return slot;
    }
    *parent_out = parent; return slot;
}

extern "C" InternalCallNode *get_or_insert_internal_call_node(InternalCallMap *map, const Il2CppStringKey *name, Il2CppStringKey **name_to_move) {
    InternalCallNode *parent = nullptr;
    InternalCallNode **slot = find_internal_call_insert_slot(map, &parent, name);
    InternalCallNode *node = *slot;
    if (!node) {
        node = static_cast<InternalCallNode *>(allocate_or_throw(sizeof(InternalCallNode)));
        Il2CppStringKey *src = *name_to_move;
        node->left = nullptr; node->right = nullptr; node->parent = nullptr; node->flags = 0;
        node->name.meta = src->meta; node->name.size = src->size; node->name.data = src->data;
        src->meta = 0; src->size = 0; src->data = nullptr;
        node->method = 0;
        rb_tree_insert_and_rebalance(map, parent, slot, node);
    }
    return node;
}

extern "C" int64_t string_find_char(const Il2CppStringKey *str, unsigned char ch, uint64_t start) {
    size_t len = string_length(str);
    if (len <= start) return -1;
    const char *data = string_data_const(str);
    const void *p = memchr(data + start, ch, len - start);
    if (!p) return -1;
    return static_cast<const char *>(p) - data;
}

extern "C" void string_substr(Il2CppStringKey *dst, const Il2CppStringKey *src, uint64_t start, size_t count) {
    size_t src_len = string_length(src);
    if (start > src_len) throw_out_of_range(dst);
    size_t available = src_len - start;
    size_t len = available >= count ? count : available;
    if (len >= 0xFFFFFFFFFFFFFFF0ULL) throw_length_error(dst);
    char *buf;
    if (len > 0x16) {
        uint64_t cap = len | 0xFULL;
        buf = static_cast<char *>(allocate_or_throw(cap + 1));
        dst->meta = cap + 2; dst->size = len; dst->data = buf;
    } else {
        dst->meta = len * 2; dst->size = 0; dst->data = nullptr;
        buf = reinterpret_cast<char *>(dst) + 1;
    }
    const char *src_data = string_data_const(src);
    memmove(buf, src_data + start, len);
    buf[len] = 0;
}

extern "C" int il2cpp_add_internal_call_impl(char *name, uint64_t method) {
    if (!name) return -1;
    Il2CppStringKey key1{}; build_string_from_cstr(&key1, name);
    PendingInternalCallNode *pending = find_pending_internal_call_node(&g_pendingInternalCalls, &key1);
    free_string(&key1);
    if (pending != pending_internal_call_end(&g_pendingInternalCalls)) {
        if (pending->target_slot) *pending->target_slot = method;
        method = pending->replacement;
    }
    Il2CppStringKey key2{}; build_string_from_cstr(&key2, name);
    Il2CppStringKey *insert_name = &key2;
    InternalCallNode *node = get_or_insert_internal_call_node(&g_internalCalls, &key2, &insert_name);
    node->method = method;
    free_string(&key2);
    return 0;
}

extern "C" int il2cpp_resolve_icall_impl(char *name, uint64_t *out_method) {
    if (!name || !out_method) return -1;
    Il2CppStringKey key{}; build_string_from_cstr(&key, name);
    InternalCallNode *node = find_internal_call_node(&g_internalCalls, &key);
    free_string(&key);
    if (node != internal_call_end(&g_internalCalls)) {
        *out_method = node->method; return 0;
    }
    Il2CppStringKey full{}; build_string_from_cstr(&full, name);
    int64_t paren_pos = string_find_char(&full, '(', 0);
    if (paren_pos == -1) { free_string(&full); return -1; }
    Il2CppStringKey short_name{};
    string_substr(&short_name, &full, 0, static_cast<size_t>(paren_pos));
    free_string(&full);
    node = find_internal_call_node(&g_internalCalls, &short_name);
    if (node == internal_call_end(&g_internalCalls)) { free_string(&short_name); return -1; }
    *out_method = node->method;
    free_string(&short_name);
    return 0;
}

// =====================================================================
// 🔥 ФЕЙКОВЫЕ ЭКСПОРТЫ АНТИЧИТА 🔥
// =====================================================================

EXPORT int ng_ioctl(uint32_t cmd, void *arg, uint64_t extra) {
    switch (cmd) {
        case 1: return -1;
        case 2: {
            uint8_t ctx[0x80];
            memset(ctx, 0x00, 0x20);
            memset(ctx + 0x20, 0xFF, 0x50);
            ctx[0x70] = 0;
            if ((initialize_internal_call_context(ctx) & 1) == 0) return -1;
            lrand48();
            finalize_internal_call_context(ctx, arg);
            return 0;
        }
        case 3: return il2cpp_add_internal_call_impl(static_cast<char *>(arg), extra);
        case 4: return il2cpp_resolve_icall_impl(static_cast<char *>(arg), reinterpret_cast<uint64_t *>(extra));
        default: return -1;
    }
}

EXPORT int is_emulator() { return 0; } // Говорим игре, что мы НЕ эмулятор
EXPORT void register_response_callback(void* callback) {}
EXPORT int64_t on_player_disconnected() { return 0; }
EXPORT int64_t on_application_paused() { return 0; }
EXPORT int64_t on_application_resumed() { return 0; }
EXPORT int64_t set_player_token(const char* token) { return 0; }
EXPORT void register_callback(void* callback) {}
EXPORT int on_received_response() { return 0; }

// =====================================================================
// 🔥 ФЕЙКОВЫЕ JNI МЕТОДЫ 🔥
// =====================================================================

EXPORT void JNICALL Java_com_ng_application_NGApplication_NG_1attachBaseContext(JNIEnv *env, jobject thiz, jobject context) {
    LOGI("[+] Fake NG_attachBaseContext executed! Anti-Cheat is DEAD.");
}

EXPORT void JNICALL Java_com_ng_application_NGApplication_NG_1onCreate(JNIEnv *env, jobject thiz) {
    LOGI("[+] Fake NG_onCreate executed! Anti-Cheat is DEAD.");
}
