#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "cv_bridge.h"
#include "cvo_bridge.h"
#include "cvn_bridge.h"
#include "zira_tts.h"
#include "protalker_tts.h"

extern uint32_t _dyld_image_count(void);
extern const char *_dyld_get_image_name(uint32_t image_index);
extern intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index);

typedef void *(*fn_objc_getClass)(const char *);
typedef void *(*fn_sel_registerName)(const char *);
typedef void (*fn_dispatch_async_f)(void *, void *, void (*)(void *));
typedef int (*fn_copyfile)(const char *, const char *, void *, uint32_t);
typedef void (*fn_UIAccessibilityPostNotification)(uint32_t, void *);

static fn_objc_getClass p_getClass = NULL;
static fn_sel_registerName p_sel = NULL;
static void *p_msgSend = NULL;

static pthread_once_t g_group_once = PTHREAD_ONCE_INIT;
static char g_bundle_path[1024] = "";
static char g_bundle_ext_dir[1024] = "";
static char g_bundle_oc_dir[1024] = "";
static char g_bundle_nv_dir[1024] = "";
static char g_bundle_pt_dir[1024] = "";
static char g_group_path[1024] = "";
static char g_extra_oc_dir[1024] = "";
static char g_extra_nv_dir[1024] = "";
static char g_extra_pt_dir[1024] = "";

static void *msg0(void *obj, const char *sel)
{
    if (!obj || !p_msgSend || !p_sel) return NULL;
    return ((void *(*)(void *, void *))p_msgSend)(obj, p_sel(sel));
}

static void *msg1(void *obj, const char *sel, void *a1)
{
    if (!obj || !p_msgSend || !p_sel) return NULL;
    return ((void *(*)(void *, void *, void *))p_msgSend)(obj, p_sel(sel), a1);
}

static unsigned long msg_ul0(void *obj, const char *sel)
{
    if (!obj || !p_msgSend || !p_sel) return 0;
    return ((unsigned long (*)(void *, void *))p_msgSend)(obj, p_sel(sel));
}

static void *msg_idx(void *obj, const char *sel, unsigned long idx)
{
    if (!obj || !p_msgSend || !p_sel) return NULL;
    return ((void *(*)(void *, void *, unsigned long))p_msgSend)(obj, p_sel(sel), idx);
}

static void *nsstr(const char *utf8)
{
    void *cls;
    if (!utf8 || !p_getClass || !p_msgSend || !p_sel) return NULL;
    cls = p_getClass("NSString");
    if (!cls) return NULL;
    return ((void *(*)(void *, void *, const char *))p_msgSend)(cls, p_sel("stringWithUTF8String:"), utf8);
}

static const char *cstr(void *nsstring)
{
    if (!nsstring || !p_msgSend || !p_sel) return NULL;
    return ((const char *(*)(void *, void *))p_msgSend)(nsstring, p_sel("UTF8String"));
}

static void resolve_app_group_once(void)
{
    void *mainBundle, *groups, *groupID = NULL, *fm, *url;
    const char *bpath, *gpath;

    p_getClass = (fn_objc_getClass)dlsym(RTLD_DEFAULT, "objc_getClass");
    p_sel = (fn_sel_registerName)dlsym(RTLD_DEFAULT, "sel_registerName");
    p_msgSend = dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!p_getClass || !p_sel || !p_msgSend) return;

    mainBundle = msg0(p_getClass("NSBundle"), "mainBundle");
    if (!mainBundle) return;

    bpath = cstr(msg0(mainBundle, "bundlePath"));
    if (bpath && *bpath) {
        snprintf(g_bundle_path, sizeof g_bundle_path, "%s", bpath);
    }

    groups = msg1(mainBundle, "objectForInfoDictionaryKey:", nsstr("ALTAppGroups"));
    if (groups) {
        unsigned long n = msg_ul0(groups, "count"), k;
        for (k = 0; k < n; k++) {
            void *item = msg_idx(groups, "objectAtIndex:", k);
            const char *s = cstr(item);
            if (s && strncmp(s, "group.com.quinton.classicvoices", 31) == 0) {
                groupID = item;
                break;
            }
        }
        if (!groupID && n > 0) {
            groupID = msg_idx(groups, "objectAtIndex:", 0);
        }
    }
    if (!groupID) {
        groupID = nsstr("group.com.quinton.classicvoices");
    }

    fm = msg0(p_getClass("NSFileManager"), "defaultManager");
    url = msg1(fm, "containerURLForSecurityApplicationGroupIdentifier:", groupID);
    gpath = cstr(msg0(url, "path"));
    if (gpath && *gpath) {
        snprintf(g_group_path, sizeof g_group_path, "%s", gpath);
        snprintf(g_extra_oc_dir, sizeof g_extra_oc_dir, "%s/OneCoreVoice", gpath);
        snprintf(g_extra_nv_dir, sizeof g_extra_nv_dir, "%s/NeuralVoices", gpath);
        snprintf(g_extra_pt_dir, sizeof g_extra_pt_dir, "%s/ProTalkerVoice", gpath);
        zira_tts_set_extra_dir(g_extra_oc_dir);
        cvn_set_extra_dir(g_extra_nv_dir);
        cvn_set_extra_oc_dir(g_extra_oc_dir);
        protalker_tts_set_extra_dir(g_extra_pt_dir);
    }
}

static cvo_voice *wrapped_cvo_voice_open(const char *data_dir, const char *voice, char *err, size_t errlen)
{
    struct stat st;
    pthread_once(&g_group_once, resolve_app_group_once);
    if (!data_dir || stat(data_dir, &st) != 0) {
        if (g_bundle_oc_dir[0] && stat(g_bundle_oc_dir, &st) == 0) {
            data_dir = g_bundle_oc_dir;
        } else if (g_extra_oc_dir[0]) {
            data_dir = g_extra_oc_dir;
        }
    }
    return cvo_voice_open(data_dir, voice, err, errlen);
}

static cvn_voice *wrapped_cvn_voice_open(const char *sdk_dir, const char *data_dir, const char *onecore_dir,
                                         const char *work_dir, const char *voice, char *err, size_t errlen)
{
    struct stat st;
    pthread_once(&g_group_once, resolve_app_group_once);
    if (!onecore_dir || stat(onecore_dir, &st) != 0) {
        if (g_bundle_oc_dir[0] && stat(g_bundle_oc_dir, &st) == 0) {
            onecore_dir = g_bundle_oc_dir;
        } else if (g_extra_oc_dir[0]) {
            onecore_dir = g_extra_oc_dir;
        }
    }
    if (!data_dir || stat(data_dir, &st) != 0) {
        if (g_bundle_nv_dir[0] && stat(g_bundle_nv_dir, &st) == 0) {
            data_dir = g_bundle_nv_dir;
        } else if (g_extra_nv_dir[0]) {
            data_dir = g_extra_nv_dir;
        }
    }
    return cvn_voice_open(sdk_dir, data_dir, onecore_dir, work_dir, voice, err, errlen);
}

typedef struct {
    char title[256];
    char message[1024];
} alert_payload;

static void present_alert_main(void *ctx)
{
    alert_payload *p = (alert_payload *)ctx;
    void *avCls, *app, *windows, *rootVC = NULL;
    fn_UIAccessibilityPostNotification post_notif;
    uint32_t *ann_notif;

    if (!p) return;
    pthread_once(&g_group_once, resolve_app_group_once);
    if (!p_getClass || !p_sel || !p_msgSend) {
        free(p);
        return;
    }

    avCls = p_getClass("AVSpeechSynthesisProviderVoice");
    if (avCls) {
        msg0(avCls, "updateSpeechVoices");
    }

    post_notif = (fn_UIAccessibilityPostNotification)dlsym(RTLD_DEFAULT, "UIAccessibilityPostNotification");
    ann_notif = (uint32_t *)dlsym(RTLD_DEFAULT, "UIAccessibilityAnnouncementNotification");
    if (post_notif && ann_notif && *ann_notif) {
        char combined[1400];
        snprintf(combined, sizeof combined, "%s。%s", p->title, p->message);
        post_notif(*ann_notif, nsstr(combined));
    }

    app = msg0(p_getClass("UIApplication"), "sharedApplication");
    if (app) {
        windows = msg0(app, "windows");
        if (windows) {
            unsigned long wcount = msg_ul0(windows, "count"), k;
            for (k = 0; k < wcount; k++) {
                void *w = msg_idx(windows, "objectAtIndex:", k);
                void *rvc = msg0(w, "rootViewController");
                if (rvc) {
                    rootVC = rvc;
                    break;
                }
            }
        }
        if (!rootVC) {
            void *kw = msg0(app, "keyWindow");
            if (kw) rootVC = msg0(kw, "rootViewController");
        }
        while (rootVC) {
            void *presented = msg0(rootVC, "presentedViewController");
            if (!presented) break;
            rootVC = presented;
        }
        if (rootVC) {
            void *alertCls = p_getClass("UIAlertController");
            void *actCls = p_getClass("UIAlertAction");
            if (alertCls && actCls) {
                void *alert = ((void *(*)(void *, void *, void *, void *, long))p_msgSend)(
                    alertCls, p_sel("alertControllerWithTitle:message:preferredStyle:"),
                    nsstr(p->title), nsstr(p->message), 1L);
                void *act = ((void *(*)(void *, void *, void *, long, void *))p_msgSend)(
                    actCls, p_sel("actionWithTitle:style:handler:"),
                    nsstr("OK"), 0L, NULL);
                if (alert && act) {
                    msg1(alert, "addAction:", act);
                    ((void (*)(void *, void *, void *, int, void *))p_msgSend)(
                        rootVC, p_sel("presentViewController:animated:completion:"),
                        alert, 1, NULL);
                }
            }
        }
    }
    free(p);
}

static void show_ios_alert(const char *title, const char *message)
{
    void *main_q = dlsym(RTLD_DEFAULT, "_dispatch_main_q");
    fn_dispatch_async_f async_f = (fn_dispatch_async_f)dlsym(RTLD_DEFAULT, "dispatch_async_f");
    alert_payload *p;
    if (!main_q || !async_f) return;
    p = (alert_payload *)calloc(1, sizeof *p);
    if (!p) return;
    snprintf(p->title, sizeof p->title, "%s", title ? title : "");
    snprintf(p->message, sizeof p->message, "%s", message ? message : "");
    async_f(main_q, p, present_alert_main);
}

static int copy_file_fast(const char *src, const char *dst)
{
    struct stat st_src, st_dst;
    fn_copyfile p_copyfile;
    FILE *in, *out;
    uint8_t *buf;
    size_t n;

    if (stat(src, &st_src) != 0 || !S_ISREG(st_src.st_mode)) return -1;
    if (stat(dst, &st_dst) == 0 && S_ISREG(st_dst.st_mode) && st_dst.st_size == st_src.st_size && st_src.st_size > 0) {
        return 0; /* already copied */
    }
    unlink(dst);
    p_copyfile = (fn_copyfile)dlsym(RTLD_DEFAULT, "copyfile");
    if (p_copyfile) {
        /* COPYFILE_CLONE (1<<24) | COPYFILE_DATA (1<<3) */
        if (p_copyfile(src, dst, NULL, (1U << 24) | (1U << 3)) == 0) {
            return 0;
        }
    }
    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    buf = (uint8_t *)malloc(131072);
    if (!buf) {
        fclose(in);
        fclose(out);
        return -1;
    }
    while ((n = fread(buf, 1, 131072, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            free(buf);
            fclose(in);
            fclose(out);
            unlink(dst);
            return -1;
        }
    }
    free(buf);
    fclose(in);
    fclose(out);
    return 0;
}

static void copy_dir_recursive(const char *src_dir, const char *dst_dir, int *copied_count)
{
    DIR *d = opendir(src_dir);
    struct dirent *e;
    if (!d) return;
    mkdir(dst_dir, 0755);
    while ((e = readdir(d)) != NULL) {
        char sp[1024], dp[1024];
        struct stat st;
        if (e->d_name[0] == '.') continue;
        snprintf(sp, sizeof sp, "%s/%s", src_dir, e->d_name);
        snprintf(dp, sizeof dp, "%s/%s", dst_dir, e->d_name);
        if (stat(sp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            copy_dir_recursive(sp, dp, copied_count);
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file_fast(sp, dp) == 0 && copied_count) {
                (*copied_count)++;
            }
        }
    }
    closedir(d);
}

static void *sapple_app_worker(void *arg)
{
    char src_oc[1024], src_nv[1024], src_pt[1024], pack_file[1024], done_file[1024];
    char pack_id[128] = "default", pack_desc[256] = "音声パック";
    struct stat st;
    int copied = 0;
    FILE *fp;
    (void)arg;

    usleep(1200000); /* Wait 1.2s for UIApplicationMain and main window */
    pthread_once(&g_group_once, resolve_app_group_once);

    if (!g_bundle_path[0] && !g_bundle_ext_dir[0]) return NULL;
    if (!g_group_path[0]) {
        show_ios_alert("App Group が見つかりません",
                       "AltStore でインストールする際に Keep App Extensions を選択してください。");
        return NULL;
    }

    if (g_bundle_oc_dir[0]) {
        snprintf(src_oc, sizeof src_oc, "%s", g_bundle_oc_dir);
        snprintf(src_nv, sizeof src_nv, "%s", g_bundle_nv_dir);
        snprintf(src_pt, sizeof src_pt, "%s", g_bundle_pt_dir);
    } else {
        snprintf(src_oc, sizeof src_oc, "%s/PlugIns/ClassicVoicesExtension.appex/OneCoreVoice", g_bundle_path);
        snprintf(src_nv, sizeof src_nv, "%s/PlugIns/ClassicVoicesExtension.appex/NeuralVoices", g_bundle_path);
        snprintf(src_pt, sizeof src_pt, "%s/PlugIns/ClassicVoicesExtension.appex/ProTalkerVoice", g_bundle_path);
    }
    snprintf(pack_file, sizeof pack_file, "%s/.pack_info", src_oc);

    fp = fopen(pack_file, "rb");
    if (fp) {
        char line[384] = {0};
        if (fgets(line, sizeof line, fp)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            nl = strchr(line, '\r');
            if (nl) *nl = 0;
            char *sep = strchr(line, '|');
            if (sep) {
                *sep = 0;
                snprintf(pack_id, sizeof pack_id, "%s", line);
                snprintf(pack_desc, sizeof pack_desc, "%s", sep + 1);
            } else if (line[0]) {
                snprintf(pack_id, sizeof pack_id, "%s", line);
            }
        }
        fclose(fp);
    }

    /* Synchronize ProTalkerVoice first, then OneCoreVoice and NeuralVoices into shared AppGroup container */
    if (stat(src_pt, &st) == 0) copy_dir_recursive(src_pt, g_extra_pt_dir, &copied);
    if (stat(src_oc, &st) == 0) copy_dir_recursive(src_oc, g_extra_oc_dir, &copied);
    if (stat(src_nv, &st) == 0) copy_dir_recursive(src_nv, g_extra_nv_dir, &copied);

    snprintf(done_file, sizeof done_file, "%s/pack_done_%s.txt", g_group_path, pack_id);
    if (stat(done_file, &st) != 0) {
        char title[256], msg[768];
        fp = fopen(done_file, "wb");
        if (fp) {
            fprintf(fp, "copied=%d\n", copied);
            fclose(fp);
        }
        snprintf(title, sizeof title, "%s 登録完了", pack_desc);
        snprintf(msg, sizeof msg,
                 "%s の音声データ（%dファイル）を共有フォルダに保存しました。SappleおよびVoiceOverで使用できます。他の言語パックIPAを追加で上書きインストールしても、この言語パックの音声はそのまま残ります。",
                 pack_desc, copied);
        show_ios_alert(title, msg);
    }
    return NULL;
}

static char s_large_strs[16][96];
static int s_large_str_count = 0;

static void encode_small_str(uint64_t out[2], const char *s)
{
    size_t i, len = strlen(s);
    int is_ascii = 1;
    uint8_t buf[16];
    for (i = 0; i < len; i++) {
        if ((uint8_t)s[i] >= 0x80) {
            is_ascii = 0;
            break;
        }
    }
    if (len <= 15) {
        memset(buf, 0, 16);
        memcpy(buf, s, len);
        buf[15] = (uint8_t)((is_ascii ? 0xE0 : 0xA0) | len);
        memcpy(out, buf, 16);
    } else {
        int slot = -1;
        for (i = 0; i < (size_t)s_large_str_count; i++) {
            if (strcmp(&s_large_strs[i][32], s) == 0) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            slot = s_large_str_count < 15 ? s_large_str_count++ : 15;
            memset(s_large_strs[slot], 0, sizeof s_large_strs[slot]);
            snprintf(&s_large_strs[slot][32], sizeof(s_large_strs[slot]) - 32, "%s", s);
        }
        out[0] = (is_ascii ? 0xD000000000000000ULL : 0x1000000000000000ULL) | (uint64_t)len;
        out[1] = 0x8000000000000000ULL | ((uintptr_t)&s_large_strs[slot][32] - 0x20);
    }
}

static void write_sapi5_voice_def(uint8_t *dst, const char *slug, const char *display,
                                  const char *spd, double base_pitch, uint8_t female)
{
    uint64_t str[2];
    memset(dst, 0, 0xA0);
    encode_small_str(str, slug);
    memcpy(dst + 0x00, str, 16);
    encode_small_str(str, display);
    memcpy(dst + 0x10, str, 16);
    encode_small_str(str, spd);
    memcpy(dst + 0x20, str, 16);
    encode_small_str(str, "none");
    memcpy(dst + 0x30, str, 16);
    memcpy(dst + 0x40, &base_pitch, 8);
    dst[0x48] = female;
    dst[0x49] = 0;
    encode_small_str(str, "");
    memcpy(dst + 0x50, str, 16);
    memcpy(dst + 0x60, str, 16);
    memcpy(dst + 0x70, str, 16);
    memcpy(dst + 0x80, str, 16);
    encode_small_str(str, "en-US");
    memcpy(dst + 0x90, str, 16);
}

static void write_protalker_voice_def(uint8_t *dst, const char *slug, const char *display,
                                      const char *oc_voice, uint8_t female)
{
    uint64_t str[2];
    double zero = 0.0;
    memset(dst, 0, 0xA0);
    encode_small_str(str, slug);
    memcpy(dst + 0x00, str, 16);
    encode_small_str(str, display);
    memcpy(dst + 0x10, str, 16);
    encode_small_str(str, "");
    memcpy(dst + 0x20, str, 16);
    encode_small_str(str, "none");
    memcpy(dst + 0x30, str, 16);
    memcpy(dst + 0x40, &zero, 8);
    dst[0x48] = female;
    dst[0x49] = 4; /* .onecore -> routed to cvo_voice_open -> protalker_tts_open */
    encode_small_str(str, "");
    memcpy(dst + 0x50, str, 16);
    memcpy(dst + 0x60, str, 16);
    memcpy(dst + 0x80, str, 16);
    encode_small_str(str, oc_voice);
    memcpy(dst + 0x70, str, 16);
    encode_small_str(str, "ja-JP");
    memcpy(dst + 0x90, str, 16);
}

static void populate_host(uintptr_t fn_table_addr, uintptr_t orig_tv_addr, uintptr_t new_tv_addr)
{
    uintptr_t *fn_table = (uintptr_t *)fn_table_addr;
    const uint64_t *orig_tv = (const uint64_t *)orig_tv_addr;
    uint64_t *new_tv = (uint64_t *)new_tv_addr;
    uint8_t *elems = (uint8_t *)(new_tv_addr + 0x20);

    fn_table[0] = (uintptr_t)wrapped_cvo_voice_open;
    fn_table[1] = (uintptr_t)cvo_voice_close;
    fn_table[2] = (uintptr_t)cvo_voice_set_emotion;
    fn_table[3] = (uintptr_t)cvo_voice_speak;
    fn_table[4] = (uintptr_t)wrapped_cvn_voice_open;
    fn_table[5] = (uintptr_t)cvn_voice_close;
    fn_table[6] = (uintptr_t)cvn_voice_speak;
    fn_table[7] = (uintptr_t)cvn_speed_factor;

    new_tv[0] = orig_tv[0];
    new_tv[1] = orig_tv[1];
    new_tv[2] = 9;
    new_tv[3] = 18;

    write_sapi5_voice_def(elems + 0 * 0xA0, "betty",   "Betty (SAPI 5)",   "Mary", 110.0, 1);
    write_sapi5_voice_def(elems + 1 * 0xA0, "beverly", "Beverly (SAPI 5)", "Mary", 100.0, 1);
    write_sapi5_voice_def(elems + 2 * 0xA0, "brutus",  "Brutus (SAPI 5)",  "Mike", 100.0, 0);
    write_sapi5_voice_def(elems + 3 * 0xA0, "freddy",  "Freddy (SAPI 5)",  "Mike", 189.0, 0);
    write_sapi5_voice_def(elems + 4 * 0xA0, "jack",    "Jack (SAPI 5)",    "Sam",  110.0, 0);
    write_sapi5_voice_def(elems + 5 * 0xA0, "junior",  "Junior (SAPI 5)",  "Sam",  189.0, 0);
    memcpy(elems + 6 * 0xA0, (const uint8_t *)orig_tv_addr + 0x20, 0xA0);
    write_protalker_voice_def(elems + 7 * 0xA0, "protalk_hide", "ProTalker 読み秀君",     "ProTalker_Hide", 0);
    write_protalker_voice_def(elems + 8 * 0xA0, "protalk_ko",   "ProTalker 読み子ちゃん", "ProTalker_Ko",   1);
}

static int ends_with(const char *str, const char *suffix)
{
    size_t slen, suflen;
    if (!str || !suffix) return 0;
    slen = strlen(str);
    suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return memcmp(str + slen - suflen, suffix, suflen) == 0;
}

static void init_bundle_dirs_from_exec(const char *exec_path, int is_app)
{
    const char *slash;
    size_t dir_len;
    char base_dir[1024];
    if (!exec_path || !*exec_path) return;
    slash = strrchr(exec_path, '/');
    if (!slash) return;
    dir_len = (size_t)(slash - exec_path);
    if (dir_len >= sizeof base_dir) dir_len = sizeof(base_dir) - 1;
    memcpy(base_dir, exec_path, dir_len);
    base_dir[dir_len] = '\0';

    snprintf(g_bundle_path, sizeof g_bundle_path, "%s", base_dir);
    if (is_app) {
        snprintf(g_bundle_ext_dir, sizeof g_bundle_ext_dir, "%s/PlugIns/ClassicVoicesExtension.appex", base_dir);
    } else {
        snprintf(g_bundle_ext_dir, sizeof g_bundle_ext_dir, "%s", base_dir);
    }
    snprintf(g_bundle_oc_dir, sizeof g_bundle_oc_dir, "%s/OneCoreVoice", g_bundle_ext_dir);
    snprintf(g_bundle_nv_dir, sizeof g_bundle_nv_dir, "%s/NeuralVoices", g_bundle_ext_dir);
    snprintf(g_bundle_pt_dir, sizeof g_bundle_pt_dir, "%s/ProTalkerVoice", g_bundle_ext_dir);
    protalker_tts_set_bundle_dir(g_bundle_pt_dir);
}

__attribute__((constructor))
static void sapple_multilang_init(void)
{
    uint32_t i, count = _dyld_image_count();
    for (i = 0; i < count; i++) {
        const char *name = _dyld_get_image_name(i);
        if (!name) continue;
        if (ends_with(name, "/Sapple")) {
            pthread_t tid;
            intptr_t slide = _dyld_get_image_vmaddr_slide(i);
            init_bundle_dirs_from_exec(name, 1);
            populate_host(0x10026ec00ULL + slide, 0x1001352e0ULL + slide, 0x10026ed00ULL + slide);
            if (pthread_create(&tid, NULL, sapple_app_worker, NULL) == 0) {
                pthread_detach(tid);
            }
        } else if (ends_with(name, "/ClassicVoicesExtension")) {
            intptr_t slide = _dyld_get_image_vmaddr_slide(i);
            init_bundle_dirs_from_exec(name, 0);
            populate_host(0x100262600ULL + slide, 0x100128c40ULL + slide, 0x100262700ULL + slide);
        }
    }
}
