// 实时合成引擎 — 后台任务实时混音, 写 i2s TX (I2S_NUM_1)。无 M5Unified。
#include <Arduino.h>
#include "driver/i2s.h"
#include "ES8311Audio.h"
#include "synth_engine.h"

extern ES8311Audio audio;

#define PIN_SCLK 41
#define PIN_LRCK 43
#define PIN_DOUT 42
static const int WL=256;
static uint8_t WT[5][WL];          // 0锯齿 1方波 2三角 3正弦 4噪声
static uint32_t SR=16000;
static float gVol=0.6f;
static float drumGain=1.0f;
static int toneH=10;            // 波形谐波数=亮度 (2暗..14亮)
static int crushRate=1, crushBits=16;   // bitcrush: 采样保持步长 / 量化位数
static volatile float gBend=1.0f;       // 全局速度倍率(磁带停)
void seSetBend(float f){ gBend=f; }
void seSetCrush(int level){
  switch(level){ case 1: crushRate=2; crushBits=7; break; case 2: crushRate=3; crushBits=6; break;
    case 3: crushRate=4; crushBits=5; break; default: crushRate=1; crushBits=16; break; }
}

struct Voice{ volatile bool active, releasing; float phase, inc, env, rel, peak; int wave, midi, stage; long gate;
  const int16_t* smp; uint32_t smpLen; };
static const int NV=10;
static Voice v[NV];
static TaskHandle_t taskH=NULL;
static volatile bool running=false;
static float A_RATE, D_RATE; static const float S_LVL=0.7f;
static const float R_DEF=1.0f/(0.25f*16000);

static void buildWaves(){
  float saw[WL],sqr[WL],tri[WL],ms=0.001f,mq=0.001f,mt=0.001f;
  for(int i=0;i<WL;i++){ float ph=2*PI*i/WL; float s=0,q=0,t=0;
    for(int h=1;h<=toneH;h++){ s+=sinf(h*ph)/h; if(h&1){q+=sinf(h*ph)/h; int k=(h-1)/2; t+=((k&1)?-1.f:1.f)*sinf(h*ph)/(h*h);} }
    saw[i]=s;sqr[i]=q;tri[i]=t; if(fabsf(s)>ms)ms=fabsf(s); if(fabsf(q)>mq)mq=fabsf(q); if(fabsf(t)>mt)mt=fabsf(t); }
  for(int i=0;i<WL;i++){ float ph=2*PI*i/WL;
    WT[0][i]=(uint8_t)(128+110*saw[i]/ms); WT[1][i]=(uint8_t)(128+110*sqr[i]/mq);
    WT[2][i]=(uint8_t)(128+110*tri[i]/mt); WT[3][i]=(uint8_t)(128+110*sinf(ph));
    WT[4][i]=(uint8_t)random(0,256); }
}
void seSetTone(int h){ if(h<2)h=2; if(h>14)h=14; toneH=h; buildWaves(); }
void seSetDrumVol(float v){ drumGain=v; }
static float midiFreq(int m){ return 440.0f*powf(2.0f,(m-69)/12.0f); }

static int alloc(){ for(int i=0;i<NV;i++) if(!v[i].active)return i;
  for(int i=0;i<NV;i++) if(v[i].releasing)return i; return 0; }

static void audioTask(void*){
  static int16_t buf[128*2];
  while(running){
    for(int n=0;n<128;n++){
      float mix=0;
      for(int i=0;i<NV;i++){ Voice&vc=v[i]; if(!vc.active)continue;
        float s;
        if(vc.smp){ if(vc.phase<0){ vc.active=false; continue; } uint32_t idx=(uint32_t)vc.phase; if(idx>=vc.smpLen){ vc.active=false; continue; }
          s=vc.smp[idx]*(1.0f/32768.0f); vc.phase+=vc.inc*gBend; }
        else { s=(WT[vc.wave][((int)vc.phase)&(WL-1)]-128)*(1.0f/128.0f); vc.phase+=vc.inc*gBend; if(vc.phase>=WL)vc.phase-=WL; }
        mix += s*vc.env*vc.peak;
        if(vc.gate>0){ vc.gate--; if(vc.gate==0)vc.releasing=true; }
        if(vc.releasing){ vc.env-=vc.rel; if(vc.env<=0){vc.env=0;vc.active=false;} }
        else if(vc.stage==0){ vc.env+=A_RATE; if(vc.env>=1.0f){vc.env=1.0f;vc.stage=1;} }
        else if(vc.stage==1){ vc.env-=D_RATE; if(vc.env<=S_LVL){vc.env=S_LVL;vc.stage=2;} }
      }
      mix*=gVol*0.30f;
      if(mix>1)mix=1; if(mix<-1)mix=-1;
      mix = mix - (mix*mix*mix)*0.16f;            // 轻微软削顶, 防爆音
      static float heldMix=0; static int holdCnt=0;   // bitcrush
      if(crushRate>1){ if(--holdCnt<=0){ heldMix=mix; holdCnt=crushRate; } mix=heldMix; }
      if(crushBits<16){ float lv=(float)(1<<crushBits); mix=floorf(mix*lv+0.5f)/lv; }
      int16_t o=(int16_t)(mix*30000); buf[n*2]=o; buf[n*2+1]=o;
    }
    size_t bw; i2s_write(I2S_NUM_1,buf,sizeof(buf),&bw,portMAX_DELAY);
  }
  vTaskDelete(NULL);
}

void seBegin(uint32_t sampleRate){
  SR=sampleRate; buildWaves();
  for(int i=0;i<NV;i++){ v[i].active=false; v[i].releasing=false; v[i].env=0; v[i].gate=-1; v[i].smp=NULL; }
  A_RATE=1.0f/(0.008f*SR); D_RATE=(1.0f-S_LVL)/(0.10f*SR);
  audio.enableSpkForStream();
  i2s_config_t c={}; c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_TX);
  c.sample_rate=SR; c.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT; c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1; c.dma_buf_count=6; c.dma_buf_len=128;
  c.use_apll=false; c.tx_desc_auto_clear=true; c.mclk_multiple=I2S_MCLK_MULTIPLE_256; c.bits_per_chan=I2S_BITS_PER_CHAN_16BIT;
  i2s_driver_install(I2S_NUM_1,&c,0,NULL);
  i2s_pin_config_t p={}; p.mck_io_num=I2S_PIN_NO_CHANGE; p.bck_io_num=PIN_SCLK; p.ws_io_num=PIN_LRCK; p.data_out_num=PIN_DOUT; p.data_in_num=I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1,&p); i2s_zero_dma_buffer(I2S_NUM_1);
  running=true;
  xTaskCreatePinnedToCore(audioTask,"synth",4096,NULL,5,&taskH,0);
}
void seEnd(){ running=false; delay(40); i2s_driver_uninstall(I2S_NUM_1); }

void seNoteOn(int midi,int wave){
  int s=alloc(); Voice&vc=v[s]; vc.active=false; vc.smp=NULL; vc.midi=midi; vc.wave=wave&3; vc.phase=0;
  vc.inc=midiFreq(midi)*WL/(float)SR; vc.env=0.0001f; vc.stage=0; vc.rel=R_DEF*16000/SR;
  vc.peak=1; vc.gate=-1; vc.releasing=false; vc.active=true;
}
void seNoteOff(int midi){ for(int i=0;i<NV;i++) if(v[i].active&&!v[i].releasing&&v[i].midi==midi&&v[i].gate<0) v[i].releasing=true; }
void seTrig(int midi,int wave,uint32_t gateMs,float peak,float detune){
  int s=alloc(); Voice&vc=v[s]; vc.active=false; vc.smp=NULL; vc.midi=midi; vc.wave=wave&3; vc.phase=0;
  vc.inc=midiFreq(midi)*WL/(float)SR*detune; vc.env=0.0001f; vc.stage=0; vc.rel=1.0f/(0.18f*SR);
  vc.peak=peak; vc.gate=(long)((uint64_t)gateMs*SR/1000); if(vc.gate<1)vc.gate=1; vc.releasing=false; vc.active=true;
}
// 用录音(采样)当音色: buf=样本数据, len=样本数, baseRate=录音采样率, 以 C4(60)为基准音变调
void seSampleTrig(int midi,const int16_t* buf,uint32_t len,uint32_t baseRate,uint32_t gateMs,float peak,float detune,bool rev){
  if(!buf||len==0)return;
  int s=alloc(); Voice&vc=v[s]; vc.active=false; vc.smp=buf; vc.smpLen=len;
  vc.inc=(baseRate/(float)SR)*powf(2.0f,(midi-60)/12.0f)*detune;
  if(rev){ vc.phase=(float)(len-1); vc.inc=-vc.inc; } else vc.phase=0;   // 反向播放
  vc.env=1.0f; vc.stage=2; vc.rel=1.0f/(0.04f*SR);
  vc.peak=peak; vc.gate=gateMs?(long)((uint64_t)gateMs*SR/1000):-1; vc.releasing=false; vc.active=true;
}
static void perc(int wave,float inc,float relSec,float peak){
  int s=alloc(); Voice&vc=v[s]; vc.smp=NULL; vc.phase=0; vc.env=1.0f; vc.stage=2; vc.releasing=true;
  vc.gate=-1; vc.active=true; vc.midi=-1; vc.wave=wave; vc.inc=inc; vc.rel=1.0f/(relSec*SR); vc.peak=peak*drumGain;
}
// type: 0kick 1snare 2hat 3clap 4tom 5openhat 6rim 7cowbell ; kit: 0ACOU 1=808 2=909
void seDrum(int type,int kit){
  const float K=WL/(float)SR;   // Hz -> phase inc
  switch(type){
    case 0: if(kit==1) perc(3,46*K,0.40f,1.3f); else if(kit==2){ perc(3,58*K,0.14f,1.3f); perc(2,150*K,0.04f,0.5f);} else perc(3,62*K,0.13f,1.3f); break;
    case 1: if(kit==1){ perc(4,8,0.14f,0.85f); perc(2,175*K,0.12f,0.5f);} else if(kit==2){ perc(4,11,0.13f,0.95f); perc(2,225*K,0.10f,0.6f);} else { perc(4,9,0.12f,0.95f); perc(2,190*K,0.10f,0.5f);} break;
    case 2: perc(4, kit==2?20:16, 0.04f, 0.6f); break;
    case 3: perc(4,12,0.10f,0.7f); perc(4,13,0.14f,0.5f); break;
    case 4: if(kit==1) perc(3,80*K,0.28f,1.1f); else perc(3,98*K,0.17f,1.1f); break;
    case 5: perc(4, kit==2?16:15, 0.20f, 0.5f); break;
    case 6: perc(4,22,0.03f,0.8f); break;
    case 7: if(kit==1){ perc(1,545*K,0.12f,0.65f); perc(1,845*K,0.12f,0.5f);} else { perc(1,540*K,0.10f,0.6f); perc(1,800*K,0.10f,0.5f);} break;
  }
}
void seAllOff(){ for(int i=0;i<NV;i++){ v[i].active=false; v[i].releasing=false; v[i].env=0; } }
void seSetVolume(float x){ gVol=x; }
