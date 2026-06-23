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
// 持续 drone 正弦(人偶用): 目标频率/音量由外部设, 引擎里一阶平滑过渡
#define NDR 3                                          // 多层 drone(SHAKE 太空 pad 用; 人偶只用 layer0)
static volatile float drFreqT[NDR]={0}, drAmpT[NDR]={0}, drToneT=0.f;
static float drInc[NDR]={0}, drAmp[NDR]={0}, drPhase[NDR]={0}, drTone=0.f;
void seDrone(float freqHz,float amp){ drFreqT[0]=freqHz; drAmpT[0]=amp; }
void seDroneLayer(int i,float freqHz,float amp){ if(i<0||i>=NDR)return; drFreqT[i]=freqHz; drAmpT[i]=amp; }
void seDroneTone(float t){ if(t<0)t=0; if(t>1)t=1; drToneT=t; }
// 连续单音(滑音合成器): 频率快速跟踪目标(让调用方的滑音/颤音通过), sine<->saw 音色
static volatile float ldFreqT=0.f, ldAmpT=0.f, ldToneT=0.f;
static float ldInc=0.f, ldAmp=0.f, ldPhase=0.f, ldTone=0.f;
void seLead(float f,float a,float t){ ldFreqT=f; ldAmpT=a; if(t<0)t=0; if(t>1)t=1; ldToneT=t; }
// 无缝循环播放器(SHAKE 变速): 整段循环, 边界淡接防咔哒, 速度可实时连续调
static volatile bool loopOn=false;
static const int16_t* loopBuf=NULL; static volatile uint32_t loopLen=0;
static volatile float loopBaseInc=1.0f, loopRate=1.0f, loopAmpT=0.0f;
static float loopPhase=0, loopAmp=0;
void seLoopStart(const int16_t* buf,uint32_t len,uint32_t baseRate){ loopBuf=buf; loopLen=len; loopBaseInc=(float)baseRate/(float)SR; loopPhase=0; loopRate=1.0f; loopOn=true; }
void seLoopRate(float r){ if(r<0.05f)r=0.05f; if(r>4.0f)r=4.0f; loopRate=r; }
void seLoopAmp(float a){ if(a<0)a=0; if(a>1)a=1; loopAmpT=a; }
void seLoopStop(){ loopOn=false; loopAmpT=0; }
// 内部 looper(LOOP station): 采集现场乐器 -> 循环 -> 叠录。缓冲由 app 提供。
static int16_t* lpBuf=NULL; static volatile uint32_t lpMax=0, lpLen=0, lpPos=0; static volatile int lpState=0;  // 0空 1录 2放 3叠
// 空间效果(主延迟): 缓冲由 app 提供(进 SYNTH 分配/退出释放), 不占常驻内存
static int16_t* fxBuf=NULL; static volatile uint32_t fxLen=0; static uint32_t fxPos=0;
static volatile float fxWet=0.f, fxFb=0.f, fxDamp=1.f; static float fxLp=0.f;
void seLooperSet(int16_t* b,uint32_t m){ lpState=0; lpBuf=b; lpMax=m; lpLen=0; lpPos=0; }
void seLooperRec(){ if(!lpBuf)return; lpPos=0; lpLen=0; lpState=1; }
void seLooperClose(){ if(lpState==1){ lpLen=lpPos; lpPos=0; lpState=(lpLen>0)?2:0; } }
void seLooperOverdub(bool on){ if(lpLen>0) lpState=on?3:2; }
void seLooperClear(){ lpState=0; lpLen=0; lpPos=0; }
int seLooperState(){ return lpState; }
uint32_t seLooperPos(){ return lpPos; }
uint32_t seLooperLen(){ return lpLen; }
void seSetCrush(int level){
  switch(level){ case 1: crushRate=2; crushBits=7; break; case 2: crushRate=3; crushBits=6; break;
    case 3: crushRate=4; crushBits=5; break; default: crushRate=1; crushBits=16; break; }
}

struct Voice{ volatile bool active, releasing; float phase, inc, incT, gl, env, rel, peak; int wave, midi, stage; long gate;
  const int16_t* smp; uint32_t smpLen; };
static const int NV=18;
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

static int alloc(){ int s=-1;
  for(int i=0;i<NV;i++) if(!v[i].active){ s=i; break; }          // 优先空闲槽
  if(s<0) for(int i=0;i<NV;i++) if(v[i].releasing){ s=i; break; }// 其次释放中的槽
  if(s<0){ float lo=1e9f; s=0; for(int i=0;i<NV;i++){ float e=v[i].env*v[i].peak;  // 都忙: 抢最安静的(咔哒最小)
    if(e<lo){lo=e;s=i;} } }
  v[s].gl=1.0f; return s; }   // 默认无滑音; seTrigGlide 再设 gl<1

static void audioTask(void*){
  static int16_t buf[128*2];
  while(running){
    for(int n=0;n<128;n++){
      float mix=0;
      for(int i=0;i<NV;i++){ Voice&vc=v[i]; if(!vc.active)continue;
        if(vc.gl<1.0f) vc.inc += (vc.incT-vc.inc)*vc.gl;   // 滑音(portamento): 频率向目标缓动
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
      // 持续 drone: 频率与音量都向目标做一阶平滑 -> 三态间无缝过渡
      for(int L=0;L<NDR;L++){
        float incT=drFreqT[L]*WL/(float)SR;
        drInc[L] += (incT-drInc[L])*0.0004f;
        drAmp[L] += (drAmpT[L]-drAmp[L])*0.0006f;
        int dph=((int)drPhase[L])&(WL-1); float s;
        if(L==0){ drTone += (drToneT-drTone)*0.0008f;     // layer0 保留 暗(正弦)<->亮(锯齿)(人偶用)
          float dsin=(WT[3][dph]-128)*(1.0f/128.0f), dsaw=(WT[0][dph]-128)*(1.0f/128.0f);
          s=dsin*(1.0f-drTone)+dsaw*drTone; }
        else s=(WT[3][dph]-128)*(1.0f/128.0f);            // 其余层: 纯正弦
        drPhase[L]+=drInc[L]; if(drPhase[L]>=WL)drPhase[L]-=WL;
        mix += s*drAmp[L];
      }
      // 连续单音 lead(滑音合成器)
      { float incT=ldFreqT*WL/(float)SR;
        ldInc += (incT-ldInc)*0.03f;       // 快速跟踪 -> 滑音/颤音由 app 决定
        ldAmp += (ldAmpT-ldAmp)*0.01f;
        ldTone += (ldToneT-ldTone)*0.002f;
        int lph=((int)ldPhase)&(WL-1);
        float lsin=(WT[3][lph]-128)*(1.0f/128.0f), lsaw=(WT[0][lph]-128)*(1.0f/128.0f);
        float lv=lsin*(1.0f-ldTone)+lsaw*ldTone;
        ldPhase+=ldInc; if(ldPhase>=WL)ldPhase-=WL;
        mix += lv*ldAmp;
      }
      // 无缝循环播放(SHAKE 变速): 线性插值 + 边界淡接
      loopAmp += (loopAmpT-loopAmp)*0.002f;
      if(loopOn && loopBuf && loopLen>1){
        uint32_t i0=(uint32_t)loopPhase; if(i0>=loopLen)i0=0; uint32_t i1=i0+1; if(i1>=loopLen)i1=0;
        float frac=loopPhase-(float)i0;
        float sv=(loopBuf[i0]+(loopBuf[i1]-loopBuf[i0])*frac)*(1.0f/32768.0f);
        const uint32_t fd=160; float e=1.0f;       // ~10ms 边界淡接防咔哒
        if(loopLen>2*fd){ if(i0<fd)e=(float)i0/fd; else if(i0>=loopLen-fd)e=(float)(loopLen-i0)/fd; }
        mix += sv*loopAmp*e;
        loopPhase += loopBaseInc*loopRate*gBend; if(loopPhase>=loopLen)loopPhase-=loopLen;
      }
      // 内部 looper: 录现场乐器 / 循环回放 / 叠录
      if(lpBuf){
        float liveVal=mix;
        if(lpState==1){ float v=liveVal; if(v>1)v=1; if(v<-1)v=-1;
          if(lpPos<lpMax){ lpBuf[lpPos++]=(int16_t)(v*32767); lpLen=lpPos; } else { lpLen=lpPos; lpPos=0; lpState=2; } }
        else if(lpState==2 || lpState==3){ if(lpLen>0){
          float old=lpBuf[lpPos]*(1.0f/32768.0f);
          if(lpState==3){ float s=old+liveVal; if(s>1)s=1; if(s<-1)s=-1; lpBuf[lpPos]=(int16_t)(s*32767); }
          mix = liveVal + old;                    // 现场 + 循环 一起听
          lpPos++; if(lpPos>=lpLen)lpPos=0; } }
      }
      mix*=gVol*0.30f;
      mix = tanhf(mix);                           // 软限幅: 平滑压峰, 防数字削波/爆炸
      static float lpf=0; lpf += 0.82f*(mix-lpf); mix=lpf;   // 温和一阶低通(~4-5kHz), 削刺耳高频
      if(fxBuf && fxLen){                          // 空间效果: 带阻尼/反馈的延迟(回声/混响感)
        float d=fxBuf[fxPos]*(1.0f/32768.0f);
        float in=mix*0.55f + d*fxFb;               // 进延迟线: 干声压低, 防累积
        fxLp += fxDamp*(in-fxLp); in=fxLp;         // 高频阻尼
        in=tanhf(in);                              // 软限幅延迟线 -> 反馈累积也不硬爆
        fxBuf[fxPos]=(int16_t)(in*32760.0f); fxPos++; if(fxPos>=fxLen)fxPos=0;
        mix=tanhf(mix + d*fxWet);                  // 混入湿声并软限幅
      }
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
  for(int L=0;L<NDR;L++){ drFreqT[L]=0; drAmpT[L]=0; drInc[L]=0; drAmp[L]=0; drPhase[L]=0; } drToneT=0.f; drTone=0.f;  // 复位 drone
  ldFreqT=0.f; ldAmpT=0.f; ldToneT=0.f; ldInc=0.f; ldAmp=0.f; ldPhase=0.f; ldTone=0.f;   // 复位 lead
  loopOn=false; loopAmp=0.f; loopAmpT=0.f; loopPhase=0.f; loopRate=1.f;                  // 复位循环播放
  lpBuf=NULL; lpState=0; lpLen=0; lpPos=0;                                               // 复位 looper
  fxBuf=NULL; fxLen=0; fxPos=0; fxLp=0;                                                  // 复位空间效果
  for(int i=0;i<NV;i++){ v[i].active=false; v[i].releasing=false; v[i].env=0; v[i].gate=-1; v[i].smp=NULL; }
  A_RATE=1.0f/(0.014f*SR); D_RATE=(1.0f-S_LVL)/(0.10f*SR);   // 起音 ~14ms: 音符渐入更柔
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
void seEnd(){ running=false; delay(40); fxBuf=NULL; fxLen=0; i2s_driver_uninstall(I2S_NUM_1); }

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
// 滑音触发(303 风格): 从 fromMidi 起音, 缓动到 midi; 其余同 seTrig
void seTrigGlide(int midi,int wave,uint32_t gateMs,float peak,float detune,int fromMidi,uint32_t glideMs){
  int s=alloc(); Voice&vc=v[s]; vc.active=false; vc.smp=NULL; vc.midi=midi; vc.wave=wave&3; vc.phase=0;
  vc.inc=midiFreq(fromMidi)*WL/(float)SR*detune;          // 起点音高
  vc.incT=midiFreq(midi)*WL/(float)SR*detune;             // 目标音高
  vc.gl=(glideMs<1)?1.0f:1.0f/((float)glideMs*0.001f*SR); if(vc.gl>1)vc.gl=1; if(vc.gl<0.00005f)vc.gl=0.00005f;
  vc.env=0.0001f; vc.stage=0; vc.rel=1.0f/(0.18f*SR);
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
// 短促脉冲(人偶炸开): 任意频率, 不走 midi; wave 3=正弦 4=噪声
void seBlip(float freqHz,uint32_t ms,float amp,int wave){
  int s=alloc(); Voice&vc=v[s]; vc.active=false; vc.smp=NULL; vc.wave=wave; vc.phase=0; vc.midi=-1;
  vc.inc=freqHz*WL/(float)SR; vc.env=0.0001f; vc.stage=0; vc.rel=1.0f/(0.03f*SR);
  vc.peak=amp; vc.gate=(long)((uint64_t)ms*SR/1000); if(vc.gate<1)vc.gate=1; vc.releasing=false; vc.active=true;
}
// 加窗颗粒(SHAKE 颗粒合成): 用全局 A_RATE 起音淡入 + gate 末尾淡出 = 三角窗, 防咔哒
void seGrain(const int16_t* buf,uint32_t len,uint32_t baseRate,float detune,float peak){
  if(!buf||len==0)return;
  int s=alloc(); Voice&vc=v[s]; vc.active=false; vc.smp=buf; vc.smpLen=len; vc.midi=-1;
  vc.inc=(baseRate/(float)SR)*detune; if(vc.inc<=0.0001f)vc.inc=0.0001f; vc.phase=0;
  uint32_t outSamps=(uint32_t)((float)len/vc.inc);     // 输出采样数
  uint32_t relS=(uint32_t)(0.010f*SR); if(relS<8)relS=8;
  vc.rel=1.0f/relS;
  vc.gate=(outSamps>relS+4)?(long)(outSamps-relS):1L;   // 末尾 ~10ms 触发释放淡出
  vc.env=0.0001f; vc.stage=0;                           // 起音淡入(全局 A_RATE)
  vc.peak=peak; vc.releasing=false; vc.active=true;
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
void seFxSet(int16_t* buf, uint32_t len, float wet, float fb, float damp){   // 空间效果: 设缓冲+参数(buf=NULL/len=0 关闭)
  if(!buf||len==0){ fxBuf=NULL; fxLen=0; return; }
  fxWet=wet; fxFb=fb; fxDamp=damp; fxLen=len; if(fxPos>=len)fxPos=0; fxBuf=buf;
}
void seFxClear(){ fxBuf=NULL; fxLen=0; }   // 释放前先调它, 让音频任务停止读
