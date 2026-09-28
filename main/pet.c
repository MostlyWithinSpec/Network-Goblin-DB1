#include "state.h"
#include <stdio.h>

static const char *TITLES[] = {
    "Cable Chewer",      // 1
    "Packet Pup",        // 2
    "Ping Pest",         // 3
    "Port Prowler",      // 4
    "LAN Lurker",        // 5
    "Subnet Snoop",      // 6
    "Router Rascal",     // 7
    "ARP Hoarder",       // 8
    "Firewall Fiend",    // 9
    "DNS Gremlin",       // 10
    "Packet Pirate",     // 11
    "Bandwidth Baron",   // 12
    "Network Warlord",   // 13
    "Goblin King of the LAN", // 14+
};

const char *pet_title(int level)
{
    int n = sizeof(TITLES) / sizeof(TITLES[0]);
    if (level < 1) level = 1;
    if (level > n) level = n;
    return TITLES[level - 1];
}

// XP needed to go from `level` to level+1
uint32_t pet_xp_for(int level)
{
    return 60 + 40 * (uint32_t)level * level;
}

void pet_add_xp(int xp)
{
    LOCK();
    if (G.pet.hunger == 0 && xp > 0) xp /= 2;   // starving goblins learn slowly
    G.pet.xp += xp;
    bool up = false;
    while (G.pet.xp >= pet_xp_for(G.pet.level)) {
        G.pet.xp -= pet_xp_for(G.pet.level);
        G.pet.level++;
        up = true;
    }
    int lvl = G.pet.level;
    UNLOCK();
    if (up) {
        char buf[64];
        snprintf(buf, sizeof(buf), "LEVEL %d!|%s", lvl, pet_title(lvl));
        ui_event(EV_LEVEL_UP, buf);
        pet_save();
    }
}

// called once a minute
void pet_tick_minute(void)
{
    static int minutes;
    minutes++;
    LOCK();
    bool online = G.inet_up;
    // hunger: ~100 -> 0 in about 20 hours
    if (minutes % 12 == 0 && G.pet.hunger > 0) G.pet.hunger--;
    // happiness drifts toward a target set by network health + food
    int target = 60;
    if (!G.wifi_up || !G.inet_up) target = 15;
    else if (G.ping_ms > G.cfg.slow_ms_div10 * 10) target = 40;
    if (G.pet.hunger < 20) target -= 25;
    if (target < 0) target = 0;
    if (minutes % 5 == 0) {
        if (G.pet.happy > target) G.pet.happy--;
        else if (G.pet.happy < target) G.pet.happy++;
    }
    UNLOCK();
    // XP for staying connected
    if (online && minutes % 5 == 0) pet_add_xp(1);
    if (minutes % 10 == 0) pet_save();
}

int pet_feed(void)
{
    LOCK();
    uint32_t now = now_epoch();
    if (G.pet.hunger >= 92) { UNLOCK(); ui_event(EV_TOO_FULL, "I'm STUFFED.|No more cookies!"); return 1; }
    if (G.pet.last_fed && now - G.pet.last_fed < 120 && now >= G.pet.last_fed) { UNLOCK(); return 2; }
    G.pet.hunger = G.pet.hunger + 30 > 100 ? 100 : G.pet.hunger + 30;
    G.pet.happy = G.pet.happy + 8 > 100 ? 100 : G.pet.happy + 8;
    G.pet.feeds++;
    G.pet.last_fed = now;
    UNLOCK();
    ui_event(EV_FEED, "*nom nom*|Tasty HTTP cookie!");
    pet_add_xp(3);
    pet_save();
    return 0;
}

// Patting: XP at most once a minute (and 15 XP/day), plus an annoyance meter.
// Spam-patting makes him grumpy, then angry, then he bites and sulks.
#include "esp_timer.h"
#define PAT_XP_COOLDOWN_S 60
#define PAT_XP_DAILY_CAP  15
#define ANNOY_DECAY_S     20     // annoyance drops 1 point per 20 s of peace
#define SULK_S            90

static int64_t s_last_pat, s_last_xp_pat = -1000000000LL, s_sulk_until;
static int s_annoy;
static uint32_t s_xp_day, s_xp_today;

pat_result_t pet_pat(const char **msg)
{
    int64_t now = esp_timer_get_time() / 1000000;
    if (s_last_pat) {
        int decay = (int)((now - s_last_pat) / ANNOY_DECAY_S);
        s_annoy = s_annoy > decay ? s_annoy - decay : 0;
    }
    s_last_pat = now;

    if (now < s_sulk_until) {
        *msg = "He's sulking and ignoring you.";
        ui_event_now(EV_SULK, "*ignoring you*|...hmph.");
        return PAT_SULKING;
    }

    s_annoy++;
    LOCK();
    G.pet.pats++;
    UNLOCK();

    if (s_annoy <= 2) {
        // happy pat; XP only if the cooldown has passed and today's cap isn't hit
        uint32_t day = now_epoch() / 86400;
        if (day != s_xp_day) { s_xp_day = day; s_xp_today = 0; }
        bool xp = (now - s_last_xp_pat >= PAT_XP_COOLDOWN_S) && s_xp_today < PAT_XP_DAILY_CAP;
        LOCK();
        G.pet.happy = G.pet.happy + 4 > 100 ? 100 : G.pet.happy + 4;
        UNLOCK();
        ui_event_now(EV_PAT, s_annoy == 1 ? "hehehe|...do it again" : "hehe. okay.|that's nice.");
        if (xp) {
            s_last_xp_pat = now;
            s_xp_today++;
            pet_add_xp(1);
            *msg = "hehehe (+1 XP)";
            return PAT_HAPPY_XP;
        }
        *msg = "hehehe (no XP - he only counts one pat a minute)";
        return PAT_HAPPY;
    }
    if (s_annoy <= 4) {
        ui_event_now(EV_GRUMPY, s_annoy == 3 ? "Okay, okay.|That's plenty." : "...you're still|doing it.");
        *msg = "He's getting annoyed.";
        return PAT_GRUMPY;
    }
    if (s_annoy <= 7) {
        LOCK();
        G.pet.happy = G.pet.happy >= 3 ? G.pet.happy - 3 : 0;
        UNLOCK();
        static const char *rants[] = { "STOP. POKING. ME.|", "I WILL unplug|your router.", "I know where your|DHCP server lives." };
        ui_event_now(EV_ANGRY, rants[s_annoy - 5]);
        *msg = "He's ANGRY. Maybe stop.";
        return PAT_ANGRY;
    }
    // bite!
    LOCK();
    G.pet.happy = G.pet.happy >= 8 ? G.pet.happy - 8 : 0;
    UNLOCK();
    s_sulk_until = now + SULK_S;
    s_annoy = 3;
    ui_event_now(EV_BITE, "*CHOMP*|Leave me ALONE.");
    *msg = "OW. He bit you. He's sulking for 90 seconds.";
    return PAT_BIT;
}
