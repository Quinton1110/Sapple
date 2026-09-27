// Core of the Win32 environment: memory, thunks, heap, threads/scheduler, guest calls, PE loader.
#include "emu_internal.h"
#include <dirent.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <pthread.h>

int emu_trace_api = 0;

static void emu_failf(Emu *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->error, sizeof e->error, fmt, ap);
    va_end(ap);
    e->failed = 1;
}

X86 *emu_cpu(Emu *e) { return &e->cpu; }
int emu_failed(Emu *e) { return e->failed; }
const char *emu_error(Emu *e) { return e->error; }
EmuStats emu_stats(Emu *e) { EmuStats s = e->stats; s.insns = e->cpu.icount; return s; }

void *emu_ptr(Emu *e, uint32_t a, uint32_t n) { return x86_ptr(&e->cpu, a, n); }

const char *emu_str(Emu *e, uint32_t a) {
    X86 *c = &e->cpu;
    const char *p = (const char *)x86_ptr(c, a, 1);
    size_t max = e->mem_size - a;
    if (!memchr(p, 0, max)) x86_fault(c, "unterminated string at %08x", a);
    return p;
}
const uint16_t *emu_wstr(Emu *e, uint32_t a, size_t *len) {
    X86 *c = &e->cpu;
    const uint16_t *p = (const uint16_t *)(void *)x86_ptr(c, a, 2);
    size_t n = 0, max = (e->mem_size - a) / 2;
    while (n < max && p[n]) n++;
    if (n == max) x86_fault(c, "unterminated wide string at %08x", a);
    if (len) *len = n;
    return p;
}

// ------------------------------------------------------------------ static + heap
uint32_t emu_static(Emu *e, uint32_t size) {
    uint32_t a = (e->static_next + 15) & ~15u;
    if (a + size > EMU_STATIC_END) x86_fault(&e->cpu, "static area exhausted");
    e->static_next = a + size;
    memset(e->mem + a, 0, size);
    return a;
}

#define HEAP_MAGIC 0x48454150u
static int heap_class(uint32_t size, uint32_t *cap) {
    if (size <= 1024) { uint32_t c = (size + 15) & ~15u; if (!c) c = 16; *cap = c; return (int)(c / 16) - 1; } // 0..63? limited below
    return -1;
}

uint32_t emu_malloc(Emu *e, uint32_t size) {
    X86 *c = &e->cpu;
    uint32_t cap = 0;
    int k = size <= 512 ? heap_class(size, &cap) : -1;   // classes 0..31 (16..512 bytes)
    if (k >= 0 && e->heap_free[k]) {
        uint32_t b = e->heap_free[k];
        e->heap_free[k] = rd32(c, b);
        wr32(c, b - 4, HEAP_MAGIC);
        e->heap_live += cap;
        return b;
    }
    if (k < 0) {
        cap = (size + 4095) & ~4095u;
        if (size <= 65536) { cap = 1024; while (cap < size) cap <<= 1; } // power-of-two buckets for mid sizes
        // first fit among large free blocks
        uint32_t prev = 0, b = e->heap_large;
        while (b) {
            uint32_t bcap = rd32(c, b - 8), next = rd32(c, b);
            if (bcap >= cap && bcap <= cap * 2 + 65536) {
                if (prev) wr32(c, prev, next); else e->heap_large = next;
                wr32(c, b - 4, HEAP_MAGIC);
                e->heap_live += bcap;
                return b;
            }
            prev = b; b = next;
        }
    }
    uint32_t hdr = (e->heap_next + 15) & ~15u;
    uint32_t b = hdr + 16;   // 16-byte aligned user pointer, header in the 8 bytes before it
    if ((uint64_t)b + cap > e->mem_size) x86_fault(c, "guest heap exhausted (%u bytes requested)", size);
    e->heap_next = b + cap;
#ifndef CV_NO_DEBUG_ENV
    { static int fill = -2; if (fill == -2) { const char *f = getenv("SAPI4_HEAP_FILL"); fill = f ? (int)strtol(f, NULL, 0) : -1; }
      if (fill >= 0) memset(e->mem + b, fill, cap); }
#endif
    wr32(c, b - 8, cap);
    wr32(c, b - 4, HEAP_MAGIC);
    e->heap_live += cap;
    return b;
}

void emu_free(Emu *e, uint32_t p) {
    X86 *c = &e->cpu;
    if (!p) return;
    if (p < EMU_HEAP_BASE + 16 || p >= e->heap_next || rd32(c, p - 4) != HEAP_MAGIC) {
        // The Win32 heap rejects invalid pointers silently (HeapFree returns FALSE) and MSVCRT's free
        // ignores that. The 1999 Microsoft engine relies on it: ITTSEnumA::Next frees its temporary
        // buffer after advancing the pointer by one TTSMODEINFOW.
        if (emu_trace_api) EMU_LOG("[emu] free(%08x): not a live heap block, ignored (as Win32 does)\n", p);
        return;
    }
    uint32_t cap = rd32(c, p - 8);
    wr32(c, p - 4, 0);
    e->heap_live -= cap;
    if (cap <= 512 && (cap & 15) == 0) {
        int k = (int)(cap / 16) - 1;
        wr32(c, p, e->heap_free[k]);
        e->heap_free[k] = p;
    } else {
        wr32(c, p, e->heap_large);
        e->heap_large = p;
    }
}

uint32_t emu_realloc(Emu *e, uint32_t p, uint32_t size) {
    X86 *c = &e->cpu;
    if (!p) return emu_malloc(e, size);
    if (!size) { emu_free(e, p); return 0; }
    if (rd32(c, p - 4) != HEAP_MAGIC) x86_fault(c, "realloc of invalid pointer %08x", p);
    uint32_t cap = rd32(c, p - 8);
    if (size <= cap) return p;
    uint32_t n = emu_malloc(e, size);
    memmove(e->mem + n, e->mem + p, cap);
    emu_free(e, p);
    return n;
}

uint32_t emu_strdup(Emu *e, const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1, a = emu_malloc(e, n);
    memcpy(e->mem + a, s, n);
    return a;
}

// ------------------------------------------------------------------ thunks / host calls
static void host_dispatch(X86 *c, uint32_t idx) {
    Emu *e = (Emu *)c->user;
    if (idx >= (uint32_t)e->nhosts) x86_fault(c, "bad host call index %u", idx);
    HostEntry *h = &e->hosts[idx];
    h->calls++;
    e->stats.host_calls++;
    if (emu_trace_api) {
        EMU_LOG("[T%d] %s(%08x, %08x, %08x, %08x) ret=%08x\n", e->cur ? e->cur->id : 0, h->name,
                x86_arg(c, 0), x86_arg(c, 1), x86_arg(c, 2), x86_arg(c, 3), rd32(c, c->s.r[ESP]));
    }
    e->retry = 0;
    h->fn(e, c);
    if (e->retry) return;
    if (h->argbytes >= 0) x86_ret(c, (uint32_t)h->argbytes);
}

uint32_t emu_thunk(Emu *e, const char *name, EmuHostFn fn, int argbytes) {
    if (e->nhosts >= EMU_THUNK_MAX) x86_fault(&e->cpu, "too many thunks");
    int i = e->nhosts++;
    e->hosts[i] = (HostEntry){ name, fn, argbytes, 0, 0 };
    uint32_t a = EMU_THUNK_BASE + 4u * (uint32_t)i;
    e->mem[a] = 0xF1; e->mem[a + 1] = (uint8_t)i; e->mem[a + 2] = (uint8_t)(i >> 8); e->mem[a + 3] = 0xC3;
    return a;
}

uint32_t emu_vtable(Emu *e, const EmuMethod *methods, int n) {
    uint32_t vt = emu_static(e, 4u * (uint32_t)n);
    for (int i = 0; i < n; i++) wr32(&e->cpu, vt + 4u * (uint32_t)i, emu_thunk(e, methods[i].name, methods[i].fn, 4 * (methods[i].nargs + 1)));
    return vt;
}

void emu_block(Emu *e) {
    e->retry = 1;
    e->stop_reason = STOP_BLOCK;
    e->cpu.stop = 1;
}

// ------------------------------------------------------------------ threads
static void h_return(Emu *e, X86 *c) {
    Frame *f = e->nframes ? &e->frames[e->nframes - 1] : NULL;
    uint32_t esp = c->s.r[ESP];
    e->retry = 1;   // EIP stays on the thunk
    if (f && f->thread == e->cur && esp >= f->esp_lo && esp <= f->esp_hi) {
        e->stop_reason = STOP_RETURN;
        c->stop = 1;
        return;
    }
    // this thread returned into a host frame that is not the innermost one: park it
    e->cur->state = T_WAIT_RETURN;
    e->stop_reason = STOP_BLOCK;
    c->stop = 1;
}

static void h_thread_exit(Emu *e, X86 *c) {
    Thread *t = e->cur;
    t->exit_code = c->s.r[EAX];
    t->state = T_DEAD;
    if (t->handle_closed) {   // TruVoice mode: its handle was closed while it ran
        Handle *h = emu_handle(e, t->handle);
        if (h && h->type == H_THREAD && h->thread == t) h->type = H_FREE;
    }
    e->gen++;
    e->retry = 1;
    e->stop_reason = STOP_EXIT;
    c->stop = 1;
    if (emu_trace_api) EMU_LOG("[T%d] thread exit %u\n", t->id, t->exit_code);
}

Thread *emu_new_thread(Emu *e, uint32_t start, uint32_t arg, uint32_t stack_hint) {
    (void)stack_hint;
    X86 *c = &e->cpu;
    Thread *t;
    uint32_t teb = 0;
    if (e->nthreads < EMU_MAX_THREADS) {
        t = &e->threads[e->nthreads];
        mq_free(&t->mq);
        memset(t, 0, sizeof *t);
        t->id = ++e->nthreads;
    } else {
        // all slots used: with emu_enable_thread_queues (engines that start a thread per utterance) the
        // slot of an exited thread is reused - same id, stack and TEB; a still-open handle to the old
        // thread stays signalled
        Thread *old = NULL;
        for (int k = 0; k < e->nthreads && e->thread_queues; k++) if (e->threads[k].state == T_DEAD && &e->threads[k] != e->main) { old = &e->threads[k]; break; }
        if (!old) x86_fault(c, "too many threads");
        Handle *h = emu_handle(e, old->handle);
        if (h && h->type == H_THREAD && h->thread == old) { h->thread = NULL; h->signaled = 1; }
        int id = old->id;
        teb = old->teb;
        mq_free(&old->mq);
        memset(old, 0, sizeof *old);
        t = old;
        t->id = id;
    }
    t->stack_lo = EMU_STACK_BASE + EMU_STACK_SIZE * (uint32_t)(t->id - 1);
    t->stack_hi = t->stack_lo + EMU_STACK_SIZE;
    t->teb = teb ? teb : emu_static(e, 0x1000);
    if (teb) memset(e->mem + teb, 0, 0x1000);
    wr32(c, t->teb + 0x00, 0xFFFFFFFFu);        // SEH chain end
    wr32(c, t->teb + 0x04, t->stack_hi);
    wr32(c, t->teb + 0x08, t->stack_lo);
    wr32(c, t->teb + 0x18, t->teb);
    wr32(c, t->teb + 0x24, (uint32_t)t->id);
    t->rand_seed = 1;
    memset(&t->st, 0, sizeof t->st);
    t->st.fs_base = t->teb;
    t->st.fpu.cw = 0x027F;
    for (int i = 0; i < 8; i++) t->st.fpu.empty[i] = 1;
    uint32_t sp = t->stack_hi - 64;
#ifndef CV_NO_DEBUG_ENV
    { const char *f = getenv("SAPI4_STACK_FILL"); if (f) memset(e->mem + t->stack_lo, (int)strtol(f, NULL, 0), EMU_STACK_SIZE - 64); }
#endif
    if (start) {
        sp -= 4; wr32(c, sp, arg);
        sp -= 4; wr32(c, sp, e->thunk_thread_exit);
        t->st.eip = start;
    }
    t->st.r[ESP] = sp;
    t->state = T_RUNNABLE;
    t->handle = emu_new_handle(e, H_THREAD);
    emu_handle(e, t->handle)->thread = t;
    e->stats.threads_created++;
    return t;
}

static void switch_to(Emu *e, Thread *t) {
    if (e->cur == t) return;
    if (e->cur) e->cur->st = e->cpu.s;
    e->cpu.s = t->st;
    e->cur = t;
    e->stats.switches++;
}

// choose the next thread to run; pump = host is idle waiting (main thread parked as T_HOST)
static Thread *pick_next(Emu *e, int pump) {
    int n = e->nthreads;
    int start = e->cur ? e->cur->id % n : 0;
    for (int k = 0; k < n; k++) {
        Thread *t = &e->threads[(start + k) % n];
        if (t->state == T_RUNNABLE) return t;
        if (t->state == T_BLOCKED && t->gen != e->gen) { t->state = T_RUNNABLE; return t; }
        if (t->state == T_WAIT_RETURN && e->nframes && e->frames[e->nframes - 1].thread == t) { t->state = T_RUNNABLE; return t; }
    }
    // nothing runnable: expire the earliest finite timeout (virtual time)
    if (pump && e->idle_spins > 64) return NULL;
    Thread *best = NULL;
    for (int k = 0; k < n; k++) {
        Thread *t = &e->threads[k];
        if (t->state == T_BLOCKED && t->wake_at && (!best || t->wake_at < best->wake_at)) best = t;
    }
    if (best) {
        if (best->wake_at > e->vtime) e->vtime = best->wake_at;
        best->timed_out = 1;
        best->state = T_RUNNABLE;
        e->idle_spins++;
    }
    return best;
}

// run guest threads until the innermost frame returns (frame != 0) or, in pump mode, until idle.
// heuristic guest backtrace: return addresses on the stack that follow a call instruction
void emu_backtrace(Emu *e) {
#ifdef CV_NO_DEBUG_ENV
    (void)e;
#else
    X86 *c = &e->cpu;
    uint32_t sp = c->s.r[ESP];
    fprintf(stderr, "guest backtrace (T%d, EIP %08x):", e->cur ? e->cur->id : 0, c->s.eip);
    int n = 0;
    for (uint32_t a = sp; a < sp + 0x4000 && a + 4 <= e->mem_size && n < 40; a += 4) {
        uint32_t v = rd32(c, a);
        for (int i = 0; i < e->nmodules; i++) {
            EmuModule *m = &e->modules[i];
            if (v > m->base + 0x1000 && v < m->base + m->size) {
                uint8_t *p = e->mem + v;
                if (p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
                    (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)) {
                    fprintf(stderr, " %08x(%s %08x)", v, m->name, v - m->base + m->pref_base);
                    n++;
                }
            }
        }
    }
    fprintf(stderr, "\n");
#endif
}

void emu_abort_pump(Emu *e) { e->abort = 1; }
void emu_clear_abort(Emu *e) { e->abort = 0; }
int emu_aborted(Emu *e) { return e->abort; }
void emu_set_insn_budget(Emu *e, uint64_t insns) {
    e->insn_limit = (!insns || insns > UINT64_MAX - e->cpu.icount) ? UINT64_MAX : e->cpu.icount + insns;
}
void emu_set_user(Emu *e, void *user) { e->user = user; }
void *emu_user(Emu *e) { return e->user; }

static int run_loop(Emu *e, int pump) {
    X86 *c = &e->cpu;
    for (;;) {
        e->stop_reason = STOP_NONE;
        int rc = x86_run(c, e->insn_limit > c->icount ? e->insn_limit - c->icount : 0);
        if (rc == 1) { emu_backtrace(e); x86_fault(c, "instruction limit reached (watchdog)"); }
        if (rc < 0) return -1;
        if (e->stop_reason == STOP_RETURN) return 0;
        if (e->stop_reason == STOP_NONE) x86_fault(c, "interpreter stopped without reason");
        if (pump && e->abort) return 1;
        if (pump && e->host_msg) { e->host_msg = 0; return 1; }   // thread-queue mode: deliver it now
        if (e->stop_reason == STOP_BLOCK || e->stop_reason == STOP_EXIT || e->stop_reason == STOP_YIELD) {
            Thread *t = pick_next(e, pump);
            if (!t) {
                if (pump) return 1;
                // deadlock: describe it
                char buf[256]; int o = 0;
                for (int k = 0; k < e->nthreads; k++)
                    o += snprintf(buf + o, sizeof buf - (size_t)o, " T%d:%d@%08x", e->threads[k].id, e->threads[k].state,
                                  e->threads[k].id == e->cur->id ? c->s.eip : e->threads[k].st.eip);
                x86_fault(c, "deadlock: no runnable guest thread (%s)", buf);
            }
            switch_to(e, t);
        }
    }
}

static void rethrow(Emu *e) {
    char msg[512];
    snprintf(msg, sizeof msg, "%s", e->cpu.fault_msg);
    x86_fault(&e->cpu, "%s", msg);
}

uint32_t emu_call(Emu *e, uint32_t fn, int nargs, const uint32_t *args) {
    X86 *c = &e->cpu;
    if (e->failed) return 0;
    Thread *t = e->cur;
    int saved_state = t->state;
    t->state = T_RUNNABLE;
    uint32_t saved_eip = c->s.eip, saved_esp = c->s.r[ESP];
    int saved_stop = c->stop, saved_reason = e->stop_reason, saved_retry = e->retry;
    for (int i = nargs - 1; i >= 0; i--) x86_push(c, args[i]);
    x86_push(c, e->thunk_return);
    c->s.eip = fn;
    if (e->nframes >= 64) x86_fault(c, "guest call nesting too deep");
    Frame *f = &e->frames[e->nframes++];
    f->thread = t;
    f->esp_lo = saved_esp - 4u * (uint32_t)nargs;
    f->esp_hi = saved_esp;
    e->run_depth++;
    int rc;
    if (!c->fault_jmp) {
        // top level: catch faults raised by the scheduler itself (outside x86_run)
        jmp_buf jb;
        c->fault_jmp = &jb;
        if (setjmp(jb)) rc = -1;
        else rc = run_loop(e, 0);
        c->fault_jmp = NULL;
    } else rc = run_loop(e, 0);
    e->run_depth--;
    e->nframes--;
    if (rc < 0) {
        if (c->fault_jmp) rethrow(e);
        emu_failf(e, "guest fault: %s", c->fault_msg);
#ifndef CV_NO_DEBUG_ENV
        x86_dump(c);
#endif
        return 0;
    }
    if (e->cur != t) x86_fault(c, "returned on wrong thread");
    uint32_t r = c->s.r[EAX];
    c->s.eip = saved_eip;
    c->s.r[ESP] = saved_esp;
    c->stop = saved_stop;
    e->stop_reason = saved_reason;
    e->retry = saved_retry;   // the nested return thunk sets retry; it must not leak into the calling host fn
    t->state = saved_state;
    return r;
}

uint32_t emu_com(Emu *e, uint32_t obj, int method, int nargs, const uint32_t *args) {
    X86 *c = &e->cpu;
    if (e->failed) return 0x80004005u;
    uint32_t vt = rd32(c, obj), fn = rd32(c, vt + 4u * (uint32_t)method);
    uint32_t a[24];
    a[0] = obj;
    for (int i = 0; i < nargs; i++) a[i + 1] = args[i];
    return emu_call(e, fn, nargs + 1, a);
}

void emu_set_idle_hook(Emu *e, int (*hook)(Emu *, void *), void *ctx) { e->idle_hook = hook; e->idle_ctx = ctx; }
void emu_enable_thread_queues(Emu *e) { e->thread_queues = 1; }

int mq_push(MsgQ *q, Msg m) {
    if (q->head + q->n == q->cap) {
        if (q->head) { memmove(q->m, q->m + q->head, sizeof(Msg) * (size_t)q->n); q->head = 0; }
        else {
            int cap = q->cap ? q->cap * 2 : 256;
            if (cap > 65536) return 0;
            Msg *nm = realloc(q->m, sizeof(Msg) * (size_t)cap);
            if (!nm) return 0;
            q->m = nm;
            q->cap = cap;
        }
    }
    q->m[q->head + q->n++] = m;
    return 1;
}
int mq_take(MsgQ *q, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove, Msg *out) {
    for (int i = 0; i < q->n; i++) {
        Msg *m = &q->m[q->head + i];
        if (hwnd && m->hwnd != hwnd) continue;
        if ((lo || hi) && (m->msg < lo || m->msg > hi) && m->msg != 0x12) continue;   // WM_QUIT always passes
        *out = *m;
        if (remove) {
            if (i == 0) q->head++;
            else memmove(m, m + 1, sizeof(Msg) * (size_t)(q->n - i - 1));
            if (--q->n == 0) q->head = 0;
        }
        return 1;
    }
    return 0;
}
void mq_free(MsgQ *q) { free(q->m); memset(q, 0, sizeof *q); }

void emu_post(Emu *e, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!mq_push(&e->gq, (Msg){ hwnd, msg, wp, lp })) x86_fault(&e->cpu, "message queue overflow");
    e->idle_spins = 0;
}

static uint32_t wndproc_of(Emu *e, uint32_t hwnd) {
    for (int i = 0; i < e->nwindows; i++) if (e->windows[i].hwnd == hwnd && e->windows[i].alive) return e->windows[i].wndproc;
    return 0;
}

void emu_pump(Emu *e) {
    X86 *c = &e->cpu;
    if (e->failed) return;
    Thread *main = e->main;
    jmp_buf jb;
    jmp_buf *prev = c->fault_jmp;
    c->fault_jmp = &jb;
    if (setjmp(jb)) {
        c->fault_jmp = prev;
        emu_failf(e, "guest fault: %s", c->fault_msg);
#ifndef CV_NO_DEBUG_ENV
        x86_dump(c);
#endif
        return;
    }
    for (int guard = 0; guard < 1000000; guard++) {
        if (e->abort) break;
        uint32_t g0 = e->gen;
        main->state = T_HOST;
        Thread *t = pick_next(e, 1);
        if (t) {
            switch_to(e, t);
            e->run_depth++;
            int rc = run_loop(e, 1);
            e->run_depth--;
            if (rc < 0) {
                c->fault_jmp = prev;
                emu_failf(e, "guest fault: %s", c->fault_msg);
#ifndef CV_NO_DEBUG_ENV
                x86_dump(c);
#endif
                return;
            }
        }
        switch_to(e, main);
        main->state = T_RUNNABLE;
        if (e->abort) break;
        if (e->gen != g0) e->idle_spins = 0;
        if (e->gq.n) {
            Msg m;
            mq_take(&e->gq, 0, 0, 0, 1, &m);
            uint32_t wp = wndproc_of(e, m.hwnd);
            e->stats.messages++;
            if (emu_trace_api) EMU_LOG("[pump] dispatch hwnd=%x msg=%x wp=%x lp=%x -> %08x\n", m.hwnd, m.msg, m.wp, m.lp, wp);
            if (wp) { uint32_t a[4] = { m.hwnd, m.msg, m.wp, m.lp }; emu_call(e, wp, 4, a); }
            if (e->failed) break;
            continue;
        }
        if (t) continue;   // threads made progress; look again
        if (e->idle_hook && e->idle_hook(e, e->idle_ctx)) { e->idle_spins = 0; continue; }
        break;
    }
    c->fault_jmp = prev;
}

// ------------------------------------------------------------------ handles
uint32_t emu_new_handle(Emu *e, int type) {
    for (int i = 1; i < EMU_MAX_HANDLES; i++) {
        if (e->handles[i].type == H_FREE) {
            memset(&e->handles[i], 0, sizeof e->handles[i]);
            e->handles[i].type = type;
            return 0x100u + 4u * (uint32_t)i;
        }
    }
    x86_fault(&e->cpu, "out of handles");
}
Handle *emu_handle(Emu *e, uint32_t h) {
    if (h < 0x100 || (h & 3)) return NULL;
    uint32_t i = (h - 0x100) / 4;
    if (i >= EMU_MAX_HANDLES || e->handles[i].type == H_FREE) return NULL;
    return &e->handles[i];
}
void emu_close_handle(Emu *e, uint32_t h) {
    Handle *p = emu_handle(e, h);
    if (!p) return;
    if (p->type == H_FILE && p->fp) fclose(p->fp);
    if (p->type == H_FIND) { for (int i = 0; i < p->count; i++) free(p->names[i]); free(p->names); }
    if (p->type == H_THREAD && (!e->thread_queues || (p->thread && p->thread->state != T_DEAD))) {
        // keep thread handles alive (cheap); TruVoice mode frees them once the thread has exited
        if (p->thread && e->thread_queues) p->thread->handle_closed = 1;
        return;
    }
    p->type = H_FREE;
}

void emu_set_last_error(Emu *e, uint32_t err) { if (e->cur) e->cur->last_error = err; }

// ------------------------------------------------------------------ paths
void emu_map_dir(Emu *e, const char *win_prefix, const char *host_dir) {
    VDir *v = &e->vdirs[e->nvdirs++];
    snprintf(v->win, sizeof v->win, "%s", win_prefix);
    snprintf(v->host, sizeof v->host, "%s", host_dir);
}

// map a Windows path to a host path; each component is matched case-insensitively
int emu_host_path(Emu *e, const char *win_path, char *out, size_t outsz, int must_exist) {
    char p[520];
    snprintf(p, sizeof p, "%s", win_path);
    for (char *q = p; *q; q++) if (*q == '/') *q = '\\';
    for (int i = 0; i < e->nvdirs; i++) {
        VDir *v = &e->vdirs[i];
        size_t n = strlen(v->win);
        if (strncasecmp(p, v->win, n) == 0 && (p[n] == '\\' || p[n] == 0)) {
            char cur[1100];
            snprintf(cur, sizeof cur, "%s", v->host);
            char *rest = p + n;
            while (*rest == '\\') rest++;
            while (*rest) {
                char comp[260];
                size_t k = 0;
                while (rest[k] && rest[k] != '\\' && k < sizeof comp - 1) { comp[k] = rest[k]; k++; }
                comp[k] = 0;
                rest += k;
                while (*rest == '\\') rest++;
                // find case-insensitive match
                DIR *d = opendir(cur);
                int found = 0;
                if (d) {
                    struct dirent *de;
                    while ((de = readdir(d))) {
                        if (strcasecmp(de->d_name, comp) == 0) {
                            size_t L = strlen(cur);
                            snprintf(cur + L, sizeof cur - L, "/%s", de->d_name);
                            found = 1;
                            break;
                        }
                    }
                    closedir(d);
                }
                if (!found) {
                    if (must_exist) return 0;
                    size_t L = strlen(cur);
                    snprintf(cur + L, sizeof cur - L, "/%s", comp);
                }
            }
            snprintf(out, outsz, "%s", cur);
            return 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------------ API registry
// Filled once per process (pthread_once in emu_create), read-only afterwards: engines may be created
// and run concurrently on different host threads, each with its own Emu.
static ApiDef g_apis[1024];
static int g_napis;
void emu_add_apis(const ApiDef *defs, int n) {
    for (int i = 0; i < n && g_napis < 1024; i++) g_apis[g_napis++] = defs[i];
}
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void register_all_apis(void) {
    emu_register_crt_apis();
    emu_register_winapi_apis();
    emu_register_ole_apis();
    emu_register_registry_apis();
#ifndef CV_NO_DEBUG_ENV
    const char *tr = getenv("SAPI4_TRACE_API");
    emu_trace_api = tr && *tr == '1';
#endif
}

static void h_unimplemented(Emu *e, X86 *c) {
    (void)e;
    uint32_t idx = (c->s.eip - EMU_THUNK_BASE) / 4;
    x86_fault(c, "unimplemented import %s called", e->hosts[idx].name);
}

EmuHostFn emu_lookup_import(Emu *e, const char *dll, const char *name, int *argbytes, uint32_t *data_addr) {
    *data_addr = 0;
    if (strcasecmp(dll, "MSVCRT.dll") == 0) {
        if (!strcmp(name, "_pctype")) { *data_addr = e->pctype_var; return NULL; }
        if (!strcmp(name, "__mb_cur_max")) { *data_addr = e->mb_cur_max; return NULL; }
        if (!strcmp(name, "_adjust_fdiv")) { *data_addr = e->adjust_fdiv; return NULL; }
        uint32_t d = emu_crt_data_import(e, name);
        if (d) { *data_addr = d; return NULL; }
    }
    for (int i = 0; i < g_napis; i++) {
        if (strcasecmp(g_apis[i].dll, dll) == 0 && strcmp(g_apis[i].name, name) == 0) {
            *argbytes = g_apis[i].argbytes;
            return g_apis[i].fn;
        }
    }
    // an already-loaded guest module of that name (e.g. the real MSVCP50.dll): link to its export
    for (int i = 0; i < e->nmodules; i++) {
        if (strcasecmp(e->modules[i].name, dll)) continue;
        uint32_t a = 0;
        const char *bang = strchr(name, '!');   // ordinal imports arrive as "DLL!#n"
        if (bang) { if (bang[1] == '#') a = emu_get_export_ord(e, &e->modules[i], (uint32_t)strtoul(bang + 2, NULL, 10)); }
        else a = emu_get_export(e, &e->modules[i], name);
        if (a) { *data_addr = a; return NULL; }
    }
    *argbytes = 0;
    return h_unimplemented;
}

// ------------------------------------------------------------------ PE loader
EmuModule *emu_load_pe(Emu *e, const char *host_path, const char *win_path, uint32_t load_base) {
    return emu_load_pe_ex(e, host_path, win_path, load_base, 0);
}

EmuModule *emu_load_pe_ex(Emu *e, const char *host_path, const char *win_path, uint32_t load_base, int flags) {
    X86 *c = &e->cpu;
    if (e->nmodules >= (int)(sizeof e->modules / sizeof e->modules[0])) { emu_failf(e, "too many modules"); return NULL; }
    FILE *f = fopen(host_path, "rb");
    if (!f) { emu_failf(e, "cannot open %s", host_path); return NULL; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *img = malloc((size_t)fsz);
    if (fread(img, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(img); emu_failf(e, "read error %s", host_path); return NULL; }
    fclose(f);
#define U16(o) ((uint32_t)img[o] | ((uint32_t)img[(o) + 1] << 8))
#define U32(o) ((uint32_t)img[o] | ((uint32_t)img[(o) + 1] << 8) | ((uint32_t)img[(o) + 2] << 16) | ((uint32_t)img[(o) + 3] << 24))
    if (fsz < 0x200 || img[0] != 'M' || img[1] != 'Z') { free(img); emu_failf(e, "not a PE file"); return NULL; }
    uint32_t pe = U32(0x3C);
    if (U32(pe) != 0x4550 || U16(pe + 4) != 0x14C) { free(img); emu_failf(e, "not an i386 PE"); return NULL; }
    uint32_t nsec = U16(pe + 6), optsz = U16(pe + 20), opt = pe + 24;
    uint32_t pref_base = U32(opt + 28), image_size = U32(opt + 56), hdr_size = U32(opt + 60);
    uint32_t entry = U32(opt + 16);
    uint32_t dd = opt + 96;
    uint32_t exp_rva = U32(dd), exp_sz = U32(dd + 4), imp_rva = U32(dd + 8), rel_rva = U32(dd + 40), rel_sz = U32(dd + 44);
    uint32_t base = load_base;
    if ((uint64_t)base + image_size > EMU_HEAP_BASE) { free(img); emu_failf(e, "image does not fit"); return NULL; }
    uint8_t *m = e->mem + base;
    memcpy(m, img, hdr_size < (uint32_t)fsz ? hdr_size : (uint32_t)fsz);
    uint32_t sec = opt + optsz;
    uint32_t code_va = 0, code_size = 0;
    for (uint32_t i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsz = U32(sec + 8), va = U32(sec + 12), rawsz = U32(sec + 16), raw = U32(sec + 20), ch = U32(sec + 36);
        uint32_t n = rawsz < vsz || !vsz ? rawsz : vsz;
        if (raw + n > (uint32_t)fsz) n = (uint32_t)fsz - raw;
        memcpy(m + va, img + raw, n);
        // executable and not writable (Windows maps it read-only): safe for the decoded-instruction cache
        if (!code_size && (ch & 0x20000000u) && !(ch & 0x80000000u)) { code_va = va; code_size = vsz ? vsz : rawsz; }
    }
    free(img);
    // relocations
    int32_t delta = (int32_t)(base - pref_base);
    if (delta && rel_rva) {
        uint32_t p = rel_rva, end = rel_rva + rel_sz;
        while (p < end) {
            uint32_t page = rd32(c, base + p), bsz = rd32(c, base + p + 4);
            if (!bsz) break;
            for (uint32_t k = 8; k < bsz; k += 2) {
                uint16_t ent = rd16(c, base + p + k);
                int type = ent >> 12;
                uint32_t off = ent & 0xFFF;
                if (type == 3) { uint32_t a = base + page + off; wr32(c, a, rd32(c, a) + (uint32_t)delta); }
                else if (type != 0) { emu_failf(e, "unsupported relocation type %d", type); return NULL; }
            }
            p += bsz;
        }
    }
    // imports
    if (imp_rva) {
        for (uint32_t d = base + imp_rva;; d += 20) {
            uint32_t ilt = rd32(c, d), name_rva = rd32(c, d + 12), iat = rd32(c, d + 16);
            if (!name_rva && !iat) break;
            const char *dll = (const char *)(e->mem + base + name_rva);
            uint32_t look = ilt ? ilt : iat;
            for (uint32_t k = 0;; k++) {
                uint32_t ent = rd32(c, base + look + 4 * k);
                if (!ent) break;
                char full[160];
                if (ent & 0x80000000u) snprintf(full, sizeof full, "%s!#%u", dll, ent & 0xFFFF);
                const char *fname = (ent & 0x80000000u) ? full : (const char *)(e->mem + base + ent + 2);
                int argbytes = 0;
                uint32_t data = 0;
                EmuHostFn fn = emu_lookup_import(e, dll, fname, &argbytes, &data);
                uint32_t addr;
                if (data) addr = data;
                else {
                    size_t nl = strlen(dll) + strlen(fname) + 2;
                    char *nm = malloc(nl);
                    if (!nm) x86_fault(c, "out of host memory");
                    snprintf(nm, nl, "%s!%s", dll, fname);
                    addr = emu_thunk(e, nm, fn, argbytes);
                    e->hosts[e->nhosts - 1].owned_name = 1;
                }
                wr32(c, base + iat + 4 * k, addr);
            }
        }
    }
    if (code_size && !c->dcache && !(flags & EMU_LOAD_NO_CODE_CACHE)) {
        x86_set_code_cache(c, base + code_va, code_size);
#ifndef CV_NO_DEBUG_ENV
        { const char *s = getenv("SAPI4_NO_FAST"); if (s && *s == '1') c->no_fast = 1; }
#endif
    }
    EmuModule *mod = &e->modules[e->nmodules++];
    memset(mod, 0, sizeof *mod);
    const char *bn = strrchr(win_path, '\\');
    snprintf(mod->name, sizeof mod->name, "%s", bn ? bn + 1 : win_path);
    snprintf(mod->win_path, sizeof mod->win_path, "%s", win_path);
    mod->base = base;
    mod->size = image_size;
    mod->entry = entry ? base + entry : 0;
    mod->export_dir = exp_rva ? base + exp_rva : 0;
    mod->export_size = exp_sz;
    mod->pref_base = pref_base;
    return mod;
#undef U16
#undef U32
}

uint32_t emu_get_export(Emu *e, EmuModule *m, const char *name) {
    X86 *c = &e->cpu;
    if (!m->export_dir) return 0;
    uint32_t d = m->export_dir;
    uint32_t nnames = rd32(c, d + 24), funcs = m->base + rd32(c, d + 28), names = m->base + rd32(c, d + 32), ords = m->base + rd32(c, d + 36);
    for (uint32_t i = 0; i < nnames; i++) {
        const char *n = emu_str(e, m->base + rd32(c, names + 4 * i));
        if (!strcmp(n, name)) return m->base + rd32(c, funcs + 4u * rd16(c, ords + 2 * i));
    }
    return 0;
}

uint32_t emu_get_export_ord(Emu *e, EmuModule *m, uint32_t ord) {
    X86 *c = &e->cpu;
    if (!m->export_dir) return 0;
    uint32_t d = m->export_dir, base = rd32(c, d + 16), n = rd32(c, d + 20);
    if (ord < base || ord - base >= n) return 0;
    uint32_t rva = rd32(c, m->base + rd32(c, d + 28) + 4 * (ord - base));
    return rva ? m->base + rva : 0;
}

int emu_dll_attach(Emu *e, EmuModule *m) {
    if (!m->entry) return 1;
    uint32_t a[3] = { m->base, 1, 0 };
    uint32_t r = emu_call(e, m->entry, 3, a);
    return !e->failed && r != 0;
}

// ------------------------------------------------------------------ creation
Emu *emu_create(uint32_t mem_size) {
    pthread_once(&g_once, register_all_apis);
    Emu *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    void *m = mmap(NULL, (size_t)mem_size + 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { free(e); return NULL; }
    e->mem = m;
    e->mem_size = mem_size;
    x86_init(&e->cpu, e->mem, mem_size);
    e->cpu.host_call = host_dispatch;
    e->cpu.user = e;
    e->static_next = EMU_STATIC_BASE;
    e->heap_next = EMU_HEAP_BASE;
    e->insn_limit = UINT64_MAX;
#ifndef CV_NO_DEBUG_ENV
    { const char *s = getenv("SAPI4_MAX_INSNS"); if (s) e->insn_limit = strtoull(s, NULL, 10); }
#endif
    jmp_buf jb;
    e->cpu.fault_jmp = &jb;
    if (setjmp(jb)) {
        EMU_LOG("emu_create: %s\n", e->cpu.fault_msg);
        munmap(e->mem, (size_t)mem_size + 0x10000);
        free(e);
        return NULL;
    }
    e->thunk_return = emu_thunk(e, "<return-to-host>", h_return, -1);
    e->thunk_thread_exit = emu_thunk(e, "<thread-exit>", h_thread_exit, -1);
    emu_register_crt(e);
    emu_register_ole(e);
    // main thread
    e->main = emu_new_thread(e, 0, 0, 0);
    e->cur = e->main;
    e->cpu.s = e->main->st;
    e->cpu.fault_jmp = NULL;
    return e;
}

void emu_destroy(Emu *e) {
    if (!e) return;
    for (int i = 0; i < EMU_MAX_HANDLES; i++) {
        Handle *p = &e->handles[i];
        if (p->type == H_FILE && p->fp) fclose(p->fp);
        if (p->type == H_FIND) { for (int k = 0; k <= p->count; k++) free(p->names[k]); free(p->names); }
        p->type = H_FREE;
    }
    emu_ole_cleanup(e);
    emu_registry_cleanup(e);
    mq_free(&e->gq);
    for (int i = 0; i < EMU_MAX_THREADS; i++) mq_free(&e->threads[i].mq);
    for (int i = 0; i < 16; i++) if (e->crt_files[i].fp) { fclose(e->crt_files[i].fp); e->crt_files[i].fp = NULL; }
    x86_free_code_cache(&e->cpu);
    for (int i = 0; i < e->nhosts; i++) if (e->hosts[i].owned_name) free((void *)e->hosts[i].name);
    munmap(e->mem, (size_t)e->mem_size + 0x10000);
    free(e);
}
