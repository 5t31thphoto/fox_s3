// fox_tools.inc — radio tools and calibrated direction-of-arrival radar.
#pragma once
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <math.h>

extern M5Canvas* face_canvas;
extern FoxConfig cfg;

// ---------------------------------------------------------------------------
// Radio ownership. Every tool explicitly acquires the radio it needs and
// releases it before returning. In particular, never erase saved WiFi config
// just to start a scan.
// ---------------------------------------------------------------------------
static bool s_ble_started = false;
static BLEScan* ble_scan = nullptr;

static void radio_enter_wifi() {
    if (s_ble_started) {
        BLEDevice::deinit(true);
        s_ble_started = false;
        ble_scan = nullptr;
        delay(40);
    }
    esp_wifi_set_promiscuous(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    delay(80);
}

static void radio_enter_ble() {
    esp_wifi_set_promiscuous(false);
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    delay(60);
    if (!s_ble_started) {
        BLEDevice::init("");
        ble_scan = BLEDevice::getScan();
        ble_scan->setActiveScan(true);
        ble_scan->setInterval(80);
        ble_scan->setWindow(60);
        s_ble_started = true;
    }
}

void tool_radio_release() {
    esp_wifi_set_promiscuous(false);
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    if (s_ble_started) {
        BLEDevice::deinit(true);
        s_ble_started = false;
        ble_scan = nullptr;
    }
    delay(40);
    if (cfg.cloud_enabled && cfg.wifi_enabled) wifi_connect();
}

// ---------------------------------------------------------------------------
// Compass / IMU
// ---------------------------------------------------------------------------
struct CompassCal { float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f; bool valid=false; };
static CompassCal compass_cal;

static float compass_raw_deg(bool apply_cal=true) {
    float mx,my,mz;
    if (M5.Imu.getMag(&mx,&my,&mz)) {
        // Apply the live hard-iron offsets. The AtomS3R magnetometer is mounted
        // close to the board electronics, so raw atan2 drifts badly without it.
        if (apply_cal && compass_cal.valid) {
            float ox=(compass_cal.maxx+compass_cal.minx)*.5f, oy=(compass_cal.maxy+compass_cal.miny)*.5f;
            float sx=max(1.0f,(compass_cal.maxx-compass_cal.minx)*.5f), sy=max(1.0f,(compass_cal.maxy-compass_cal.miny)*.5f);
            mx=(mx-ox)/sx; my=(my-oy)/sy;
        }
        float h = atan2f(my,mx)*180.0f/PI;
        if(h<0)h+=360; return h;
    }
    float gx,gy,gz; if(M5.Imu.getGyro(&gx,&gy,&gz)) return 0;
    return 0;
}

float imu_heading_deg() { return compass_raw_deg(); }

static void draw_compass_calibration(float heading, float progress) {
    face_canvas->fillSprite(cfg.color_bg);
    face_canvas->setTextColor(cfg.color_accent); face_canvas->setTextDatum(top_center);
    face_canvas->drawString("COMPASS CALIBRATION",64,5);
    int cx=64,cy=65;
    face_canvas->drawCircle(cx,cy,42,0x39C7); face_canvas->drawCircle(cx,cy,3,cfg.color_primary);
    float a=(heading-90)*PI/180.0f;
    face_canvas->drawLine(cx,cy,cx+34*cosf(a),cy+34*sinf(a),cfg.color_primary);
    // arrow showing the required rotation direction
    face_canvas->fillTriangle(104,50,119,58,106,66,cfg.color_accent);
    face_canvas->setTextDatum(middle_center); face_canvas->setTextColor(0xFFFF);
    face_canvas->drawString("rotate 360°",64,104);
    face_canvas->drawString(String((int)(min(progress,1.0f)*100))+"%",64,119);
    face_canvas->pushSprite(0,0);
}

static bool compass_calibrate() {
    compass_cal = CompassCal{};
    speak("calibrate compass: slowly rotate me one full turn~");
    uint32_t t0=millis(), last=millis(); float prev=compass_raw_deg(), travel=0;
    while(millis()-t0<15000 && travel<330.0f) {
        M5.update(); ButtonEvent ev=input_button_event();
        if(ev==BTN_DOUBLE) return false;
        float mx,my,mz;
        if(M5.Imu.getMag(&mx,&my,&mz)) {
            compass_cal.minx=min(compass_cal.minx,mx); compass_cal.maxx=max(compass_cal.maxx,mx);
            compass_cal.miny=min(compass_cal.miny,my); compass_cal.maxy=max(compass_cal.maxy,my);
            float h=compass_raw_deg(false); float d=h-prev; if(d>180)d-=360; if(d<-180)d+=360; travel+=fabsf(d); prev=h;
            compass_cal.valid=true; draw_compass_calibration(h,travel/360.0f);
        }
        delay(18);
    }
    // Require a meaningful magnetic span; otherwise a flat/failed sensor must
    // not produce a convincing-looking radar.
    bool ok=compass_cal.valid && (compass_cal.maxx-compass_cal.minx)>5 && (compass_cal.maxy-compass_cal.miny)>5 && travel>=330;
    if(!ok){ speak("i couldn't get a full compass turn"); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Radar storage and rendering
// ---------------------------------------------------------------------------
struct Blip { String id; String name; int rssi; float bearing; uint32_t seen; };
static const int MAX_BLIPS=24; static Blip blips[MAX_BLIPS]; static int blip_n=0;

static void blip_upsert(const String& id,const String& name,int rssi,float bearing) {
    for(int i=0;i<blip_n;++i) if(blips[i].id==id) {
        // Keep a running bearing, but let a substantially stronger reading move
        // the estimate. This prevents one noisy packet from teleporting a dot.
        if(rssi>blips[i].rssi+3) { blips[i].rssi=rssi; blips[i].bearing=bearing; }
        else blips[i].rssi=(blips[i].rssi*3+rssi)/4;
        blips[i].seen=millis(); return;
    }
    if(blip_n<MAX_BLIPS) blips[blip_n++]={id,name,rssi,bearing,millis()};
    else {
        int old=0; for(int i=1;i<MAX_BLIPS;++i) if(blips[i].seen<blips[old].seen) old=i;
        blips[old]={id,name,rssi,bearing,millis()};
    }
}

static float rssi_radius(int rssi) {
    // Empirical relative distance, not meters: -35 dBm ≈ center, -95 dBm ≈ edge.
    float q=(float)(rssi+35)/60.0f; q=constrain(q,0.0f,1.0f); return 7.0f+(1.0f-q)*45.0f;
}

static void radar_draw(float heading,bool wifi) {
    face_canvas->fillSprite(cfg.color_bg); int cx=64,cy=63,R=48;
    for(int r=16;r<=R;r+=16) face_canvas->drawCircle(cx,cy,r,0x2145);
    face_canvas->drawLine(cx-R,cy,cx+R,cy,0x2145); face_canvas->drawLine(cx,cy-R,cx,cy+R,0x2145);
    // Compass needle rotates with the physical device. Source dots use absolute
    // calibrated bearings, so they remain stationary in world space.
    float a=(heading-90)*PI/180.0f;
    face_canvas->drawLine(cx,cy,cx+R*cosf(a),cy+R*sinf(a),cfg.color_accent);
    for(int i=0;i<blip_n;++i){
        if(millis()-blips[i].seen>5000) continue;
        float b=(blips[i].bearing-90)*PI/180.0f; float rr=rssi_radius(blips[i].rssi);
        int x=cx+(int)(rr*cosf(b)),y=cy+(int)(rr*sinf(b));
        uint16_t col=millis()-blips[i].seen<1200?cfg.color_primary:0x8410;
        face_canvas->fillCircle(x,y,3,col);
    }
    face_canvas->setTextColor(cfg.color_accent);face_canvas->setTextDatum(top_left);
    face_canvas->drawString(wifi?"WIFI RADAR":"BLE RADAR",3,2);
    face_canvas->setTextDatum(top_right);face_canvas->drawString(String(blip_n),125,2);
    face_canvas->setTextDatum(bottom_center);face_canvas->setTextColor(0x9CD3);face_canvas->drawString("double click = menu",64,126);
    face_canvas->pushSprite(0,0);
}

void tool_menu_ble_radar() {
    if(!compass_calibrate()) { tool_radio_release(); return; }
    radio_enter_ble(); blip_n=0; speak("BLE is live. rotate slowly and watch the dots~");
    uint32_t t0=millis();
    while(millis()-t0<120000) {
        M5.update(); ButtonEvent ev=input_button_event(); if(ev==BTN_DOUBLE) break;
        float h=imu_heading_deg();
        BLEScanResults* res=ble_scan->start(1,false);
        if(res){int n=res->getCount(); for(int i=0;i<n;++i){BLEAdvertisedDevice d=res->getDevice(i);String id=String(d.getAddress().toString().c_str());String nm=d.haveName()?String(d.getName().c_str()):id.substring(0,8);blip_upsert(id,nm,d.getRSSI(),h);} ble_scan->clearResults();}
        radar_draw(h,false); delay(8);
    }
    int found=blip_n; tool_radio_release(); speak(String("BLE radar saw ")+found+" sources~");
}

const char* tool_ble_scan_report() {
    static char buf[220]; radio_enter_ble(); int n=0,best=-999;String who="";
    BLEScanResults* res=ble_scan->start(3,false); if(res){n=res->getCount();for(int i=0;i<n;++i){auto d=res->getDevice(i);if(d.getRSSI()>best){best=d.getRSSI();who=d.haveName()?String(d.getName().c_str()):String(d.getAddress().toString().c_str());}}ble_scan->clearResults();}
    snprintf(buf,sizeof(buf),"{\"ble_devices\":%d,\"closest\":\"%s\",\"rssi\":%d}",n,who.c_str(),best); tool_radio_release(); return buf;
}

void tool_menu_wifi_radar() {
    if(!compass_calibrate()) { tool_radio_release(); return; }
    radio_enter_wifi(); blip_n=0; speak("WiFi is live. rotate slowly so I can map the signals~"); uint32_t t0=millis();
    while(millis()-t0<120000) {
        M5.update(); ButtonEvent ev=input_button_event(); if(ev==BTN_DOUBLE)break;
        float h=imu_heading_deg(); int n=WiFi.scanNetworks(false,true,true,180);
        if(n>0)for(int i=0;i<n;++i){String bssid=WiFi.BSSIDstr(i);blip_upsert(bssid,WiFi.SSID(i),WiFi.RSSI(i),h);} WiFi.scanDelete();
        radar_draw(h,true); delay(12);
    }
    int found=blip_n; tool_radio_release(); speak(String("WiFi radar mapped ")+found+" sources~");
}

const char* tool_wifi_scan_report() {
    static char buf[220]; radio_enter_wifi(); int n=WiFi.scanNetworks(false,true,true,180);int best=-999;String who="";
    if(n>0)for(int i=0;i<n;++i)if(WiFi.RSSI(i)>best){best=WiFi.RSSI(i);who=WiFi.SSID(i);} WiFi.scanDelete();
    snprintf(buf,sizeof(buf),"{\"wifi_aps\":%d,\"strongest\":\"%s\",\"rssi\":%d}",max(n,0),who.c_str(),best); tool_radio_release(); return buf;
}

// ---------------------------------------------------------------------------
// Passive WiFi tools. They never transmit and always disable promiscuous mode
// before leaving.
// ---------------------------------------------------------------------------
static uint32_t sniff_mgmt=0,sniff_data=0,sniff_ctrl=0,pwn_open_beacons=0,pwn_eapol=0,pwn_pmkid=0,pwn_probes=0,pwn_score=0,pwn_level=1;
static inline uint16_t le16(const uint8_t*p){return p[0]|(p[1]<<8);}
static void pwn_inspect(const uint8_t*pkt,int len){
    if(len<24)return;uint16_t fc=le16(pkt);uint8_t ft=(fc>>2)&3,fs=(fc>>4)&15;
    if(ft==0&&fs==4){pwn_probes++;return;} if(ft==0&&fs==8){if(len>=36){uint16_t cap=le16(pkt+34);if(!(cap&0x10))pwn_open_beacons++;}return;}
    if(ft==2)for(int i=24;i+8<len&&i<48;++i)if(pkt[i]==0xAA&&pkt[i+1]==0xAA&&pkt[i+2]==3&&pkt[i+6]==0x88&&pkt[i+7]==0x8E){pwn_eapol++;return;}
}
static void sniffer_cb(void*buf,wifi_promiscuous_pkt_type_t type){auto*p=(wifi_promiscuous_pkt_t*)buf;if(type==WIFI_PKT_MGMT)sniff_mgmt++;else if(type==WIFI_PKT_DATA)sniff_data++;else if(type==WIFI_PKT_CTRL)sniff_ctrl++;if(p&&p->rx_ctrl.sig_len>0)pwn_inspect(p->payload,p->rx_ctrl.sig_len);}

void tool_menu_sniffer(){
    radio_enter_wifi(); speak("passive WiFi sniffing is live. double click exits~"); pwn_open_beacons=pwn_eapol=pwn_pmkid=pwn_probes=pwn_score=0;pwn_level=1;esp_wifi_set_promiscuous_rx_cb(&sniffer_cb);esp_wifi_set_promiscuous(true);
    int ch=1;uint32_t last=0,t0=millis();
    while(millis()-t0<120000){M5.update();ButtonEvent ev=input_button_event();if(ev==BTN_DOUBLE)break;if(millis()-last>250){ch=(ch%13)+1;esp_wifi_set_channel(ch,WIFI_SECOND_CHAN_NONE);last=millis();pwn_score=pwn_open_beacons*2+pwn_eapol*5+pwn_pmkid*10;pwn_level=1+(uint32_t)(sqrtf((float)pwn_score)/3.0f);}
        face_canvas->fillSprite(cfg.color_bg);face_canvas->setTextColor(cfg.color_accent);face_canvas->setTextDatum(top_left);face_canvas->drawString(String("HUNT ch ")+ch+"  Lv"+pwn_level,3,2);face_canvas->setTextColor(0xFFFF);face_canvas->drawString(String("open ")+pwn_open_beacons,5,28);face_canvas->drawString(String("EAPOL ")+pwn_eapol,5,48);face_canvas->drawString(String("probes ")+pwn_probes,5,68);face_canvas->drawString(String("frames ")+sniff_mgmt+"/"+sniff_data,5,88);face_canvas->drawString("double click = menu",5,112);face_canvas->pushSprite(0,0);delay(40);}
    esp_wifi_set_promiscuous(false); tool_radio_release(); speak(String("I heard ")+pwn_eapol+" EAPOL frames and "+pwn_probes+" probe requests~"); sniff_mgmt=sniff_data=sniff_ctrl=0;
}

static char probe_names[8][33];static int probe_count=0;
static void probe_cb(void*buf,wifi_promiscuous_pkt_type_t type){if(type!=WIFI_PKT_MGMT)return;auto*p=(wifi_promiscuous_pkt_t*)buf;const uint8_t*pkt=p->payload;int len=p->rx_ctrl.sig_len;if(len<26)return;uint16_t fc=le16(pkt);if(((fc>>2)&3)!=0||((fc>>4)&15)!=4)return;if(pkt[24]!=0)return;int slen=pkt[25];if(slen<=0||slen>32||26+slen>len)return;char ssid[33];memcpy(ssid,pkt+26,slen);ssid[slen]=0;for(int i=0;i<probe_count;++i)if(!strcmp(probe_names[i],ssid))return;if(probe_count<8)strncpy(probe_names[probe_count++],ssid,32);}
void tool_menu_probe_sniff(){
    radio_enter_wifi();probe_count=0;speak("listening for probe requests~");esp_wifi_set_promiscuous_rx_cb(&probe_cb);esp_wifi_set_promiscuous(true);int ch=1;uint32_t last=0,t0=millis();
    while(millis()-t0<120000){M5.update();ButtonEvent ev=input_button_event();if(ev==BTN_DOUBLE)break;if(millis()-last>300){ch=(ch%13)+1;esp_wifi_set_channel(ch,WIFI_SECOND_CHAN_NONE);last=millis();}face_canvas->fillSprite(cfg.color_bg);face_canvas->setTextColor(cfg.color_accent);face_canvas->setTextDatum(top_left);face_canvas->drawString(String("PROBES ch ")+ch,3,3);for(int i=0;i<probe_count;++i){face_canvas->setTextColor(0xFFFF);String n=String(probe_names[i]);if(n.length()>18)n=n.substring(0,18)+"…";face_canvas->drawString(n,5,22+i*12);}if(!probe_count){face_canvas->setTextColor(0x9CD3);face_canvas->drawString("listening...",5,30);}face_canvas->pushSprite(0,0);delay(40);}
    esp_wifi_set_promiscuous(false);tool_radio_release();speak(String("saw ")+probe_count+" requested network names~");
}
