void sim_all_quips(void (*cb)(const char *));
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "state.h"
app_t G; SemaphoreHandle_t G_lock; int64_t sim_now_us = 1000000;
uint32_t now_epoch(void){return 1790000000;}
uint32_t pet_xp_for(int l){return 60+40*l*l;}
const char *pet_title(int l){return "Goblin King of the LAN";}
static int bl; void lcd_backlight(int p){bl=p;}
static uint16_t FB[240*240]; static int band_y; static uint16_t bandbuf[240*20];
void lcd_frame_begin(void){band_y=0;}
uint16_t *lcd_band_buf(void){return bandbuf;}
void lcd_band_push(int rows){memcpy(&FB[band_y*240],bandbuf,240*rows*2);band_y+=rows;}
void lcd_frame_end(void){}
#include "../main/ui.c"
static void render(const char *path, int tick){
  build_scene(tick);
  lcd_frame_begin();
  for(int y=0;y<240;y+=BAND_H){draw_band(lcd_band_buf(),y,BAND_H);lcd_band_push(BAND_H);}
  FILE*f=fopen(path,"wb");fprintf(f,"P6 240 240 255\n");
  for(int i=0;i<240*240;i++){uint16_t c=FB[i];c=(c>>8)|(c<<8);unsigned char p[3]={(c>>11)<<3,((c>>5)&63)<<2,(c&31)<<3};fwrite(p,1,3,f);}
  fclose(f);
}
static int bad;
static void chk(const char *q){ char a[64],b[64]; const char*bar=strchr(q,'|'); if(bar){int n=bar-q;memcpy(a,q,n);a[n]=0;strcpy(b,bar+1);}else{strcpy(a,q);b[0]=0;}
  int wa=gfx_text_w(&FONT_SMALL,a), wb=gfx_text_w(&FONT_SMALL,b); int w=wa>wb?wa:wb;
  printf("%3d %s%s  | %s\n", w, w>218?"TOO WIDE ":"", a, b); if(w>218)bad++; }
int main(){
  memset(&G,0,sizeof G);
  G.cfg.slow_ms_div10=15; G.cfg.sleep_start=23; G.cfg.sleep_end=7; strcpy(G.cfg.name,"Gnorb");
  G.wifi_up=1; G.inet_up=1; G.have_time=0; strcpy(G.ip,"192.168.1.87"); strcpy(G.ssid,"HomeNet"); G.rssi=-58;
  G.pet.level=3; G.pet.xp=200; G.pet.hunger=72; G.pet.happy=80; G.dev_online=14;
  for(int i=0;i<60;i++){G.hist[i]=18+(i*7)%25; if(i==40)G.hist[i]=-1; if(i>50)G.hist[i]=160+i;} G.hist_count=60; G.hist_head=0;
  G.ping_ms=22; ui_init();
  { G.ping_ms=1888; G.gw_ms=1234; G.loss_pct=100; G.dev_online=64; strcpy(G.ip,"192.168.100.200"); strcpy(G.gw,"192.168.100.254");
    strcpy(G.ssid,"MyVeryLongNetworkName"); G.rssi=-100; G.pet.hunger=100; G.pet.level=14; strcpy(G.cfg.ping_host,"one.one.one.one"); strcpy(G.ap_ssid,"Goblin-Setup-7AD0");
    G.outage_n=123; G.outage_start=1790000000-3*3600-59*60; sim_all_quips(chk); printf("too wide: %d\n",bad);
    G.ping_ms=22; G.gw_ms=5; G.loss_pct=0; G.dev_online=14; strcpy(G.ip,"192.168.1.87"); strcpy(G.gw,"192.168.1.1"); strcpy(G.ssid,"HomeNet"); G.rssi=-58; G.pet.hunger=72; G.pet.level=3; G.outage_start=0; }
  render("s1_happy.ppm",4);
  G.ping_ms=240; G.loss_pct=5; sim_now_us+=20000000000LL; render("s2_slow.ppm",9);
  G.ping_ms=-1; G.inet_up=0; G.outage_start=1789999900; sim_now_us+=20000000000LL; render("s3_offline.ppm",5);
  G.inet_up=1; G.outage_start=0; G.ping_ms=20; ui_event(EV_NEW_DEVICE,"NEW FACE!|192.168.1.57"); sim_now_us+=20000000000LL; render("s4_newdev.ppm",6);
  sim_now_us+=20000000000LL; ui_event(EV_FEED,"*nom nom*|Tasty HTTP cookie!"); render("s5_feed.ppm",8);
  sim_now_us+=20000000000LL; G.wifi_up=0; G.setup_mode=1; strcpy(G.ap_ssid,"Goblin-Setup-7AD0"); render("s6_setup.ppm",3);
  sim_now_us+=20000000000LL; G.wifi_up=1; G.setup_mode=0; G.pet.hunger=10; G.ping_ms=30; render("s7_hungry.ppm",3);
  G.wifi_up=1; G.pet.hunger=70; G.setup_mode=0;
  sim_now_us+=20000000000LL; ui_event_now(EV_GRUMPY,"Okay, okay.|That's plenty."); render("s8_grumpy.ppm",3);
  sim_now_us+=20000000000LL; ui_event_now(EV_ANGRY,"I WILL unplug|your router."); render("s9_angry.ppm",3);
  sim_now_us+=20000000000LL; ui_event_now(EV_BITE,"*CHOMP*|Leave me ALONE."); render("s10_bite.ppm",3);
  return 0;
}
