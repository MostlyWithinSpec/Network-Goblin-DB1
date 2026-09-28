#include "ui.h"
#include "state.h"
#include "lcd.h"
#include "gfx.h"
#include "assets.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ------------------------------------------------------------------ colours
#define C_BG_TOP    RGB(0x1C, 0x2B, 0x24)
#define C_BG_BOT    RGB(0x0A, 0x12, 0x0E)
#define C_BAR       RGB(0x08, 0x0D, 0x0B)
#define C_TEXT      RGB(0xE8, 0xF5, 0xE0)
#define C_DIM       RGB(0x7A, 0x93, 0x80)
#define C_GREEN     RGB(0x6C, 0xE0, 0x46)
#define C_AMBER     RGB(0xFF, 0xB0, 0x30)
#define C_RED       RGB(0xFF, 0x3A, 0x3A)
#define C_CYAN      RGB(0x3F, 0xF0, 0xFF)
#define C_PURPLE    RGB(0xB0, 0x70, 0xFF)
#define C_BUBBLE    RGB(0xF2, 0xF7, 0xEE)
#define C_BUBBLE_TX RGB(0x14, 0x20, 0x18)
#define C_PANEL     RGB(0x12, 0x1C, 0x17)

// ------------------------------------------------------------------ events
typedef struct { ev_type_t t; char text[64]; } ev_t;
static QueueHandle_t s_evq;

int s_render_ms;
static ev_t s_now_ev;
static volatile bool s_now_pending;

void ui_event_now(ev_type_t t, const char *text)
{
    s_now_ev.t = t;
    strlcpy(s_now_ev.text, text, sizeof(s_now_ev.text));
    s_now_pending = true;
}

void ui_event(ev_type_t t, const char *text)
{
    if (!s_evq) return;
    ev_t e = { .t = t };
    strlcpy(e.text, text, sizeof(e.text));
    if (xQueueSend(s_evq, &e, 0) != pdTRUE) {
        ev_t junk;
        xQueueReceive(s_evq, &junk, 0);
        xQueueSend(s_evq, &e, 0);
    }
}

// ------------------------------------------------------------------ scene
typedef enum { M_HAPPY, M_IDLE, M_SLOW, M_OFFLINE, M_NOWIFI, M_SLEEP, M_HUNGRY, M_SETUP } mood_t;

typedef struct {
    int frame;
    int bob;
    uint16_t glow, glow_dk;
    uint16_t bg_top, bg_bot;
    char l1[40], l2[40];
    bool bubble_alert;
    int effect;            // EF_*
    int t;                 // animation tick
    // status bar
    char clock[8];
    char ipline[24];
    int level;
    int rssi;
    bool wifi;
    // bottom panel
    int16_t hist[HIST_N];
    int hist_n;
    int ping;
    int hunger, xp_pct;
} scene_t;

enum { EF_NONE, EF_SWEAT, EF_PANIC, EF_ZZZ, EF_HEARTS, EF_COOKIE, EF_BANG, EF_SPARKLE, EF_VEIN, EF_RAGE };

static scene_t S;
static uint16_t s_pal[16];

// ------------------------------------------------------------------ helpers
static void split(const char *src, char *l1, char *l2)
{
    const char *bar = strchr(src, '|');
    if (bar) {
        int n = bar - src;
        if (n > 39) n = 39;
        memcpy(l1, src, n); l1[n] = 0;
        strlcpy(l2, bar + 1, 40);
    } else {
        strlcpy(l1, src, 40);
        l2[0] = 0;
    }
}

static bool is_night(void)
{
    if (!G.have_time) return false;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour, a = G.cfg.sleep_start, b = G.cfg.sleep_end;
    if (a == b) return false;
    return a < b ? (h >= a && h < b) : (h >= a || h < b);
}

static void fmt_dur(uint32_t s, char *out, size_t n)
{
    if (s >= 3600) snprintf(out, n, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
    else if (s >= 60) snprintf(out, n, "%lum %lus", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else snprintf(out, n, "%lus", (unsigned long)s);
}

// Phrase book. '|' splits the two bubble lines. Tokens are filled with live values:
// {ping} {gwms} {loss} {dev} {ip} {gw} {ssid} {rssi} {food} {lvl} {title} {name}
// {down} {host} {ap} {outn}. Prefix "@m" = mornings only, "@e" = evenings only.
static const char *const Q_HAPPY[] = {
    "{ping}ms ping.|Zoom zoom.",
    "Packets taste|FRESH today.",
    "{dev} devices online.|I'm watching them.",
    "All quiet on the LAN.|Suspiciously quiet.",
    "*gnaws happily|on a cat6 cable*",
    "Latency? Never|heard of her.",
    "Router's purring.|Good router.",
    "{ping}ms! I could|ping all day.",
    "Zero drama on|the network today.",
    "I love the smell of|fresh DHCP leases.",
    "Everything's green.|I'm suspicious.",
    "Internet: working.|Goblin: thriving.",
    "Level {lvl} and|feeling great.",
    "Fast pings, full|belly. Life is good.",
    "@mGood morning!|The internet's up too.",
    "@eLong day?|The LAN missed you.",
};
static const char *const Q_IDLE[] = {
    "Ping {ping}ms.|Could be worse.",
    "Guarding {dev} devices.|You're welcome.",
    "*sniffs packets*|smells like DNS.",
    "Nothing to report.|For now...",
    "Just vibing on|{ssid}.",
    "Counting packets.|Lost count. Again.",
    "Router's at {gw}.|I'm watching it.",
    "Signal {rssi}dBm.|I've had worse.",
    "*taps gadget*|still works.",
    "Is it snack time?|Asking for me.",
    "{title},|reporting for duty.",
    "Watching the blinky|lights. Relaxing.",
    "I'm not lazy. I'm|monitoring.",
    "@mMorning. Coffee?|I'll take a cookie.",
    "@eEvening shift.|Still guarding.",
};
static const char *const Q_SLOW[] = {
    "Ping {ping}ms?!|Who's streaming 4K?",
    "Everything is|sooo sloooow...",
    "{loss}% packet loss.|I'm sweating.",
    "The tubes are|clogged again.",
    "Someone's hogging|the bandwidth. Rude.",
    "{ping}ms... I could|walk there faster.",
    "Is someone on a|video call? Again?",
    "Packets stuck in|traffic. Honk honk.",
    "Router, you okay?|You seem stressed.",
    "This lag is making|me itchy.",
    "Router says {gwms}ms.|So it's upstream.",
};
static const char *const Q_OFFLINE[] = {
    "NO INTERNET!|PANIC!!!",
    "Down for {down}|and counting...",
    "Router, WAKE UP!|",
    "Have you tried|turning it off & on?",
    "The internet left.|Was it me?",
    "Pinging {host}...|nothing. NOTHING.",
    "This is fine.|Everything is fine.",
    "{blame}",
    "Don't panic.|(I'm panicking.)",
    "Outage #{outn} of|my career.",
    "Quick, check if the|modem is blinking!",
};
static const char *const Q_NOWIFI[] = {
    "Where did the|Wi-Fi go?!",
    "Can't find|{ssid}!",
    "Hello? Wi-Fi?|Anyone?",
    "No Wi-Fi. Just a|goblin in a box.",
    "Reconnecting...|any second now...",
};
static const char *const Q_SLEEP[] = {
    "zzz...|",
    "*snores in|binary*",
    "zzz... packets...|zzz...",
    "*dreams of|gigabit fiber*",
    "zzz... mmm...|cookies...",
    "Wake me if the|internet breaks.",
};
static const char *const Q_HUNGRY[] = {
    "Feed me cookies!|{ip}",
    "So... hungry...|Food: {food}%",
    "My tummy is making|dial-up noises.",
    "Cookie. Now.|Please.",
    "I'd eat a packet|at this point.",
    "Hungry goblins|chew cables.",
    "Food is at {food}%.|This is a crisis.",
};
static const char *const Q_SETUP[] = {
    "Join Wi-Fi:|{ap}",
    "Then open|http://192.168.4.1",
};

#define NQ(a) (int)(sizeof(a) / sizeof(a[0]))

static void expand(const char *tpl, char *out, size_t n)
{
    char v[40];
    size_t o = 0;
    while (*tpl && o + 1 < n) {
        if (*tpl != '{') { out[o++] = *tpl++; continue; }
        const char *end = strchr(tpl, '}');
        if (!end) { out[o++] = *tpl++; continue; }
        int kl = end - tpl - 1;
        const char *k = tpl + 1;
        v[0] = 0;
#define K(name) (kl == (int)strlen(name) && !strncmp(k, name, kl))
        if (K("ping")) snprintf(v, sizeof(v), "%d", G.ping_ms);
        else if (K("gwms")) { if (G.gw_ms >= 0) snprintf(v, sizeof(v), "%d", G.gw_ms); else strcpy(v, "??"); }
        else if (K("loss")) snprintf(v, sizeof(v), "%d", G.loss_pct);
        else if (K("dev")) snprintf(v, sizeof(v), "%d", G.dev_online);
        else if (K("ip")) snprintf(v, sizeof(v), "%s", G.ip);
        else if (K("gw")) snprintf(v, sizeof(v), "%s", G.gw);
        else if (K("ssid")) snprintf(v, sizeof(v), "%.20s", G.ssid[0] ? G.ssid : "the Wi-Fi");
        else if (K("rssi")) snprintf(v, sizeof(v), "%d", G.rssi);
        else if (K("food")) snprintf(v, sizeof(v), "%d", G.pet.hunger);
        else if (K("lvl")) snprintf(v, sizeof(v), "%d", G.pet.level);
        else if (K("title")) snprintf(v, sizeof(v), "%s", pet_title(G.pet.level));
        else if (K("name")) snprintf(v, sizeof(v), "%s", G.cfg.name);
        else if (K("host")) snprintf(v, sizeof(v), "%.16s", G.cfg.ping_host);
        else if (K("ap")) snprintf(v, sizeof(v), "%s", G.ap_ssid);
        else if (K("outn")) snprintf(v, sizeof(v), "%d", G.outage_n);
        else if (K("down")) fmt_dur(G.outage_start ? now_epoch() - G.outage_start : 0, v, sizeof(v));
        else if (K("blame")) strcpy(v, G.gw_ms >= 0 ? "Router's fine.|Blame the ISP!" : "Router's silent|too. Uh oh.");
#undef K
        for (char *c = v; *c && o + 1 < n; c++) out[o++] = *c;
        tpl = end + 1;
    }
    out[o] = 0;
}

static int s_last_pick[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int s_seed_seen[8];

static void quip(mood_t m, int seed, char *out, size_t n)
{
    const char *const *list;
    int cnt;
    switch (m) {
    case M_HAPPY:   list = Q_HAPPY;   cnt = NQ(Q_HAPPY);   break;
    case M_IDLE:    list = Q_IDLE;    cnt = NQ(Q_IDLE);    break;
    case M_SLOW:    list = Q_SLOW;    cnt = NQ(Q_SLOW);    break;
    case M_OFFLINE: list = Q_OFFLINE; cnt = NQ(Q_OFFLINE); break;
    case M_NOWIFI:  list = Q_NOWIFI;  cnt = NQ(Q_NOWIFI);  break;
    case M_SLEEP:   list = Q_SLEEP;   cnt = NQ(Q_SLEEP);   break;
    case M_HUNGRY:  list = Q_HUNGRY;  cnt = NQ(Q_HUNGRY);  break;
    default:        expand(Q_SETUP[seed % 2], out, n); return;
    }
    int hour = -1;
    if (G.have_time) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); hour = tm.tm_hour; }
    bool morning = hour >= 5 && hour < 11, evening = hour >= 17 && hour < 22;
    // same seed -> same line (values still refresh live); new seed -> new line, never a repeat
    if (s_last_pick[m] >= 0 && s_seed_seen[m] == seed) {
        const char *q = list[s_last_pick[m]];
        if (q[0] == '@') q += 2;
        expand(q, out, n);
        return;
    }
    s_seed_seen[m] = seed;
    int pick = -1;
    for (int tries = 0; tries < 12; tries++) {
        int i = (int)((seed * 2654435761u + tries * 40503u) >> 8) % cnt;
        const char *q = list[i];
        if (q[0] == '@' && ((q[1] == 'm' && !morning) || (q[1] == 'e' && !evening))) continue;
        if (i == s_last_pick[m] && cnt > 1) continue;
        pick = i;
        break;
    }
    if (pick < 0) pick = 0;
    s_last_pick[m] = pick;
    const char *q = list[pick];
    if (q[0] == '@') q += 2;
    expand(q, out, n);
}

#ifdef SIM
// used by the host simulator to check every phrase fits the bubble
void sim_all_quips(void (*cb)(const char *))
{
    const char *const *lists[] = { Q_HAPPY, Q_IDLE, Q_SLOW, Q_OFFLINE, Q_NOWIFI, Q_SLEEP, Q_HUNGRY, Q_SETUP };
    int cnts[] = { NQ(Q_HAPPY), NQ(Q_IDLE), NQ(Q_SLOW), NQ(Q_OFFLINE), NQ(Q_NOWIFI), NQ(Q_SLEEP), NQ(Q_HUNGRY), NQ(Q_SETUP) };
    char buf[96];
    for (int l = 0; l < 8; l++)
        for (int i = 0; i < cnts[l]; i++) {
            const char *q = lists[l][i];
            if (q[0] == '@') q += 2;
            expand(q, buf, sizeof(buf));
            cb(buf);
        }
}
#endif

// ------------------------------------------------------------------ scene build
static ev_t s_cur_ev;
static int64_t s_ev_until;
static int s_quip_pick;
static int64_t s_quip_next;
static mood_t s_last_mood = (mood_t)-1;

static int ev_frame(ev_type_t t, int *effect, bool *alert)
{
    *alert = false;
    switch (t) {
    case EV_NEW_DEVICE: *effect = EF_BANG; *alert = true; return FR_SURPRISE;
    case EV_FEED:       *effect = EF_COOKIE; return FR_EAT;
    case EV_PAT:        *effect = EF_HEARTS; return FR_HAPPY;
    case EV_LEVEL_UP:   *effect = EF_SPARKLE; return FR_HAPPY;
    case EV_OUTAGE:     *effect = EF_PANIC; *alert = true; return FR_PANIC;
    case EV_RECOVER:    *effect = EF_SPARKLE; return FR_HAPPY;
    case EV_BASELINE:   *effect = EF_BANG; return FR_SURPRISE;
    case EV_TOO_FULL:   *effect = EF_NONE; return FR_SWEAT;
    case EV_HELLO:      *effect = EF_SPARKLE; return FR_HAPPY;
    case EV_GRUMPY:     *effect = EF_NONE; return FR_GRUMPY;
    case EV_ANGRY:      *effect = EF_VEIN; return FR_ANGRY;
    case EV_BITE:       *effect = EF_RAGE; *alert = true; return FR_ANGRY;
    case EV_SULK:       *effect = EF_NONE; return FR_GRUMPY;
    default:            *effect = EF_NONE; return FR_IDLE;
    }
}

static int ev_duration_ms(ev_type_t t)
{
    switch (t) {
    case EV_NEW_DEVICE: case EV_OUTAGE: case EV_LEVEL_UP: return 9000;
    case EV_BASELINE: case EV_RECOVER: case EV_HELLO: return 7000;
    case EV_BITE: return 6000;
    default: return 4500;
    }
}

static bool s_night;

static void build_scene(int tick)
{
    int64_t now = esp_timer_get_time() / 1000;
    S.t = tick;

    // pull next event (an immediate one pre-empts whatever is showing)
    if (s_now_pending) {
        s_now_pending = false;
        // don't lose an important alert that a pat interrupted: re-show it afterwards
        ev_type_t c = s_cur_ev.t;
        if (now < s_ev_until && (c == EV_NEW_DEVICE || c == EV_OUTAGE || c == EV_LEVEL_UP || c == EV_RECOVER || c == EV_BASELINE))
            xQueueSendToFront(s_evq, &s_cur_ev, 0);
        s_cur_ev = s_now_ev;
        s_ev_until = now + ev_duration_ms(s_cur_ev.t);
    } else if (now >= s_ev_until) {
        ev_t e;
        if (xQueueReceive(s_evq, &e, 0) == pdTRUE) {
            s_cur_ev = e;
            s_ev_until = now + ev_duration_ms(e.t);
        } else {
            s_cur_ev.t = EV_NONE;
        }
    }

    LOCK();
    // mood
    mood_t m;
    s_night = is_night();
    if (G.setup_mode && !G.wifi_up) m = M_SETUP;
    else if (!G.wifi_up) m = M_NOWIFI;
    else if (!G.inet_up && G.outage_start) m = M_OFFLINE;
    else if (s_night) m = M_SLEEP;
    else if (G.pet.hunger < 20) m = M_HUNGRY;
    else if (G.ping_ms > G.cfg.slow_ms_div10 * 10 || G.loss_pct >= 15) m = M_SLOW;
    else if (G.ping_ms >= 0 && G.ping_ms < 45 && G.pet.happy >= 40) m = M_HAPPY;
    else m = M_IDLE;

    if (m != s_last_mood || now >= s_quip_next) {
        if (m != s_last_mood) s_quip_pick = esp_random() & 0xFF;
        else s_quip_pick = esp_random();
        s_quip_next = now + (m == M_SETUP ? 4000 : 14000);
        s_last_mood = m;
    }

    // glow colour on the goblin's gadget = network health
    uint16_t glow;
    switch (m) {
    case M_SETUP: case M_NOWIFI: glow = C_PURPLE; break;
    case M_OFFLINE: glow = C_RED; break;
    case M_SLOW: glow = C_AMBER; break;
    default: glow = C_CYAN; break;
    }
    if (m == M_OFFLINE && (tick / 4) % 2) glow = RGB(0x60, 0x10, 0x10);
    S.glow = glow;
    S.glow_dk = gfx_blend(glow, 0, 110);

    S.bg_top = C_BG_TOP;
    S.bg_bot = C_BG_BOT;
    if (m == M_OFFLINE) S.bg_top = gfx_blend(C_BG_TOP, RGB(0x60, 0, 0), (tick / 4) % 2 ? 140 : 60);
    if (m == M_SLEEP) { S.bg_top = RGB(0x10, 0x14, 0x28); S.bg_bot = RGB(0x04, 0x06, 0x10); }

    // frame + effect
    int frame = FR_IDLE, effect = EF_NONE;
    bool alert = false;
    if (s_cur_ev.t != EV_NONE) {
        frame = ev_frame(s_cur_ev.t, &effect, &alert);
        split(s_cur_ev.text, S.l1, S.l2);
    } else {
        switch (m) {
        case M_HAPPY: frame = FR_HAPPY; break;
        case M_IDLE: frame = FR_IDLE; break;
        case M_SLOW: frame = FR_SWEAT; effect = EF_SWEAT; break;
        case M_OFFLINE: frame = FR_PANIC; effect = EF_PANIC; alert = true; break;
        case M_NOWIFI: frame = FR_PANIC; break;
        case M_SLEEP: frame = FR_SLEEP; effect = EF_ZZZ; break;
        case M_HUNGRY: frame = FR_HUNGRY; break;
        case M_SETUP: frame = FR_SURPRISE; break;
        }
        char q[80];
        quip(m, s_quip_pick, q, sizeof(q));
        split(q, S.l1, S.l2);
        // blink now and then
        if ((frame == FR_IDLE || frame == FR_HUNGRY || frame == FR_SWEAT) && (tick % 47) < 2) frame = FR_BLINK;
    }
    S.frame = frame;
    if (effect == EF_RAGE) S.bg_top = gfx_blend(S.bg_top, RGB(0x70, 0x10, 0x00), (tick / 2) % 2 ? 150 : 80);
    S.effect = effect;
    S.bubble_alert = alert;

    // bob: calm when sleeping, jittery when panicking
    if (m == M_SLEEP && s_cur_ev.t == EV_NONE) S.bob = ((tick / 10) % 2) * 2;
    else if (effect == EF_PANIC || effect == EF_RAGE) S.bob = (tick % 2) * 3;
    else if (frame == FR_HAPPY || frame == FR_EAT) S.bob = ((tick / 3) % 2) * 4;
    else S.bob = ((tick / 6) % 2) * 2;

    // status bar
    if (G.have_time) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(S.clock, sizeof(S.clock), "%d:%02d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min);
    } else {
        strcpy(S.clock, "--:--");
    }
    S.level = G.pet.level;
    S.rssi = G.rssi;
    S.wifi = G.wifi_up;
    if (G.wifi_up) snprintf(S.ipline, sizeof(S.ipline), "%s", G.ip);
    else if (G.setup_mode) strcpy(S.ipline, "setup 192.168.4.1");
    else strcpy(S.ipline, "no wi-fi");

    // bottom panel
    S.hist_n = G.hist_count;
    for (int i = 0; i < G.hist_count; i++) {
        int idx = (G.hist_head - G.hist_count + i + HIST_N) % HIST_N;
        S.hist[i] = G.hist[idx];
    }
    S.ping = G.ping_ms;
    S.hunger = G.pet.hunger;
    uint32_t need = pet_xp_for(G.pet.level);
    S.xp_pct = need ? (int)(G.pet.xp * 100 / need) : 0;
    UNLOCK();

    memcpy(s_pal, goblin_palette, sizeof(s_pal));
    s_pal[9] = S.glow;
    s_pal[10] = S.glow_dk;
}

// ------------------------------------------------------------------ drawing
#define SPR_X 56
#define SPR_Y 30
#define SPR_S 4

static void draw_wifi(int x, int y)
{
    int bars = 0;
    if (S.wifi) bars = S.rssi > -55 ? 4 : S.rssi > -65 ? 3 : S.rssi > -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = 4 + i * 3;
        gfx_rect(x + i * 5, y + 13 - h, 3, h, i < bars ? C_GREEN : RGB(0x30, 0x40, 0x38));
    }
    if (!S.wifi) { gfx_line(x - 1, y + 1, x + 18, y + 13, C_RED); }
}

static void draw_bar(int x, int y, int w, int pct, uint16_t c, const char *label)
{
    gfx_text(&FONT_SMALL, x, y - 1, label, C_DIM);
    int bx = x + 44, bw = w - 44;
    gfx_rrect(bx, y + 2, bw, 9, 4, RGB(0x25, 0x33, 0x2C));
    int fw = bw * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
    if (fw > 0) gfx_rrect(bx, y + 2, fw < 8 ? 8 : fw, 9, 4, c);
}

static void draw_effects(void)
{
    int t = S.t;
    int hx = SPR_X, hy = SPR_Y + S.bob;
    switch (S.effect) {
    case EF_SWEAT: {
        int dy = (t * 2) % 28;
        gfx_sprite4(PROP_SWEAT.px, PROP_SWEAT.w, PROP_SWEAT.h, hx + 112, hy + 20 + dy, 3, s_pal);
        gfx_sprite4(PROP_SWEAT.px, PROP_SWEAT.w, PROP_SWEAT.h, hx - 6, hy + 30 + (dy + 14) % 28, 2, s_pal);
        break;
    }
    case EF_PANIC:
        if ((t / 3) % 2) {
            gfx_sprite4(PROP_BANG.px, PROP_BANG.w, PROP_BANG.h, 18, 44, 4, s_pal);
            gfx_sprite4(PROP_BANG.px, PROP_BANG.w, PROP_BANG.h, 212, 44, 4, s_pal);
        }
        break;
    case EF_BANG:
        gfx_sprite4(PROP_BANG.px, PROP_BANG.w, PROP_BANG.h, 196, hy + 6 + ((t / 2) % 2) * 3, 5, s_pal);
        gfx_sprite4(PROP_BANG.px, PROP_BANG.w, PROP_BANG.h, 34, hy + 6 + (((t / 2) + 1) % 2) * 3, 5, s_pal);
        break;
    case EF_ZZZ: {
        for (int i = 0; i < 3; i++) {
            int p = (t + i * 14) % 42;
            gfx_text(i == 1 ? &FONT_MED : &FONT_SMALL, 178 + p / 3 + i * 4, 90 - p * 2, "z", C_PURPLE);
        }
        break;
    }
    case EF_HEARTS:
        for (int i = 0; i < 3; i++) {
            int p = (t * 2 + i * 13) % 40;
            gfx_sprite4(PROP_HEART.px, PROP_HEART.w, PROP_HEART.h, 30 + i * 80, 120 - p * 2, 3, s_pal);
        }
        break;
    case EF_COOKIE:
        if ((t / 4) % 3 != 2)
            gfx_sprite4(PROP_COOKIE.px, PROP_COOKIE.w, PROP_COOKIE.h, hx + 50, hy + 76, 3, s_pal);
        break;
    case EF_RAGE:
    case EF_VEIN: {
        int pulse = (t / 2) % 2;
        gfx_sprite4(PROP_VEIN.px, PROP_VEIN.w, PROP_VEIN.h, hx + 96 - pulse, hy + 2 - pulse, 3 + pulse, s_pal);
        if (S.effect == EF_RAGE && (t / 2) % 2) {
            // steam puffs off both ears
            gfx_circle(hx + 2, hy + 12 - (t % 6), 5, RGB(0xC8, 0xC8, 0xC8));
            gfx_circle(hx + 126, hy + 12 - ((t + 3) % 6), 5, RGB(0xC8, 0xC8, 0xC8));
        }
        break;
    }
    case EF_SPARKLE:
        for (int i = 0; i < 6; i++) {
            int ang = (t + i * 7) % 24;
            int x = 120 + ((i % 2) ? 90 : -90) + ((ang < 12) ? ang : 24 - ang) - 6;
            int y = 40 + i * 18;
            uint16_t c = (i + t / 3) % 2 ? RGB(0xFF, 0xE0, 0x60) : C_CYAN;
            gfx_rect(x, y - 4, 2, 10, c);
            gfx_rect(x - 4, y, 10, 2, c);
        }
        break;
    }
}

static int isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r ? r : 1; }

static void draw_band(uint16_t *buf, int y0, int h)
{
    gfx_band(buf, y0, h);

    // background
    gfx_vgrad(0, 0, LCD_W, 200, S.bg_top, S.bg_bot);
    // floor shadow
    gfx_rrect(SPR_X + 20, SPR_Y + 124, 88, 8, 4, gfx_blend(S.bg_bot, 0, 120));

    // status bar
    gfx_rect(0, 0, LCD_W, 24, C_BAR);
    gfx_text(&FONT_MED, 8, 2, S.clock, C_TEXT);
    gfx_text_c(&FONT_SMALL, 128, 5, S.ipline, S.wifi ? C_DIM : C_PURPLE);
    draw_wifi(212, 5);

    // goblin
    gfx_sprite4(GOBLIN_FRAME(S.frame), SPR_W, SPR_H, SPR_X, SPR_Y + S.bob, SPR_S, s_pal);
    draw_effects();

    // speech bubble
    int by = 160;
    uint16_t bub = S.bubble_alert && (S.t / 3) % 2 ? RGB(0xFF, 0xE0, 0x60) : C_BUBBLE;
    gfx_rrect(6, by, 228, 38, 9, bub);
    gfx_rect(112, by - 5, 10, 5, bub);   // tail
    gfx_rect(114, by - 8, 6, 3, bub);
    if (S.l2[0]) {
        gfx_text_c(&FONT_SMALL, 120, by + 3, S.l1, C_BUBBLE_TX);
        gfx_text_c(&FONT_SMALL, 120, by + 19, S.l2, C_BUBBLE_TX);
    } else {
        gfx_text_c(&FONT_SMALL, 120, by + 11, S.l1, C_BUBBLE_TX);
    }

    // bottom panel
    gfx_rect(0, 202, LCD_W, 38, C_BAR);
    // ping sparkline
    int gx = 6, gy = 205, gw = 120, gh = 32;
    gfx_rrect(gx, gy, gw, gh, 5, C_PANEL);
    // sqrt scale so 20 ms and 300 ms are both readable
    int maxv = 30;
    for (int i = 0; i < S.hist_n; i++) if (S.hist[i] > maxv) maxv = S.hist[i];
    if (maxv > 600) maxv = 600;
    int maxs = isqrt(maxv);
    int n = S.hist_n;
    for (int i = 0; i < n; i++) {
        int v = S.hist[i];
        int x = gx + gw - 3 - (n - 1 - i) * 2;
        if (x < gx + 2) continue;
        if (v < 0) { gfx_rect(x, gy + 3, 2, gh - 6, RGB(0x70, 0x18, 0x18)); continue; }
        int bh = isqrt(v) * (gh - 16) / maxs + 1;
        if (bh < 1) bh = 1;
        if (bh > gh - 16) bh = gh - 16;
        uint16_t c = v > G.cfg.slow_ms_div10 * 10 ? C_AMBER : C_GREEN;
        gfx_rect(x, gy + gh - 3 - bh, 2, bh, c);
    }
    char ps[16];
    if (S.ping >= 0) snprintf(ps, sizeof(ps), "%dms", S.ping); else strcpy(ps, S.wifi ? "LOST" : "--");
    gfx_text(&FONT_SMALL, gx + 4, gy + 1, ps, S.ping >= 0 ? C_TEXT : C_RED);

    // pet bars
    draw_bar(132, 206, 102, S.hunger, S.hunger < 20 ? C_RED : C_AMBER, "FOOD");
    char lvl[12];
    snprintf(lvl, sizeof(lvl), "Lv%d", S.level);
    draw_bar(132, 222, 102, S.xp_pct, C_CYAN, lvl);
}

// ------------------------------------------------------------------ task
static void ui_task(void *arg)
{
    int tick = 0;
    int bl_now = -1;
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        build_scene(tick);

        // backlight: dim at night unless an event is showing
        int want;
        LOCK();
        want = (s_night && s_cur_ev.t == EV_NONE) ? G.cfg.night_bright : G.cfg.bright;
        UNLOCK();
        if (bl_now != want) {
            bl_now = bl_now < 0 ? want : bl_now + (want > bl_now ? 2 : -2);
            if (abs(bl_now - want) < 2) bl_now = want;
            lcd_backlight(bl_now);
        }

        lcd_frame_begin();
        for (int y = 0; y < LCD_H; y += BAND_H) {
            draw_band(lcd_band_buf(), y, BAND_H);
            lcd_band_push(BAND_H);
        }
        lcd_frame_end();
        tick++;
        int64_t dt = (esp_timer_get_time() - t0) / 1000;
        s_render_ms = (int)dt;
        int wait = 120 - (int)dt;
        vTaskDelay(pdMS_TO_TICKS(wait > 30 ? wait : 30));   // always leave CPU for others
    }
}

void ui_init(void)
{
    s_evq = xQueueCreate(8, sizeof(ev_t));
}

void ui_start(void)
{
    xTaskCreate(ui_task, "ui", 4096, NULL, 2, NULL);   // lowest: web + network come first
}
