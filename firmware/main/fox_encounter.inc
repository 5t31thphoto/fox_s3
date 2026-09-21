// fox_encounter.inc — interactive fox play modes and tilt maze.
#pragma once
extern M5Canvas* face_canvas;
extern FoxConfig cfg;
extern FoxNeeds needs;
extern bool g_goto_menu;

static ButtonEvent fox_game_button(){ return input_button_event(); }

void enc_pet(){
    speak("pet me by rolling gently side to side~ click for a chin scratch!");
    int strokes=0, scratches=0; float last=0; uint32_t t0=millis();
    while(millis()-t0<15000 && strokes<8){
        M5.update(); ButtonEvent ev=fox_game_button(); if(ev==BTN_DOUBLE){g_goto_menu=true;return;} if(ev==BTN_TAP){scratches++;needs_interact(needs,true);face_set_mouth(.65f);voice_babble(MOOD_HAPPY,1);}
        float rx,ry;tilt_xy(&rx,&ry); if((rx>.42f&&last<=0)||(rx<-.42f&&last>=0)){strokes++;last=rx;needs_interact(needs,true);}
        face_canvas->fillSprite(cfg.color_bg); face_set_mouth(.12f+fabsf(rx)*.35f); face_draw(MOOD_HAPPY);
        face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(bottom_center);face_canvas->drawString(String("pets ")+strokes+"/8  scratches "+scratches,64,125);face_canvas->pushSprite(0,0);delay(24);
    }
    speak(strokes>=8?"purr purr! perfect grooming~":String("that was ")+strokes+" pets. more?~");
}

void enc_feed(){
    speak("aim with tilt, click to toss the treat, catch it on the way down!");
    float aim=0; int fed=0,miss=0; uint32_t t0=millis();
    while(millis()-t0<18000 && fed<5){
        M5.update(); ButtonEvent ev=fox_game_button(); if(ev==BTN_DOUBLE){g_goto_menu=true;return;}
        float rx,ry;tilt_xy(&rx,&ry); aim=constrain(aim+rx*1.4f,-45.f,45.f);
        bool caught=false;
        if(ev==BTN_TAP){
            for(int f=0;f<=16;++f){face_draw(MOOD_EXCITED);float tx=64+aim,ty=112-f*5+(int)(0.55f*f*f);face_canvas->fillCircle((int)tx,(int)ty,4,cfg.color_accent);face_canvas->pushSprite(0,0);delay(20);}
            caught=fabsf(aim)<18; if(caught){fed++;needs_interact(needs,true);voice_babble(MOOD_HAPPY,2);}else miss++;
        }
        face_canvas->fillSprite(cfg.color_bg);face_draw(caught?MOOD_HAPPY:MOOD_EXCITED);
        face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(top_center);face_canvas->drawString("tilt to aim",64,4);face_canvas->drawString(String("fed ")+fed+"  missed "+miss,64,116);face_canvas->pushSprite(0,0);delay(20);
    }
    speak(String("nom tally: ")+fed+" treats~");
}

void enc_tug(){
    speak("hold me level, click to grab, then flick back and forth!");
    int pull=50, swipes=0; float last=0; bool grabbed=false; uint32_t t0=millis();
    while(millis()-t0<15000 && pull>0&&pull<100){
        M5.update(); ButtonEvent ev=fox_game_button(); if(ev==BTN_DOUBLE){g_goto_menu=true;return;} if(ev==BTN_TAP){grabbed=!grabbed;audio_tone(grabbed?680:380,45);}
        float rx,ry;tilt_xy(&rx,&ry); if(grabbed&&((ry>.5f&&last<=0)||(ry<-.5f&&last>=0))){pull+=7;swipes++;last=ry;needs_interact(needs,true);} pull-=1;
        face_draw(grabbed?MOOD_EXCITED:MOOD_HAPPY);face_canvas->fillRect(8,60,112,10,0x39C7);face_canvas->fillRect(8,60,(int)(104*pull/100.f),10,cfg.color_primary);face_canvas->drawFastVLine(64,50,30,0xFFFF);face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(top_center);face_canvas->drawString(grabbed?"GRAB! FLICK!":"CLICK TO GRAB",64,4);face_canvas->drawString(String("pull ")+pull+"  swipes "+swipes,64,116);face_canvas->pushSprite(0,0);delay(24);
    }
    speak(pull>=100?"you won the tug!":"hehe, my paws are stronger~");
}

void enc_random(){
    // Four distinct modes; the fourth is a tiny "mirror me" challenge.
    if((esp_random()%4)==0){
        speak("mirror my mood: tilt left for sleepy, right for excited!");uint32_t t0=millis();int ok=0;
        while(millis()-t0<9000&&ok<3){M5.update();ButtonEvent ev=fox_game_button();if(ev==BTN_DOUBLE){g_goto_menu=true;return;}float rx,ry;tilt_xy(&rx,&ry);FoxMood target=(ok&1)?MOOD_EXCITED:MOOD_SLEEPY;bool hit=(ok&1)?rx>.45f:rx<-.45f;face_canvas->fillSprite(cfg.color_bg);face_draw(target);face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(bottom_center);face_canvas->drawString(String("round ")+String(ok+1)+"/3",64,125);face_canvas->pushSprite(0,0);if(hit){ok++;needs_interact(needs,true);voice_babble(MOOD_HAPPY,1);}delay(20);}speak(ok==3?"you know my moods!":"close enough~");
    } else { switch(esp_random()%3){case 0:enc_pet();break;case 1:enc_feed();break;default:enc_tug();break;} }
}

#define MAZ_W 16
#define MAZ_H 16
static uint8_t maze[MAZ_H][MAZ_W];
static void maze_gen(){
    for(int y=0;y<MAZ_H;++y)for(int x=0;x<MAZ_W;++x)maze[y][x]=(x==0||y==0||x==MAZ_W-1||y==MAZ_H-1)?1:((esp_random()%100)<18?1:0);
    // Guaranteed monotonic corridor from start to exit.
    int x=1,y=1;maze[y][x]=0;while(x<MAZ_W-2||y<MAZ_H-2){if(x<MAZ_W-2&&(y>=MAZ_H-2||(esp_random()&1)))x++;else y++;maze[y][x]=0;}
    maze[MAZ_H-2][MAZ_W-2]=4;
    for(int i=0;i<12;++i){int tx=1+esp_random()%(MAZ_W-2),ty=1+esp_random()%(MAZ_H-2);if(maze[ty][tx]==0)maze[ty][tx]=(esp_random()&1)?2:3;}
}

void game_maze(){
    speak("maze time. tilt to steer, click to dash, double click for menu~");maze_gen();int px=1,py=1,hp=5,treats=0;uint32_t last=0;
    while(true){
        M5.update();ButtonEvent ev=fox_game_button();if(ev==BTN_DOUBLE){g_goto_menu=true;return;}
        float rx,ry;tilt_xy(&rx,&ry); if(millis()-last>140){int nx=px,ny=py;if(fabsf(rx)>fabsf(ry)){if(rx>.28f)nx++;else if(rx<-.28f)nx--;}else{if(ry>.28f)ny++;else if(ry<-.28f)ny--;}
            if(nx!=px||ny!=py){if(nx>=0&&nx<MAZ_W&&ny>=0&&ny<MAZ_H&&maze[ny][nx]!=1){px=nx;py=ny;uint8_t&c=maze[py][px];if(c==2){treats++;c=0;needs_interact(needs,true);voice_babble(MOOD_HAPPY,1);}else if(c==3){c=0;if((esp_random()%100)>=60)hp--;}else if(c==4){speak(String("escaped with ")+treats+" treats~");return;}}last=millis();}
        }
        if(ev==BTN_TAP){ // dash one tile in the current tilt direction
            int nx=px,ny=py;if(fabsf(rx)>fabsf(ry))nx+=(rx>0)-(rx<0);else ny+=(ry>0)-(ry<0);if(nx>=0&&nx<MAZ_W&&ny>=0&&ny<MAZ_H&&maze[ny][nx]!=1){px=nx;py=ny;}
        }
        if(hp<=0){speak("the maze fox needs a snack break~");return;}
        face_canvas->fillSprite(0x0000);for(int y=0;y<MAZ_H;++y)for(int x=0;x<MAZ_W;++x){uint16_t c=maze[y][x]==1?0x39C7:maze[y][x]==2?0xFD20:maze[y][x]==3?0xF800:maze[y][x]==4?0x07E0:0;if(c)face_canvas->fillRect(x*8,y*8,8,8,c);}face_canvas->fillCircle(px*8+4,py*8+4,3,cfg.color_primary);face_canvas->setTextColor(0xFFFF);face_canvas->setTextDatum(top_left);face_canvas->drawString(String("HP ")+hp+"  treats "+treats,2,1);face_canvas->pushSprite(0,0);delay(16);
    }
}
