// fox_demo.inc — tactile visual toys. Single click is owned by each toy;
// double click is the universal menu/escape gesture.
#pragma once
#include "fox_audio.h"
extern M5Canvas* face_canvas;
extern FoxConfig cfg;

static uint16_t hsv565(float h, float s, float v) {
    h = fmodf(h, 360); if (h < 0) h += 360;
    s = constrain(s, 0.0f, 1.0f); v = constrain(v, 0.0f, 1.0f);
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
    float r, g, b;
    if (h < 60)      { r = c; g = x; b = 0; }
    else if (h < 120){ r = x; g = c; b = 0; }
    else if (h < 180){ r = 0; g = c; b = x; }
    else if (h < 240){ r = 0; g = x; b = c; }
    else if (h < 300){ r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    uint8_t R = (uint8_t)((r + m) * 31), G = (uint8_t)((g + m) * 63), B = (uint8_t)((b + m) * 31);
    return (R << 11) | (G << 5) | B;
}

static ButtonEvent toy_button() { return input_button_event(); }

void toy_plasma() {
    speak("make me dance~ tilt, sound, and click to change the palette!");
    audio_speaker_end();
    float t = 0; int mode = 0; uint32_t last_audio = 0; float level = 0;
    while (true) {
        M5.update();
        ButtonEvent ev = toy_button();
        if (ev == BTN_DOUBLE) return;
        if (ev == BTN_TAP) { mode = (mode + 1) % 4; audio_tone(420 + mode * 100, 45); }
        float rx, ry; tilt_xy(&rx, &ry);
        if (millis() - last_audio > 55) { level = audio_level_rms(); last_audio = millis(); }
        float speed = 0.03f + fabsf(ry) * 0.12f + level * 0.22f;
        float hue0 = mode * 90.0f + rx * 110.0f + level * 140.0f;
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            float v = 0.25f + 0.18f * sinf(x*.42f+t*7) + 0.18f*sinf(y*.51f-t*6)
                    + 0.15f*sinf((x+y)*.29f+t*9) + 0.12f*sinf(hypotf(x-16,y-16)*.38f-t*8);
            v += level * 0.28f;
            face_canvas->fillRect(x*4,y*4,4,4,hsv565(hue0 + v*100, .92f, v));
        }
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_left);
        face_canvas->drawString(String("PLASMA  ") + (int)(level*100) + "%  mode " + (mode+1), 2, 2);
        face_canvas->pushSprite(0,0);
        t += speed;
        delay(16);
    }
}

void toy_starfield() {
    speak("to infinity~");
    const int N = 90; static float sx[N], sy[N], sz[N];
    for (int i=0;i<N;++i){sx[i]=random(-64,64);sy[i]=random(-64,64);sz[i]=random(1,64);}
    while (true) {
        M5.update(); ButtonEvent ev=toy_button(); if(ev==BTN_DOUBLE)return;
        float rx,ry; tilt_xy(&rx,&ry); face_canvas->fillSprite(0x0000);
        for(int i=0;i<N;++i){
            sz[i]-=1.2f+fabsf(ry)*2; sx[i]+=rx*2;
            if(sz[i]<=1){sx[i]=random(-64,64);sy[i]=random(-64,64);sz[i]=64;}
            int px=64+(int)(sx[i]/sz[i]*64), py=64+(int)(sy[i]/sz[i]*64);
            if(px>=0&&px<128&&py>=0&&py<128) face_canvas->fillCircle(px,py,(int)(3*(1-sz[i]/64))+1,hsv565(200+rx*100,.35f,1-sz[i]/64));
        }
        if(ev==BTN_TAP) { for(int i=0;i<N;++i) sz[i]=min(sz[i],12.0f); audio_tone(760,45); }
        face_canvas->setTextColor(0xFFFF); face_canvas->setTextDatum(top_left); face_canvas->drawString("STARFIELD  click=warp",2,2);
        face_canvas->pushSprite(0,0); delay(16);
    }
}

void toy_ink() {
    speak("roll the ink through sound~ click to splash it!");
    audio_speaker_end();
    struct Dot { float x,y,age; } trail[96]; int head=0, count=0, mode=0;
    float x=64,y=64,vx=0,vy=0; uint32_t last_audio=0; float level=0;
    while(true){
        M5.update(); ButtonEvent ev=toy_button();
        if(ev==BTN_DOUBLE)return;
        float rx,ry; tilt_xy(&rx,&ry);
        vx += rx*0.48f; vy += ry*0.48f; vx*=0.94f; vy*=0.94f;
        if(ev==BTN_TAP){ mode=(mode+1)%3; for(int i=0;i<14;++i){ float a=i*0.45f; trail[head]={x,y,0}; head=(head+1)%96; if(count<96)count++; vx+=cosf(a)*1.8f; vy+=sinf(a)*1.8f; } audio_tone(520+mode*160,55); }
        x+=vx; y+=vy;
        if(x<3){x=3;vx=fabsf(vx)*.8f;} if(x>124){x=124;vx=-fabsf(vx)*.8f;}
        if(y<12){y=12;vy=fabsf(vy)*.8f;} if(y>124){y=124;vy=-fabsf(vy)*.8f;}
        if(millis()-last_audio>65){level=audio_level_rms();last_audio=millis();}
        trail[head]={x,y,0}; head=(head+1)%96; if(count<96)count++;
        face_canvas->fillSprite(0x0000);
        int start=(head-count+96)%96;
        for(int i=0;i<count;++i){int idx=(start+i)%96; float a=1.0f-(float)i/count; float bri=.18f+.82f*a; uint16_t c=hsv565(210+mode*100+level*160+i*1.5f,.9f,bri); int r=1+(int)(3*a+level*3); face_canvas->fillCircle((int)trail[idx].x,(int)trail[idx].y,r,c);}
        int br=3+(int)(level*7); face_canvas->fillCircle((int)x,(int)y,br,hsv565(170+mode*80,.95f,1));
        face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(top_left);face_canvas->drawString(String("INK  sound ")+(int)(level*100)+"%  mode "+(mode+1),2,2);
        face_canvas->pushSprite(0,0); delay(16);
    }
}

void toy_spiro() {
    speak("turn me to draw; click changes the gear~");
    audio_speaker_end();
    face_canvas->fillSprite(cfg.color_bg); face_canvas->pushSprite(0,0);
    float a=0, last_t=millis(), level=0; int gear=3; uint32_t last_audio=0;
    while(true){
        M5.update(); ButtonEvent ev=toy_button();
        if(ev==BTN_DOUBLE)return;
        if(ev==BTN_TAP){gear++;if(gear>8)gear=2;audio_tone(350+gear*70,45);}
        float gx,gy,gz; imu_rotation_dps(&gx,&gy,&gz); uint32_t now=millis(); float dt=(now-last_t)/1000.0f; last_t=now; if(dt>0.05f)dt=0.05f;
        float turn=fabsf(gz)>8?gz:(fabsf(gx)>8?gx:gy); // any deliberate rotation draws
        if(fabsf(turn)>8){
            a += turn*dt*0.018f;
            if(millis()-last_audio>55){level=audio_level_rms();last_audio=millis();}
            float R=43,r=R/gear,d=15+level*22; float hue=gear*36+level*280;
            for(int k=0;k<max(1,(int)(fabsf(turn)/10));++k){
                float q=a+k*0.012f; float x=64+(R-r)*cosf(q)+d*cosf((R-r)/r*q); float y=64+(R-r)*sinf(q)-d*sinf((R-r)/r*q);
                if(x>1&&x<127&&y>1&&y<127)face_canvas->fillCircle((int)x,(int)y,1,hsv565(hue,.95f,.9f));
            }
            face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(top_left);face_canvas->drawString(String("SPIRO gear ")+gear+"  sound "+(int)(level*100)+"%",2,2); face_canvas->pushSprite(0,0);
        } else {
            // No rotation: preserve the drawing exactly. Only refresh the tiny hint.
            delay(12);
        }
    }
}
