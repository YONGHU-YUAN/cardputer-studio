// 音序器 — 自研引擎上 (双轨 A/B + 8鼓3组 + swing/概率/和弦 + 存档 + 录音当音色)。无 M5Unified。
#include <Arduino.h>
#include "Display.h"
#include "CardputerKeyboard.h"
#include "synth_engine.h"
#include "seq.h"
#include <SD.h>
#include <SPI.h>

extern LGFX lcd;
extern CardputerKeyboard kb;
extern SPIClass sdSPI;
static LGFX_Sprite spr(&lcd);

static const int STEPS=32, MAXN=6, PAGEW=16;
static int loopLen=16;
static int pat[2][STEPS][MAXN];
static int trackSrc[2]={0,3};     // 0-3=波形, 4+=采样(idx=src-4)
static uint8_t drum[STEPS];
static int curStep=0, editStep=0, bpm=112, activeTrack=0, kit=0;
// 鼓组: synthType>=0 用合成鼓(seDrum 类型); files!=NULL 用真实采样(8 垫文件名 z x c v b n m q)
struct KitDef{ const char* name; int synthType; const char* const* files; };
static const char* SK_808 [8]={"BD808","SD808","CH808","OH808","LT808","CP808","RS808","CB808"};  // 真 TR-808
static const char* SK_909 [8]={"BD909","SD909","CH909","OH909","LT909","CP909","RS909","CB909"};  // 真 TR-909(q=ride)
static const char* SK_ACUS[8]={"ABD","ASD","ACH","AOH","ALT","ACP","ARS","ACB"};                 // 真原声 Gretsch
static const char* SK_LOFI[8]={"LBD","LSD","LCH","LOH","LLT","LCP","LRS","LCB"};                 // lo-fi 处理
static const char* SK_RTRO[8]={"RBD","RSD","RCH","ROH","RLT","RCP","RRS","RCB"};                 // 复古 DrumTraks
static const KitDef KITS[]={
  {"ACOU",0,NULL}, {"808",-1,SK_808}, {"909",-1,SK_909},
  {"ACUS",-1,SK_ACUS}, {"LOFI",-1,SK_LOFI}, {"RTRO",-1,SK_RTRO},
};
static const int NKIT=(int)(sizeof(KITS)/sizeof(KITS[0]));
static const int NSLOT=16;          // 存档槽数(SD上几KB一首,放心加)
static int slot=0; static char msg[24]=""; static uint32_t msgUntil=0;
static int pendingLoad=-1;
void seqQueueLoad(int s){ pendingLoad=s; }
static float swing=0; static uint8_t prob[2][STEPS];
static float trackVol[4]={0.85f,0.85f,1.0f,1.0f};   // A B 鼓 乐句
static float gPoly=1.0f;   // 和弦复音分摊系数: 一格多个音时按音数压低每音, 防叠加削波("炸")
static int toneV=10;
static int crushV=0;   // bitcrush 等级 0-3 (f 键)
static bool tapeActive=false; static uint32_t tapeT0=0;   // 磁带停 (k 键)
static int arpRate=0; static uint32_t arpClock=0; static int arpIdx=0;   // 琶音器 (a 键): 0关 2/3/4每格下数
static bool revPlay=false;   // 律动反向 (fn+l)
static bool playing=false, helpOn=false; static int helpPage=0;
static uint32_t stepStart=0;
static uint16_t BG,INK,GREY,ACC,PLAY,CUR,GRID,SEL;
static const char* WAVN[4]={"SAW","SQR","TRI","SIN"};
static uint16_t PCOL[12];

// ---- SD 录音/采样列表 + 每轨采样缓冲 ----
#define MAXLIST 40
static int listN=0; static char listName[MAXLIST][24]; static char listPath[MAXLIST][72];
static int16_t* trackBuf[2]={0,0}; static uint32_t trackLen[2]={0,0}, trackRate[2]={16000,16000};
static const uint32_t SAMP_MAX=16000;   // A/B 每轨采样 ~1s, 省内存
// 鼓轨采样垫: 8 条 lane 各可选一条录音 (-1=合成鼓, >=0=录音list索引)
static int8_t laneSamp[8]={-1,-1,-1,-1,-1,-1,-1,-1};
static int16_t* laneBuf[8]={0}; static uint32_t laneLen[8]={0}, laneRate[8]={16000,16000,16000,16000,16000,16000,16000,16000};
static int kitLane=0;
static const uint32_t KIT_MAX=4800;     // 每垫 ~0.3s, 省内存
// 采样鼓组: 当前选中的那套(KITS[kit].files)装进 kitBuf, 8 垫各一条 WAV; 同一时刻只占一套内存
static int16_t* kitBuf[8]={0}; static uint32_t kitLen[8]={0}, kitRate[8]={16000,16000,16000,16000,16000,16000,16000,16000};
static bool kitLoaded=false; static int kitLoadedIdx=-1;   // 当前装进 kitBuf 的是哪个鼓组
// 空间效果(主延迟): 缓冲只在进 SYNTH 时分配, 退出释放(不占常驻内存/不碰录音机)
static int16_t* fxBuffer=NULL; static const uint32_t FX_MAX=8000; static int spaceMode=0;   // 0关 1回声 2空间
static void applySpaceFx(){
  if(!fxBuffer || spaceMode==0){ seFxClear(); return; }
  uint32_t stepMs=60000/bpm/4;
  if(spaceMode==1){ uint32_t dl=(stepMs*2)*16; if(dl>FX_MAX)dl=FX_MAX; if(dl<1)dl=1; seFxSet(fxBuffer,dl,0.38f,0.42f,0.55f); }  // ECHO: 1/8 拍回声
  else { uint32_t dl=70*16; if(dl>FX_MAX)dl=FX_MAX; seFxSet(fxBuffer,dl,0.30f,0.50f,0.35f); }                                   // SPACE: 短延迟+中反馈=混响感(防爆)
}

// ---- 乐句轨 D: 长录音留SD, 只载入选段 ----
static int phraseSrc=-1;                 // list idx, -1=未选
static char phrasePath[72]="";
static uint32_t phTotal=0, phRate=16000, phDataOff=0;   // 整段帧数/采样率/data偏移
static uint8_t phOv[200];                // 波形缩略(整段)
static uint32_t phStart=0, phSeg=0;      // 选区(帧)
static int16_t* phBuf=NULL; static uint32_t phBufLen=0;  // 已载入的选段
static const uint32_t PH_SEG_MAX=24000;  // 选段最长 ~1.5s
static bool phTrig[STEPS];
static bool fx[2][5]={{0},{0}};   // 每轨 A/B 各自: DLY OCT SUB 5TH CHO
static uint8_t phStepFx[STEPS]={0};   // 每个乐句触发点的效果: 0无 1OCT 2SUB 3CHO 4DLY
static uint8_t phStepLen[STEPS]={0};  // 每格播放长度: 0=整段, 否则 ×50ms
static uint8_t phStepOff[STEPS]={0};  // 每格起点偏移(段内): ×50ms
static int8_t phStepPit[STEPS]={0};   // 每格变调(半音)
static uint8_t slideStep[2][STEPS]={{0},{0}};   // 旋律轨逐格滑音开关(opt 切换)
static uint8_t noteLen[2][STEPS];   // 旋律轨逐格音符长度(占几格, 1-8); + 键调; seqEnter 初始化为 1
static uint8_t accent[3][STEPS];    // 逐格力度: 0普通 1重音 2弱音 (轨0/1=旋律A/B, 2=鼓)
static float gAccent=1.0f;          // 当前触发力度倍率(同 gPoly 的全局传参法)
static float accScale(uint8_t a){ return a==1?1.35f : a==2?0.5f : 1.0f; }
static uint8_t lenUp(uint8_t v){ const uint8_t S[6]={1,2,3,4,6,8}; for(int i=0;i<6;i++) if(S[i]==v) return S[i<5?i+1:5]; return 2; }
static uint8_t lenDn(uint8_t v){ const uint8_t S[6]={1,2,3,4,6,8}; for(int i=0;i<6;i++) if(S[i]==v) return S[i>0?i-1:0]; return 1; }
static int lastNote[2]={-1,-1};                 // 各轨上一个音(滑音起点)
static const char* PHFXN[6]={"OCT","SUB","CHO","DLY","REV","STU"};
static const char* FXN[5]={"DLY","OCT","SUB","5TH","CHO"};
static uint16_t FXC[5];
struct Echo{int t,midi;uint32_t due;float vol;uint32_t off,len;bool rev;}; static Echo eq[24]; static int eqn=0;

static int midiOfKey(const char* k){
  static const char* n[]={"z","s","x","d","c","v","g","b","h","n","j","m","q","w","e","r","t","y","u","i","o","p"};
  static const int m[]={60,61,62,63,64,65,66,67,68,69,70,71,72,74,76,77,79,81,83,84,86,88};
  for(int i=0;i<22;i++) if(!strcmp(k,n[i]))return m[i]; return -1;
}
static bool stepHas(int t,int s,int x){ for(int k=0;k<MAXN;k++) if(pat[t][s][k]==x)return true; return false; }
static bool stepAdd(int t,int s,int x){ if(stepHas(t,s,x))return false; for(int k=0;k<MAXN;k++) if(pat[t][s][k]<0){pat[t][s][k]=x;return true;} return false; }
static void stepDel(int t,int s,int x){ for(int k=0;k<MAXN;k++) if(pat[t][s][k]==x)pat[t][s][k]=-1; }
static void stepClear(int t,int s){ for(int k=0;k<MAXN;k++) pat[t][s][k]=-1; }
static int pitchY(int n,int top,int h){ const int LO=60,HI=88; if(n<LO)n=LO; if(n>HI)n=HI;  // 对齐键盘音域 z(60)..p(88)
  int span=h-8; int y=top+3+(int)((long)(HI-n)*span/(HI-LO)); return y; }
static uint16_t pcolOf(int n){ int i=(n-60)*12/29; if(i<0)i=0; if(i>11)i=11; return PCOL[i]; }   // 颜色按音高 z=红..p=蓝
static void previewT(int t,int n);    // 前向声明
static void reverseBuf(int16_t* b,uint32_t n){ if(!b||n<2)return; for(uint32_t i=0,j=n-1;i<j;i++,j--){ int16_t t=b[i]; b[i]=b[j]; b[j]=t; } }

static void drawWaveIcon(int x,int y,int wave,uint16_t col){
  int w=16,h=8,my=y+h/2;
  switch(wave){
    case 0: spr.drawLine(x,y+h,x+w/2,y,col); spr.drawLine(x+w/2,y,x+w/2,y+h,col); spr.drawLine(x+w/2,y+h,x+w,y,col); break;
    case 1: spr.drawLine(x,my,x,y,col); spr.drawLine(x,y,x+w/2,y,col); spr.drawLine(x+w/2,y,x+w/2,y+h,col); spr.drawLine(x+w/2,y+h,x+w,y+h,col); spr.drawLine(x+w,y+h,x+w,my,col); break;
    case 2: spr.drawLine(x,my,x+w/4,y,col); spr.drawLine(x+w/4,y,x+3*w/4,y+h,col); spr.drawLine(x+3*w/4,y+h,x+w,my,col); break;
    default:{ int px=x,py=my; for(int i=1;i<=w;i++){ int ny=my-(int)((h/2)*sinf(2*PI*i/w)); spr.drawLine(px,py,x+i,ny,col); px=x+i; py=ny; } } break;
  }
}

// ---- WAV 读入缓冲(16bit, 单/双声道; 截断到 SAMP_MAX) ----
static bool loadWavBuf(const char* path, int16_t** outBuf, uint32_t* outLen, uint32_t* outRate, uint32_t maxFrames=SAMP_MAX){
  File f=SD.open(path,FILE_READ); if(!f)return false;
  uint8_t h[12]; if(f.read(h,12)!=12||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)){f.close();return false;}
  uint16_t ch=1,bits=16; uint32_t rate=16000,dOff=0,dSz=0;
  while(f.available()>=8){ uint8_t c[8]; if(f.read(c,8)!=8)break;
    uint32_t cs=c[4]|(c[5]<<8)|(c[6]<<16)|((uint32_t)c[7]<<24);
    if(!memcmp(c,"fmt ",4)){ uint8_t fm[16]; uint32_t tr=cs<16?cs:16; f.read(fm,tr);
      ch=fm[2]|(fm[3]<<8); rate=fm[4]|(fm[5]<<8)|(fm[6]<<16)|((uint32_t)fm[7]<<24); bits=fm[14]|(fm[15]<<8);
      if(cs>16)f.seek(f.position()+(cs-16)); if(cs&1)f.seek(f.position()+1);
    } else if(!memcmp(c,"data",4)){ dOff=f.position(); dSz=cs; break; } else f.seek(f.position()+cs+(cs&1)); }
  if(dOff==0||dSz==0||bits!=16||ch<1||ch>2){f.close();return false;}
  uint32_t fsz=f.size(); if(dOff+dSz>fsz)dSz=fsz-dOff;
  uint32_t frames=dSz/(2*ch); if(frames>maxFrames)frames=maxFrames; if(frames==0){f.close();return false;}
  int16_t* buf=(int16_t*)malloc(frames*2); if(!buf){f.close();return false;}
  f.seek(dOff); static int16_t tmp[512*2]; uint32_t got=0;
  while(got<frames){ uint32_t want=frames-got; if(want>512)want=512; int rd=f.read((uint8_t*)tmp,want*2*ch); uint32_t fr=rd/(2*ch); if(fr==0)break;
    for(uint32_t i=0;i<fr;i++) buf[got+i]= ch==1? tmp[i] : (int16_t)((tmp[i*2]+tmp[i*2+1])/2); got+=fr; }
  f.close(); *outBuf=buf; *outLen=got; *outRate=rate; return true;
}
static void scanListDir(const char* dir){
  File root=SD.open(dir); if(!root||!root.isDirectory()){if(root)root.close();return;}
  File e; while(listN<MAXLIST && (e=root.openNextFile())){ const char* n=e.name();
    if(!e.isDirectory()){ const char* d=strrchr(n,'.'); if(d&&(!strcmp(d,".wav")||!strcmp(d,".WAV"))&&n[0]!='.'){
      if(n[0]=='/')snprintf(listPath[listN],72,"%s",n); else snprintf(listPath[listN],72,"%s/%s",dir,n);
      strncpy(listName[listN],strrchr(listPath[listN],'/')+1,23); listName[listN][23]=0; char* dot=strrchr(listName[listN],'.'); if(dot)*dot=0; listN++; } }
    e.close(); }
  root.close();
}
static void scanList(){ listN=0; scanListDir("/samples"); scanListDir("/VoiceMemos"); }

static void setTrackSample(int t,int idx){       // idx into list
  if(idx<0||idx>=listN)return;
  if(trackBuf[t]){ seAllOff(); delay(10); free(trackBuf[t]); trackBuf[t]=NULL; }   // 先停声等音频任务跑完, 再释放
  if(loadWavBuf(listPath[idx],&trackBuf[t],&trackLen[t],&trackRate[t])) trackSrc[t]=4+idx;
}
// 采样垫: 给鼓轨某条 lane 装/卸录音
static void freeLanes(){ for(int r=0;r<8;r++){ if(laneBuf[r]){ seAllOff(); delay(5); free(laneBuf[r]); laneBuf[r]=NULL; } laneLen[r]=0; } }
static void loadLane(int r,int listIdx){
  if(laneBuf[r]){ seAllOff(); delay(5); free(laneBuf[r]); laneBuf[r]=NULL; laneLen[r]=0; }
  if(listIdx>=0 && listIdx<listN) loadWavBuf(listPath[listIdx], &laneBuf[r], &laneLen[r], &laneRate[r], KIT_MAX);
}
static void freeSampleKit(){ for(int r=0;r<8;r++){ if(kitBuf[r]){ seAllOff(); delay(5); free(kitBuf[r]); kitBuf[r]=NULL; kitLen[r]=0; } } kitLoaded=false; kitLoadedIdx=-1; }
static void loadSampleKit(int k){    // 从 /drums/ 直接读 KITS[k].files 的 8 个采样(独立于用户采样列表, 不污染 SAMPLER)
  if(kitLoaded && kitLoadedIdx==k) return;   // 已是这套, 不重复加载
  freeSampleKit();
  const char* const* fl=KITS[k].files; if(!fl) return;
  char p[40];
  for(int r=0;r<8;r++){ if(!fl[r]||!fl[r][0]) continue;
    snprintf(p,40,"/drums/%s.wav",fl[r]);
    loadWavBuf(p,&kitBuf[r],&kitLen[r],&kitRate[r],KIT_MAX); }
  kitLoaded=true; kitLoadedIdx=k;
}
static void selectKit(int k){ if(KITS[k].files) loadSampleKit(k); else freeSampleKit(); }   // 切到合成鼓则释放采样
static void trigDrumLane(int r){
  if(laneSamp[r]>=0 && laneBuf[r] && laneLen[r]) seSampleTrig(60,laneBuf[r],laneLen[r],laneRate[r],0,trackVol[2]*gAccent);   // 该 lane 单独绑的采样优先
  else if(KITS[kit].files && kitBuf[r] && kitLen[r]) seSampleTrig(60,kitBuf[r],kitLen[r],kitRate[r],0,trackVol[2]*gAccent);  // 采样鼓组
  else { seSetDrumVol(trackVol[2]*gAccent); int st=KITS[kit].synthType; seDrum(r, st<0?2:st); }                              // 合成鼓(采样缺垫回退 909)
}

// 解析 WAV 头, 取 dataOff/帧数/采样率/声道
static bool wavInfo(const char* path,uint32_t* dataOff,uint32_t* frames,uint32_t* rate,uint16_t* ch){
  File f=SD.open(path,FILE_READ); if(!f)return false;
  uint8_t h[12]; if(f.read(h,12)!=12||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)){f.close();return false;}
  uint16_t c=1,bits=16; uint32_t r=16000,dOff=0,dSz=0;
  while(f.available()>=8){ uint8_t b[8]; if(f.read(b,8)!=8)break; uint32_t cs=b[4]|(b[5]<<8)|(b[6]<<16)|((uint32_t)b[7]<<24);
    if(!memcmp(b,"fmt ",4)){ uint8_t fm[16]; uint32_t tr=cs<16?cs:16; f.read(fm,tr); c=fm[2]|(fm[3]<<8); r=fm[4]|(fm[5]<<8)|(fm[6]<<16)|((uint32_t)fm[7]<<24); bits=fm[14]|(fm[15]<<8); if(cs>16)f.seek(f.position()+(cs-16)); if(cs&1)f.seek(f.position()+1); }
    else if(!memcmp(b,"data",4)){ dOff=f.position(); dSz=cs; break; } else f.seek(f.position()+cs+(cs&1)); }
  uint32_t fsz=f.size(); f.close(); if(dOff==0||dSz==0||bits!=16||c<1||c>2)return false;
  if(dOff+dSz>fsz)dSz=fsz-dOff; *dataOff=dOff; *frames=dSz/(2*c); *rate=r; *ch=c; return true;
}
static uint16_t g_ch=1;
static void phComputeOverview(){
  for(int i=0;i<200;i++)phOv[i]=0; if(phTotal==0)return;
  File f=SD.open(phrasePath,FILE_READ); if(!f)return;
  static int16_t tmp[256];
  for(int i=0;i<200;i++){ uint32_t fr=(uint64_t)i*phTotal/200; f.seek(phDataOff+(uint64_t)fr*2*g_ch);
    int rd=f.read((uint8_t*)tmp,sizeof(tmp)); int ns=rd/2; int32_t pk=0;
    for(int j=0;j<ns;j++){ int32_t a=tmp[j]<0?-tmp[j]:tmp[j]; if(a>pk)pk=a; } phOv[i]=(uint8_t)(pk>>7); }
  f.close();
}
static void phLoadSeg(){
  if(phBuf){ seAllOff(); delay(10); free(phBuf); phBuf=NULL; phBufLen=0; }   // 先停声等音频任务跑完, 再释放
  if(phrasePath[0]==0||phSeg==0)return; uint32_t seg=phSeg; if(seg>PH_SEG_MAX)seg=PH_SEG_MAX;
  phBuf=(int16_t*)malloc(seg*2); if(!phBuf)return;
  File f=SD.open(phrasePath,FILE_READ); if(!f){free(phBuf);phBuf=NULL;return;}
  f.seek(phDataOff+(uint64_t)phStart*2*g_ch);
  static int16_t tmp[512*2]; uint32_t got=0;
  while(got<seg){ uint32_t want=seg-got; if(want>512)want=512; int rd=f.read((uint8_t*)tmp,want*2*g_ch); uint32_t fr=rd/(2*g_ch); if(fr==0)break;
    for(uint32_t i=0;i<fr;i++) phBuf[got+i]= g_ch==1? tmp[i] : (int16_t)((tmp[i*2]+tmp[i*2+1])/2); got+=fr; }
  f.close(); phBufLen=got;
}
static void phPick(int idx){
  if(idx<0||idx>=listN)return;
  uint32_t dOff,fr,rate; uint16_t ch;
  if(!wavInfo(listPath[idx],&dOff,&fr,&rate,&ch))return;
  strncpy(phrasePath,listPath[idx],71); phrasePath[71]=0; phDataOff=dOff; phTotal=fr; phRate=rate; g_ch=ch; phraseSrc=idx;
  phStart=0; phSeg=(fr<rate)?fr:rate; if(phSeg>PH_SEG_MAX)phSeg=PH_SEG_MAX;     // 默认选 1 秒(或整段), 钳到上限
  phComputeOverview(); phLoadSeg();
}

static void addChord(int root,bool minor){ if(activeTrack>=2)return; int th=minor?3:4; int ns[3]={root,root+th,root+7};
  for(int i=0;i<3;i++){ stepAdd(activeTrack,editStep,ns[i]); previewT(activeTrack,ns[i]); } }

static void playNoteT(int t,int n,uint32_t g){
  if(trackSrc[t]<4) seTrig(n,trackSrc[t],g,trackVol[t]);
  else if(trackBuf[t]) seSampleTrig(n,trackBuf[t],trackLen[t],trackRate[t],g,trackVol[t]);
}
static void previewT(int t,int n){    // 按键试听: 波形短促, 采样放整段
  if(trackSrc[t]<4) seTrig(n,trackSrc[t],300,trackVol[t]);
  else if(trackBuf[t]) seSampleTrig(n,trackBuf[t],trackLen[t],trackRate[t],0,trackVol[t]);
}
static void trigVoiceDet(int t,int n,uint32_t g,float vscale,float det){
  float vol=trackVol[t]*vscale*gPoly*gAccent;
  if(trackSrc[t]<4) seTrig(n,trackSrc[t],g,vol,det);
  else if(trackBuf[t]) seSampleTrig(n,trackBuf[t],trackLen[t],trackRate[t],g,vol,det);
}
static void trigVoice(int t,int n,uint32_t g,float vscale){ trigVoiceDet(t,n,g,vscale,1.0f); }
static void noteWithFx(int t,int n,uint32_t g,uint32_t stepMs){  // 带效果触发(旋律轨, 效果分轨)
  trigVoice(t,n,g,1.0f);
  if(fx[t][1]) trigVoice(t,n+12,g,0.5f);   // OCT
  if(fx[t][2]) trigVoice(t,n-12,g,0.6f);   // SUB
  if(fx[t][3]) trigVoice(t,n+7, g,0.45f);  // 5TH
  if(fx[t][4]){ trigVoiceDet(t,n,g,0.5f,1.006f); trigVoiceDet(t,n,g,0.5f,0.994f); }  // CHO
  if(fx[t][0]){ uint32_t now=millis(); float v=0.5f;    // DLY 回声
    for(int k=1;k<=3;k++){ if(eqn<24){ eq[eqn++]={t,n,now+(uint32_t)(k*3*stepMs),v}; v*=0.6f; } } }
}
// 滑音版: 从 from 滑到 n(仅波形轨; 采样轨退回普通触发)
static void noteWithFxGlide(int t,int n,uint32_t g,int from,uint32_t glideMs){
  if(trackSrc[t]>=4){ noteWithFx(t,n,g,0); return; }
  int w=trackSrc[t]; float vol=trackVol[t]*gPoly;
  seTrigGlide(n,    w,g,vol,      1.0f,  from,    glideMs);
  if(fx[t][1]) seTrigGlide(n+12,w,g,vol*0.5f, 1.0f,  from+12, glideMs);  // OCT
  if(fx[t][2]) seTrigGlide(n-12,w,g,vol*0.6f, 1.0f,  from-12, glideMs);  // SUB
  if(fx[t][3]) seTrigGlide(n+7, w,g,vol*0.45f,1.0f,  from+7,  glideMs);  // 5TH
  if(fx[t][4]){ seTrigGlide(n,w,g,vol*0.5f,1.006f,from,glideMs); seTrigGlide(n,w,g,vol*0.5f,0.994f,from,glideMs); }  // CHO
}

struct Song{ int pat[2][STEPS][MAXN]; uint8_t drum[STEPS]; uint8_t prob[2][STEPS]; uint8_t phTrig[STEPS]; int trackSrc[2]; float trackVol[4]; int bpm, kit, tone, loopLen, phraseSrc; uint32_t phStart, phSeg; float swing; uint8_t fx[2][5]; uint8_t phStepFx[STEPS]; uint8_t phStepLen[STEPS]; uint8_t phStepOff[STEPS]; int8_t phStepPit[STEPS]; uint8_t slideStep[2][STEPS]; uint32_t magic; };
static void saveSong(){
  Song s; memcpy(s.pat,pat,sizeof(pat)); memcpy(s.drum,drum,sizeof(drum)); memcpy(s.prob,prob,sizeof(prob)); memcpy(s.trackSrc,trackSrc,sizeof(trackSrc)); memcpy(s.trackVol,trackVol,sizeof(trackVol));
  for(int i=0;i<STEPS;i++)s.phTrig[i]=phTrig[i]?1:0; for(int t=0;t<2;t++)for(int i=0;i<5;i++)s.fx[t][i]=fx[t][i]?1:0; memcpy(s.phStepFx,phStepFx,sizeof(phStepFx)); memcpy(s.phStepLen,phStepLen,sizeof(phStepLen)); memcpy(s.phStepOff,phStepOff,sizeof(phStepOff)); memcpy(s.phStepPit,phStepPit,sizeof(phStepPit)); memcpy(s.slideStep,slideStep,sizeof(slideStep));
  s.bpm=bpm; s.kit=kit; s.tone=toneV; s.loopLen=loopLen; s.swing=swing; s.phraseSrc=phraseSrc; s.phStart=phStart; s.phSeg=phSeg; s.magic=0x53474E39;
  SD.mkdir("/songs"); char p[32]; snprintf(p,32,"/songs/SONG%d.dat",slot+1);
  File f=SD.open(p,FILE_WRITE); if(f){ f.write((uint8_t*)&s,sizeof(s));
    char ln[8][24]; for(int r=0;r<8;r++){ if(laneSamp[r]>=0&&laneSamp[r]<listN){ strncpy(ln[r],listName[laneSamp[r]],23); ln[r][23]=0; } else ln[r][0]=0; }
    f.write((uint8_t*)ln,sizeof(ln));   // 追加: 采样垫每条 lane 的录音文件名
    f.close(); snprintf(msg,24,"SAVED S%d",slot+1);} else snprintf(msg,24,"SAVE FAIL");
  msgUntil=millis()+900;
}
static void loadSong(){
  char p[32]; snprintf(p,32,"/songs/SONG%d.dat",slot+1); File f=SD.open(p,FILE_READ);
  if(f && (size_t)f.size()>=sizeof(Song)){ Song s; f.read((uint8_t*)&s,sizeof(s));
    bool hasLanes = (size_t)f.size() >= sizeof(Song)+sizeof(char[8][24]);   // 新存档才有采样垫块
    char ln[8][24]; if(hasLanes) f.read((uint8_t*)ln,sizeof(ln));
    f.close();
    if(s.magic==0x53474E39){ memcpy(pat,s.pat,sizeof(pat)); memcpy(drum,s.drum,sizeof(drum)); memcpy(prob,s.prob,sizeof(prob)); memcpy(trackVol,s.trackVol,sizeof(trackVol));
      for(int i=0;i<STEPS;i++)phTrig[i]=s.phTrig[i]; for(int t=0;t<2;t++)for(int i=0;i<5;i++)fx[t][i]=s.fx[t][i]; memcpy(phStepFx,s.phStepFx,sizeof(phStepFx)); memcpy(phStepLen,s.phStepLen,sizeof(phStepLen)); memcpy(phStepOff,s.phStepOff,sizeof(phStepOff)); memcpy(phStepPit,s.phStepPit,sizeof(phStepPit)); memcpy(slideStep,s.slideStep,sizeof(slideStep));
      bpm=s.bpm; kit=s.kit; toneV=s.tone; loopLen=s.loopLen; swing=s.swing; seSetTone(toneV); if(editStep>=loopLen)editStep=0;
      if(kit<0||kit>=NKIT) kit=0; selectKit(kit);   // 读档: 按存的鼓组加载采样(或释放)
      for(int t=0;t<2;t++){ if(s.trackSrc[t]<4) trackSrc[t]=s.trackSrc[t]; else setTrackSample(t, s.trackSrc[t]-4); }
      for(int r=0;r<8;r++){ laneSamp[r]=-1; if(laneBuf[r]){free(laneBuf[r]);laneBuf[r]=NULL;laneLen[r]=0;} }   // 先清空采样垫
      if(hasLanes) for(int r=0;r<8;r++) if(ln[r][0]) for(int i=0;i<listN;i++) if(!strcmp(listName[i],ln[r])){ laneSamp[r]=i; loadLane(r,i); break; }   // 按文件名恢复
      if(s.phraseSrc>=0){ phPick(s.phraseSrc); phStart=s.phStart; phSeg=s.phSeg; phLoadSeg(); }
      snprintf(msg,24,"LOADED S%d",slot+1);}
    else snprintf(msg,24,"BAD FILE"); }
  else { if(f)f.close(); snprintf(msg,24,"S%d EMPTY",slot+1); }
  msgUntil=millis()+900;
}

static void srcName(int t,char* out){ int sv=trackSrc[t]; if(sv<4)strcpy(out,WAVN[sv]); else { int i=sv-4; strncpy(out, (i<listN)?listName[i]:"?",10); out[10]=0; } }

static void draw(){
  if(!spr.getBuffer()){ lcd.fillScreen(BG); return; }
  spr.fillScreen(BG);
  // 顶部状态条(卡片底 + 芯片, 与其它页面统一)
  spr.fillRect(0,0,240,13,CUR); spr.drawFastHLine(0,13,240,GRID); spr.setTextSize(1);
  spr.fillRoundRect(3,2,30,9,2,playing?PLAY:GREY); spr.setTextColor(BG); spr.setCursor(6,3); spr.print(playing?"PLAY":"STOP");
  spr.setTextColor(INK); spr.setCursor(38,3); spr.printf("%d",bpm);
  const char* tn=activeTrack==0?"A":activeTrack==1?"B":activeTrack==2?"DRM":"PHR";
  spr.fillRoundRect(66,2,26,9,2,ACC); spr.setTextColor(BG); spr.setCursor(70,3); spr.print(tn);
  if(activeTrack<2){ char nm[12]; srcName(activeTrack,nm); spr.setTextColor(INK); spr.setCursor(98,3); spr.print(nm);
    if(trackSrc[activeTrack]<4) drawWaveIcon(150,2,trackSrc[activeTrack],PLAY); }
  else if(activeTrack==2){ spr.setTextColor(INK); spr.setCursor(98,3);
    if(laneSamp[kitLane]>=0 && laneSamp[kitLane]<listN) spr.printf("L%d:%.8s",kitLane+1,listName[laneSamp[kitLane]]);
    else spr.printf("L%d:%s %s",kitLane+1,KITS[kit].files?"SMP":"DRM",KITS[kit].name); }
  else { spr.setTextColor(INK); spr.setCursor(98,3); spr.print(phraseSrc>=0?listName[phraseSrc]:"0=pick"); }
  spr.setTextColor(INK); spr.setCursor(196,3); spr.printf("S%d",slot+1);

  int pages=(loopLen+PAGEW-1)/PAGEW; if(pages<1)pages=1;
  int vpage=editStep/PAGEW; if(vpage>=pages)vpage=pages-1; int base=vpage*PAGEW;
  spr.setTextColor(pages>1?PLAY:GREY); spr.setCursor(216,3); spr.printf("%d/%d",vpage+1,pages);

  int gx=4,gw=232,colw=gw/PAGEW, gy=14,gh=50;
  for(int c=0;c<PAGEW;c++){ int s=base+c,x=gx+c*colw; if(s>=loopLen) spr.fillRect(x,gy,colw,gh,CUR); }
  if(activeTrack<2 && editStep>=base&&editStep<base+PAGEW){ int ex=gx+(editStep-base)*colw; spr.drawRect(ex,gy,colw,gh,ACC); spr.drawRect(ex+1,gy+1,colw-2,gh-2,ACC); }  // 选中格: 橙框(不盖音符)
  for(int c=0;c<PAGEW;c++){ int x=gx+c*colw; if(c%4==0)spr.drawFastVLine(x,gy,gh,GRID); }
  spr.drawRect(gx,gy,gw,gh,activeTrack<2?SEL:GRID);
  { int at=(activeTrack<2)?activeTrack:0, ot=1-at;
    for(int c=0;c<PAGEW;c++){ int s=base+c,x=gx+c*colw; bool cur=(s==curStep&&playing);
      for(int k=0;k<MAXN;k++){ int n=pat[ot][s][k]; if(n<0)continue; int y=pitchY(n,gy,gh); spr.drawCircle(x+colw/2,y+2,3,GRID); }
      { int L=noteLen[at][s]; if(L<1)L=1;       // 变长音符: 从音符点向右拖延音横杠
        for(int k=0;k<MAXN;k++){ int n=pat[at][s][k]; if(n<0)continue; int y=pitchY(n,gy,gh);
          if(L>1){ int w=(L-1)*colw; if(x+colw/2+w>gx+gw)w=gx+gw-(x+colw/2); if(w>0)spr.fillRect(x+colw/2,y+1,w,3,cur?INK:pcolOf(n)); } } }
      { int ar=accent[at][s]==1?4:accent[at][s]==2?2:3;   // 重音点大/弱音点小
        for(int k=0;k<MAXN;k++){ int n=pat[at][s][k]; if(n<0)continue; int y=pitchY(n,gy,gh); spr.fillCircle(x+colw/2,y+2,ar,cur?INK:pcolOf(n)); } }
      if(prob[at][s]<100){ int w=(colw-2)*prob[at][s]/100; spr.drawFastHLine(x+1,gy+1,colw-2,CUR); if(w>0)spr.drawFastHLine(x+1,gy+1,w,PLAY); }
      if(slideStep[at][s]) spr.fillRect(x+2,gy+gh-3,colw-4,2,ACC); } }   // 滑音格: 底部橙条

  int dy=66, drh=3, DROWS=8;   // 行距收窄(方块仍厚, 会叠一点)
  if(activeTrack==3){          // 乐句轨: 此区显示波形+选区(供裁剪)
    int oy=66, ohh=24, ox=4, ow=232;
    spr.drawRect(ox,oy,ow,ohh,SEL);
    for(int i=0;i<200;i++){ int x=ox+1+i*(ow-2)/200; int hh=phOv[i]*(ohh-2)/255; spr.drawFastVLine(x,oy+ohh/2-hh/2,hh,GREY); }
    if(phTotal>0){ int sx=ox+1+(uint64_t)phStart*(ow-2)/phTotal; int sw=(uint64_t)phSeg*(ow-2)/phTotal; if(sw<2)sw=2;
      spr.drawRect(sx,oy,sw,ohh,PLAY); spr.fillRect(sx,oy,sw,2,PLAY);
      if(phStepOff[editStep]>0||phStepLen[editStep]>0){   // 当前格窗口(橙框: 可移动+变长)
        uint32_t offFr=(uint32_t)(phStepOff[editStep]*0.05f*phRate); if(offFr>phSeg)offFr=phSeg;
        uint32_t lenFr=phStepLen[editStep]?(uint32_t)(phStepLen[editStep]*0.05f*phRate):(phSeg-offFr);
        if(offFr+lenFr>phSeg)lenFr=phSeg-offFr;
        int ox2=sx+(int)((uint64_t)offFr*(ow-2)/phTotal); int lw=(uint64_t)lenFr*(ow-2)/phTotal; if(lw<2)lw=2;
        spr.drawRect(ox2,oy,lw,ohh,ACC); spr.fillRect(ox2,oy,lw,3,ACC); } }
  } else {                     // 鼓网格
    for(int c=0;c<PAGEW;c++){ int s=base+c,x=gx+c*colw; if(s>=loopLen) spr.fillRect(x,dy,colw,drh*DROWS,CUR); }
    if(activeTrack==2 && editStep>=base&&editStep<base+PAGEW) spr.fillRect(gx+(editStep-base)*colw,dy,colw,drh*DROWS,CUR);
    spr.drawRect(gx,dy,gw,drh*DROWS,activeTrack==2?SEL:GRID);
    for(int r=0;r<DROWS;r++) for(int c=0;c<PAGEW;c++){ int s=base+c,x=gx+c*colw; bool cur=(s==curStep&&playing);
      if(drum[s]&(1<<r)) spr.fillRoundRect(x+2,dy+r*drh,colw-4,5,1, cur?INK:(r&1?lcd.color565(58,80,116):lcd.color565(108,128,158))); }   // 鼓: 深/浅蓝交替区分行
    for(int c=0;c<PAGEW;c++){ int s=base+c,x=gx+c*colw; if(accent[2][s]==1) spr.fillRect(x+2,dy-2,colw-4,2,ACC); else if(accent[2][s]==2) spr.fillRect(x+colw/2-1,dy-2,2,2,GRID); }   // 力度标记: 重音橙条/弱音小点
  }

  // 乐句触发条(常驻最下)
  int py=94, ph=4;
  if(activeTrack==3 && editStep>=base&&editStep<base+PAGEW) spr.fillRect(gx+(editStep-base)*colw,py,colw,ph,CUR);
  spr.drawRect(gx,py,gw,ph,activeTrack==3?SEL:GRID);
  for(int c=0;c<PAGEW;c++){ int s=base+c,x=gx+c*colw; bool cur=(s==curStep&&playing);
    if(phTrig[s]) spr.fillCircle(x+colw/2,py+ph/2,2,cur?INK:(phStepFx[s]?lcd.color565(214,116,54):lcd.color565(98,196,218))); }   // 有效果=橙 无=蓝

  // 播放头
  if(playing && curStep>=base && curStep<base+PAGEW){ int px=gx+(curStep-base)*colw+colw/2;
    spr.drawFastVLine(px,gy,gh,PLAY); if(activeTrack!=3)spr.drawFastVLine(px,dy,drh*DROWS,PLAY); spr.drawFastVLine(px,py,ph,PLAY); }

  // 效果色块(常驻底部)
  int bx=6,bw2=44,bgap=2,by=100;
  if(activeTrack==3){   // 乐句轨: 显示"当前格"的 6 个效果 (1-4 + 9=REV + fn9=STU)
    uint8_t m=phStepFx[editStep]; int pw=37,pg=2;
    for(int i=0;i<6;i++){ int x=6+i*(pw+pg); uint16_t col=i<5?FXC[i]:lcd.color565(150,140,170);
      if(m&(1<<i)){ spr.fillRoundRect(x,by,pw,14,3,col); spr.setTextColor(BG);} else { spr.drawRoundRect(x,by,pw,14,3,col); spr.setTextColor(GREY);}
      spr.setCursor(x+(pw-18)/2,by+4); spr.print(PHFXN[i]); }
  } else {
    int ft=activeTrack<2?activeTrack:0;
    for(int i=0;i<5;i++){ int x=bx+i*(bw2+bgap);
      if(fx[ft][i]){ spr.fillRoundRect(x,by,bw2,14,3,FXC[i]); spr.setTextColor(BG);} else { spr.drawRoundRect(x,by,bw2,14,3,FXC[i]); spr.setTextColor(GREY);}
      spr.setCursor(x+(bw2-18)/2,by+4); spr.print(FXN[i]); }
  }

  if(activeTrack==3){ spr.setTextColor(GREY); spr.setCursor(4,120); spr.printf("1-4/9fx 5/6mv 7/8len fn:1/2pit 9stut"); }
  else { spr.setTextColor(GREY); spr.setCursor(4,120); spr.print("ctrl = help (all keys)"); }

  if(millis()<msgUntil){ int w=strlen(msg)*12+16; spr.fillRoundRect(120-w/2,40,w,24,5,INK);
    spr.setTextColor(BG); spr.setTextSize(2); spr.setCursor(120-strlen(msg)*6,45); spr.print(msg); spr.setTextSize(1); }
  spr.pushSprite(0,0);
}

static void drawHelp(){
  if(!spr.getBuffer())return;
  spr.fillScreen(BG);
  spr.setTextColor(ACC); spr.setTextSize(2); spr.setCursor(6,4);
  spr.print(helpPage==0?"KEYS 1/2":"KEYS 2/2"); spr.setTextSize(1);
  spr.setTextColor(GREY); spr.setCursor(112,8); spr.print("tab=more ctrl=close");
  const char* P0[]={
    "enter=play/stop  space=stop",
    "tab=track  , .=cursor  notekey=write",
    "del=clear   fn+del=clear all",
    "[ ]=bpm     fn+[ ]=loop length",
    "notes  zsxdcvgbhnjm  qwertyuiop",
    "A/B  1-4=wave 0=sample ;'=vol",
    "A/B  fn+;'=tone  FX 5DLY 6OCT 7SUB",
    "A/B  8-5TH 9CHO  fn+,.=prob  opt=slide",
    "A/B  -/+=note len  fn+opt=accent  fn+key=chord",
    "DRUM keys=hit/sel 0=lane snd 1=kit +=accent",
    "SONG  fn+-/fn++ slot   /=save  \\=load" };
  const char* P1[]={
    "PHRASE track (tab over to it):",
    "  0=next rec  fn+0=prev   z=preview",
    "  step 1-4=OCT SUB CHO DLY  9=rev",
    "  step 5/6=offset   7/8=length",
    "  fn+1/2=pitch       fn+9=stutter",
    "  fn+5/6=trim start  fn+7/8=seg len",
    "",
    "LIVE FX:",
    "  f=crush  k=tape stop  a=arp  alt=space",
    "  l=reverse",
    "  fn+k=swing   fn+l=sequence reverse" };
  const char** L = (helpPage==0)?P0:P1;
  int y=22; spr.setTextColor(INK); for(int i=0;i<11;i++){ spr.setCursor(6,y); spr.print(L[i]); y+=10; }
  spr.pushSprite(0,0);
}

static void playPhrase(int m,uint32_t offFr,uint32_t g,int pit){   // m掩码, offFr=起点帧, g=毫秒(0=到末尾), pit=变调
  if(!phBuf||!phBufLen)return; if(offFr>=phBufLen)offFr=0;
  int16_t* p=phBuf+offFr; uint32_t plen=phBufLen-offFr; float v=trackVol[3];
  bool rev=m&16;                                          // REV 反向
  uint32_t winFr=g?(uint32_t)((uint64_t)g*phRate/1000):plen; if(winFr>plen)winFr=plen;
  uint32_t blen=rev?winFr:plen, bg=rev?0:g;              // 反向: 长度界定窗口, 不用gate
  int b=60+pit;
  seSampleTrig(b,p,blen,phRate,bg,v,1.0f,rev);
  if(m&1) seSampleTrig(b+12,p,blen,phRate,bg,v*0.5f,1.0f,rev);            // OCT +12
  if(m&2) seSampleTrig(b-12,p,blen,phRate,bg,v*0.6f,1.0f,rev);           // SUB -12
  if(m&4){ seSampleTrig(b,p,blen,phRate,bg,v*0.5f,1.006f,rev); seSampleTrig(b,p,blen,phRate,bg,v*0.5f,0.994f,rev); }  // CHO
  if(m&8){ uint32_t now=millis(),stepMs=60000/bpm/4; float ev=v*0.55f;   // DLY 回声(重复当前窗口)
    for(int kk=1;kk<=3;kk++){ if(eqn<24){ eq[eqn++]={2,b,now+(uint32_t)(kk*2*stepMs),ev,offFr,blen,rev}; ev*=0.6f; } } }
  if(m&32){ uint32_t now=millis(),sub=60000/bpm/4/4; if(sub<25)sub=25;    // STUT 碎拍(快速重复窗口)
    for(int kk=1;kk<=3;kk++){ if(eqn<24) eq[eqn++]={2,b,now+kk*sub,v*0.85f,offFr,blen,rev}; } }
}
static void fireEcho(Echo&e){    // 回声/碎拍到点触发
  if(e.t==2){ if(phBuf&&phBufLen&&e.off<phBufLen){ uint32_t l=e.len; if(e.off+l>phBufLen)l=phBufLen-e.off; seSampleTrig(e.midi,phBuf+e.off,l,phRate,0,e.vol,1.0f,e.rev); } }
  else trigVoice(e.t,e.midi,140,e.vol);
}
static void trigStep(int s){
  uint32_t stepMs=60000/bpm/4; uint32_t g=stepMs*0.9f; if(g<20)g=20;
  uint32_t glideMs=(uint32_t)(stepMs*0.7f);
  if(arpRate==0) for(int t=0;t<2;t++){ if(prob[t][s]<100 && (int)random(0,100)>=prob[t][s]) continue;   // 琶音开时旋律交给arp
    int cnt=0; for(int k=0;k<MAXN;k++) if(pat[t][s][k]>=0) cnt++;   // 本格音数
    gPoly = cnt>1 ? 1.0f/sqrtf((float)cnt) : 1.0f;                  // 和弦按音数分摊(1/√N): 等响度折中(用户认可的那一版)
    gAccent = accScale(accent[t][s]);                              // 逐格力度(重音/弱音)
    int nl=noteLen[t][s]; if(nl<1)nl=1; uint32_t gg=(uint32_t)(stepMs*nl*0.95f); if(gg<20)gg=20;   // 按本格音符长度定 gate(变长音符)
    int first=-1;
    for(int k=0;k<MAXN;k++){ int n=pat[t][s][k]; if(n>=0){ if(first<0)first=n;
      if(slideStep[t][s] && lastNote[t]>=0) noteWithFxGlide(t,n,gg,lastNote[t],glideMs);   // 滑: 从上一个音滑来
      else noteWithFx(t,n,gg,stepMs); } }
    gPoly=1.0f; gAccent=1.0f;
    if(first>=0) lastNote[t]=first; }   // 记住本格音, 作下一个滑音起点
  gAccent = accScale(accent[2][s]);                                // 鼓的逐格力度
  for(int r=0;r<8;r++) if(drum[s]&(1<<r)) trigDrumLane(r);
  gAccent=1.0f;
  if(phTrig[s]) playPhrase(phStepFx[s], (uint32_t)(phStepOff[s]*0.05f*phRate), phStepLen[s]?phStepLen[s]*50u:0, phStepPit[s]);   // 乐句轨(效果+偏移+长度+变调)
}

// ---- 试听(乐曲库用): 启动引擎+载入某槽并循环播放, 不绘编辑器 ----
static bool previewing=false;
void seqPreviewStart(int s){
  if(previewing)return;
  scanList(); seBegin(16000);
  seSetVolume(0.4f);                  // 显式设主音量: 和弦+多轨叠加不顶爆限幅器(防"炸"), 也不受别 app 残留影响
  seSetCrush(0); seSetBend(1.0f);     // 试听前重置脏音/磁带停, 避免带上编辑器残留状态
  slot=s; loadSong();                 // 读入存档(含 seSetTone / 采样轨 / 乐句)
  curStep=0; playing=true; stepStart=millis(); eqn=0; trigStep(0);
  previewing=true;
}
void seqPreviewStop(){
  if(!previewing)return;
  seAllOff(); seEnd(); previewing=false; playing=false;
  if(trackBuf[0]){free(trackBuf[0]);trackBuf[0]=NULL;} if(trackBuf[1]){free(trackBuf[1]);trackBuf[1]=NULL;}
  for(int r=0;r<8;r++){ if(laneBuf[r]){free(laneBuf[r]);laneBuf[r]=NULL;laneLen[r]=0;} }   // 释放采样垫
  freeSampleKit();   // 释放采样鼓组
  if(phBuf){free(phBuf);phBuf=NULL;phBufLen=0;}
}
void seqPreviewPump(){
  if(!previewing||!playing)return;
  uint32_t base=60000/bpm/4; uint32_t sm=(uint32_t)(base*((curStep%2==0)?(1+swing):(1-swing)));
  if(millis()-stepStart>=sm){ stepStart+=sm; curStep=(curStep+1)%loopLen; trigStep(curStep); }
  uint32_t now=millis(); for(int i=0;i<eqn;){ if((int32_t)(now-eq[i].due)>=0){ fireEcho(eq[i]); eq[i]=eq[--eqn]; } else i++; }
}
bool seqIsPreviewing(){ return previewing; }

void seqEnter(){
  BG=lcd.color565(233,227,214); INK=lcd.color565(74,70,62); GREY=lcd.color565(150,144,130);
  ACC=lcd.color565(218,108,40); PLAY=lcd.color565(188,108,80); CUR=lcd.color565(214,206,190);
  GRID=lcd.color565(196,189,174); SEL=lcd.color565(120,110,92);
  // 参考3(调灰, 方向反转): 低音深蓝 -> 中性 -> 高音橙
  const uint8_t pc[12][3]={{44,60,86},{58,82,112},{78,104,134},{102,128,158},{118,148,178},{190,172,124},{218,162,96},{224,148,80},{228,132,64},{228,114,54},{224,98,48},{216,86,46}};
  for(int i=0;i<12;i++) PCOL[i]=lcd.color565(pc[i][0],pc[i][1],pc[i][2]);
  int melA[16]={72,-1,76,-1,67,-1,72,-1,69,-1,74,-1,67,-1,64,-1};
  int melB[16]={48,-1,-1,-1,50,-1,-1,-1,45,-1,-1,-1,43,-1,-1,-1};
  for(int s=0;s<STEPS;s++){ stepClear(0,s); stepClear(1,s); prob[0][s]=prob[1][s]=100; phTrig[s]=false;
    if(s<16){ if(melA[s]>=0)stepAdd(0,s,melA[s]); if(melB[s]>=0)stepAdd(1,s,melB[s]);
      uint8_t b=0; if(s%8==0)b|=1; if(s%8==4)b|=2; if(s%2==0)b|=4; drum[s]=b; } else drum[s]=0; }   // 老版律动: 底鼓 0/8
  loopLen=16; curStep=0; editStep=0; playing=false; activeTrack=0; trackSrc[0]=0; trackSrc[1]=3; kit=0; swing=0;
  for(int r=0;r<8;r++)laneSamp[r]=-1; freeLanes(); kitLane=0;   // 复位采样垫
  for(int t=0;t<2;t++)for(int s=0;s<STEPS;s++)noteLen[t][s]=1;   // 音符长度复位为 1 格
  for(int t=0;t<3;t++)for(int s=0;s<STEPS;s++)accent[t][s]=0;    // 力度复位为普通
  for(int t=0;t<2;t++)for(int i=0;i<5;i++)fx[t][i]=false; eqn=0; helpOn=false;
  // 橙蓝家族(与旋律/鼓统一): DLY蓝 OCT橙 SUB深橙 5TH琥珀 CHO浅蓝
  FXC[0]=lcd.color565(98,116,146); FXC[1]=lcd.color565(214,116,54); FXC[2]=lcd.color565(192,94,38); FXC[3]=lcd.color565(228,150,82); FXC[4]=lcd.color565(118,148,178);
  trackVol[0]=trackVol[1]=0.85f; trackVol[2]=1.0f; trackVol[3]=1.0f; toneV=4; crushV=0; seSetCrush(0); tapeActive=false; seSetBend(1.0f); arpRate=0; arpIdx=0; revPlay=false; for(int i=0;i<STEPS;i++){phStepFx[i]=0;phStepLen[i]=0;phStepOff[i]=0;phStepPit[i]=0;slideStep[0][i]=0;slideStep[1][i]=0;} lastNote[0]=lastNote[1]=-1;
  if(trackBuf[0]){free(trackBuf[0]);trackBuf[0]=NULL;} if(trackBuf[1]){free(trackBuf[1]);trackBuf[1]=NULL;}
  if(phBuf){free(phBuf);phBuf=NULL;phBufLen=0;} phraseSrc=-1; phrasePath[0]=0; phTotal=0; phStart=0; phSeg=0;
  for(int s=0;s<STEPS;s++) phTrig[s]=false;
  scanList();
  spr.setColorDepth(16); if(!spr.getBuffer()) spr.createSprite(240,135);
  seBegin(16000); seSetVolume(0.4f); seSetTone(toneV);   // 显式主音量: 防和弦/多轨叠加顶爆("炸")
  if(fxBuffer){free(fxBuffer);fxBuffer=NULL;} fxBuffer=(int16_t*)malloc(FX_MAX*2); if(fxBuffer)memset(fxBuffer,0,FX_MAX*2); spaceMode=0;   // 分配空间效果缓冲
  if(pendingLoad>=0){ slot=pendingLoad; loadSong(); pendingLoad=-1; }   // 从SONGS库进入时直接载入
  draw();
}
void seqExit(){ seAllOff(); seEnd(); if(spr.getBuffer()) spr.deleteSprite();
  if(trackBuf[0]){free(trackBuf[0]);trackBuf[0]=NULL;} if(trackBuf[1]){free(trackBuf[1]);trackBuf[1]=NULL;}
  for(int r=0;r<8;r++){ if(laneBuf[r]){free(laneBuf[r]);laneBuf[r]=NULL;laneLen[r]=0;} }   // 释放采样垫(保录音机内存)
  freeSampleKit();   // 释放采样鼓组(保录音机内存)
  seFxClear(); if(fxBuffer){free(fxBuffer);fxBuffer=NULL;}   // 释放空间效果缓冲
  if(phBuf){free(phBuf);phBuf=NULL;phBufLen=0;} }

void seqLoop(){
  bool redraw=false;
  bool fn=kb.isHeld("fn");
  if(kb.wasPressed("ctrl")){ helpOn=!helpOn; helpPage=0; if(helpOn)drawHelp(); else draw(); return; }
  if(helpOn){ if(kb.wasPressed("tab")) helpPage^=1; drawHelp(); return; }
  if(kb.wasPressed("enter")||kb.wasPressed("ok")){ if(tapeActive){tapeActive=false;seSetBend(1.0f);} playing=!playing; if(playing){stepStart=millis(); trigStep(curStep);} else seAllOff(); redraw=true; }
  if(kb.wasPressed("space")){ playing=false; if(tapeActive){tapeActive=false;seSetBend(1.0f);} seAllOff(); eqn=0; redraw=true; }   // 清空回声队列, 停止后不再触发
  if(kb.wasPressed("tab")){ activeTrack=(activeTrack+1)%4; redraw=true; }
  if(kb.wasPressed("opt") && activeTrack<2){          // opt=滑音, fn+opt=力度(普通/重音/弱音)
    if(fn){ uint8_t v=(accent[activeTrack][editStep]+1)%3; accent[activeTrack][editStep]=v; snprintf(msg,24,"ACCENT %s",v==0?"norm":v==1?"LOUD":"soft"); }
    else { slideStep[activeTrack][editStep]^=1; snprintf(msg,24,"SLIDE %s",slideStep[activeTrack][editStep]?"ON":"OFF"); }
    msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed("-")){          // - = 音符长度减(旋律); fn+- = 上一存档槽
    if(fn) slot=(slot+NSLOT-1)%NSLOT;
    else if(activeTrack<2){ noteLen[activeTrack][editStep]=lenDn(noteLen[activeTrack][editStep]); snprintf(msg,24,"LEN %d",noteLen[activeTrack][editStep]); msgUntil=millis()+700; }
    redraw=true; }
  if(kb.wasPressed("+")){          // + = 音符长度加(旋律) / 力度(鼓); fn++ = 下一存档槽
    if(fn) slot=(slot+1)%NSLOT;
    else if(activeTrack<2){ noteLen[activeTrack][editStep]=lenUp(noteLen[activeTrack][editStep]); snprintf(msg,24,"LEN %d",noteLen[activeTrack][editStep]); msgUntil=millis()+700; }
    else if(activeTrack==2){ uint8_t v=(accent[2][editStep]+1)%3; accent[2][editStep]=v; snprintf(msg,24,"ACCENT %s",v==0?"norm":v==1?"LOUD":"soft"); msgUntil=millis()+700; }
    redraw=true; }
  if(fn && kb.wasPressed("k")){ swing+=0.15f; if(swing>0.31f)swing=0; snprintf(msg,24,"SWING %d",(int)(swing*100)); msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed("f")){ crushV=(crushV+1)%4; seSetCrush(crushV); snprintf(msg,24,crushV?"CRUSH %d":"CRUSH off",crushV); msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed("a")){ arpRate=(arpRate==0)?2:(arpRate>=4?0:arpRate+1); snprintf(msg,24,arpRate?"ARP %d":"ARP off",arpRate); msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed("alt")){ spaceMode=(spaceMode+1)%3; applySpaceFx(); snprintf(msg,24,"SPACE %s", spaceMode==0?"off":spaceMode==1?"ECHO":"VERB"); msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed("l")){
    if(fn){ revPlay=!revPlay; snprintf(msg,24,revPlay?"SEQ <<REV":"SEQ FWD>>"); }   // 律动反向
    else { seAllOff(); delay(10); reverseBuf(trackBuf[0],trackLen[0]); reverseBuf(trackBuf[1],trackLen[1]); reverseBuf(phBuf,phBufLen); snprintf(msg,24,"REVERSE smpl"); }  // 先停声再原地反转
    msgUntil=millis()+700; redraw=true; }
  if(!fn && kb.wasPressed("k") && playing && !tapeActive){   // 磁带停: 拉长当前音 + 启动减速
    tapeActive=true; tapeT0=millis(); uint32_t g=900;
    for(int t=0;t<2;t++)for(int kk=0;kk<MAXN;kk++){ int n=pat[t][curStep][kk]; if(n>=0) trigVoice(t,n,g,1.0f); }
    snprintf(msg,24,"TAPE STOP"); msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed(",")){ if(fn&&activeTrack<2){ int p=prob[activeTrack][editStep]-25; if(p<0)p=0; prob[activeTrack][editStep]=p; snprintf(msg,24,"PROB %d",p); msgUntil=millis()+700; redraw=true; } else if(!fn){ editStep=(editStep+loopLen-1)%loopLen; redraw=true; } }
  if(kb.wasPressed(".")){ if(fn&&activeTrack<2){ int p=prob[activeTrack][editStep]+25; if(p>100)p=100; prob[activeTrack][editStep]=p; snprintf(msg,24,"PROB %d",p); msgUntil=millis()+700; redraw=true; } else if(!fn){ editStep=(editStep+1)%loopLen; redraw=true; } }
  if(kb.wasPressed("[")){ if(fn){ if(loopLen>1)loopLen--; if(editStep>=loopLen)editStep=loopLen-1; snprintf(msg,24,"LEN %d",loopLen); msgUntil=millis()+600; } else { bpm-=4; if(bpm<60)bpm=60; if(spaceMode==1)applySpaceFx(); } redraw=true; }
  if(kb.wasPressed("]")){ if(fn){ if(loopLen<STEPS)loopLen++; snprintf(msg,24,"LEN %d",loopLen); msgUntil=millis()+600; } else { bpm+=4; if(bpm>200)bpm=200; if(spaceMode==1)applySpaceFx(); } redraw=true; }
  if(kb.wasPressed("del")){ if(fn){ for(int s=0;s<STEPS;s++){stepClear(0,s);stepClear(1,s);drum[s]=0;prob[0][s]=prob[1][s]=100;slideStep[0][s]=slideStep[1][s]=0;noteLen[0][s]=noteLen[1][s]=1;accent[0][s]=accent[1][s]=accent[2][s]=0;} snprintf(msg,24,"CLEAR ALL"); msgUntil=millis()+700; } else { if(activeTrack<2){stepClear(activeTrack,editStep);slideStep[activeTrack][editStep]=0;noteLen[activeTrack][editStep]=1;accent[activeTrack][editStep]=0;} else {drum[editStep]=0;accent[2][editStep]=0;} } redraw=true; }
  for(int i=0;i<4;i++){ char d[2]={(char)('1'+i),0}; if(kb.wasPressed(d)&&activeTrack<2){ trackSrc[activeTrack]=i; redraw=true; } }  // 波形
  if(kb.wasPressed("0")&&activeTrack<2){            // 切录音当音色 (0=下一个, fn+0=上一个, 都回卷)
    if(listN==0){ snprintf(msg,24,"NO REC/WAV"); }
    else { int curIdx=(trackSrc[activeTrack]<4)?-1:(trackSrc[activeTrack]-4);
      int ni = fn ? ((curIdx<0)?listN-1:(curIdx-1+listN)%listN) : ((curIdx<0)?0:(curIdx+1)%listN);
      setTrackSample(activeTrack,ni); char nm[12]; srcName(activeTrack,nm); snprintf(msg,24,"%c:%s",'A'+activeTrack,nm); }
    msgUntil=millis()+800; redraw=true; }
  if(kb.wasPressed("0")&&activeTrack==3){           // 乐句轨选录音 (0=下一个, fn+0=上一个)
    if(listN==0){ snprintf(msg,24,"NO REC/WAV"); }
    else { int ni = fn ? ((phraseSrc<0)?listN-1:(phraseSrc-1+listN)%listN) : ((phraseSrc<0)?0:(phraseSrc+1)%listN);
      phPick(ni); snprintf(msg,24,"PHR:%s",listName[phraseSrc]); }
    msgUntil=millis()+800; redraw=true; }
  if(activeTrack<2){ int ft=activeTrack;
    for(int i=0;i<4;i++){ char d[2]={(char)('5'+i),0}; if(kb.wasPressed(d)){ fx[ft][i]=!fx[ft][i]; snprintf(msg,24,"%c %s %s",'A'+ft,FXN[i],fx[ft][i]?"ON":"OFF"); msgUntil=millis()+700; redraw=true; } }
    if(kb.wasPressed("9")){ fx[ft][4]=!fx[ft][4]; snprintf(msg,24,"%c CHO %s",'A'+ft,fx[ft][4]?"ON":"OFF"); msgUntil=millis()+700; redraw=true; } }   // 9=CHO
  if(kb.wasPressed("1")&&activeTrack==2){ kit=(kit+1)%NKIT; selectKit(kit); snprintf(msg,24,"KIT %s",KITS[kit].name); msgUntil=millis()+700; redraw=true; }   // 鼓组循环; 切到采样组即加载
  if(kb.wasPressed("0")&&activeTrack==2){          // 给选中 lane 切音源: 合成鼓<->各录音 (0=下一个 fn+0=上一个)
    if(listN==0){ snprintf(msg,24,"NO REC/WAV"); }
    else { int cur=laneSamp[kitLane], nc;
      if(fn){ nc=(cur<=-1)?listN-1:cur-1; } else { nc=cur+1; if(nc>=listN)nc=-1; }
      laneSamp[kitLane]=nc;
      if(nc>=0) loadLane(kitLane,nc);
      else { if(laneBuf[kitLane]){ seAllOff(); delay(5); free(laneBuf[kitLane]); laneBuf[kitLane]=NULL; laneLen[kitLane]=0; } }
      if(nc<0) snprintf(msg,24,"L%d: DRUM",kitLane+1); else snprintf(msg,24,"L%d:%s",kitLane+1,listName[nc]); }
    msgUntil=millis()+900; redraw=true; }
  if(kb.wasPressed("/")){ saveSong(); redraw=true; }
  if(kb.wasPressed("\\")){ loadSong(); redraw=true; }
  const char* TVN[4]={"A","B","DR","PHR"};
  if(kb.wasPressed(";")){
    if(fn&&activeTrack<2){ toneV-=2; if(toneV<2)toneV=2; seSetTone(toneV); snprintf(msg,24,"TONE %d",toneV);}
    else { trackVol[activeTrack]-=0.1f; if(trackVol[activeTrack]<0)trackVol[activeTrack]=0; snprintf(msg,24,"%s VOL %d",TVN[activeTrack],(int)(trackVol[activeTrack]*100)); }
    msgUntil=millis()+700; redraw=true; }
  if(kb.wasPressed("'")){
    if(fn&&activeTrack<2){ toneV+=2; if(toneV>14)toneV=14; seSetTone(toneV); snprintf(msg,24,"TONE %d",toneV);}
    else { trackVol[activeTrack]+=0.1f; if(trackVol[activeTrack]>1.2f)trackVol[activeTrack]=1.2f; snprintf(msg,24,"%s VOL %d",TVN[activeTrack],(int)(trackVol[activeTrack]*100)); }
    msgUntil=millis()+700; redraw=true; }
  if(activeTrack==3 && !fn){    // 乐句轨: 1-4 + 9(REV) 当前格效果(叠加) · 7/8 当前格播放长度
    for(int i=0;i<4;i++){ char d[2]={(char)('1'+i),0}; if(kb.wasPressed(d)){ phStepFx[editStep]^=(1<<i); bool on=phStepFx[editStep]&(1<<i); snprintf(msg,24,"PHR %s %s",PHFXN[i],on?"ON":"OFF"); msgUntil=millis()+700; redraw=true; } }
    if(kb.wasPressed("9")){ phStepFx[editStep]^=16; bool on=phStepFx[editStep]&16; snprintf(msg,24,"PHR REV %s",on?"ON":"OFF"); msgUntil=millis()+700; redraw=true; }
    uint8_t segU=(uint8_t)(phSeg*20/ (phRate?phRate:16000));   // 选段总长(×50ms档)
    if(kb.wasPressed("5")){ if(phStepOff[editStep]>0)phStepOff[editStep]--; snprintf(msg,24,"OFFSET %.2fs",phStepOff[editStep]*0.05f); msgUntil=millis()+700; redraw=true; }
    if(kb.wasPressed("6")){ if(phStepOff[editStep]<segU)phStepOff[editStep]++; snprintf(msg,24,"OFFSET %.2fs",phStepOff[editStep]*0.05f); msgUntil=millis()+700; redraw=true; }
    if(kb.wasPressed("7")){ if(phStepLen[editStep]>0)phStepLen[editStep]--; if(phStepLen[editStep]==0)snprintf(msg,24,"LEN full"); else snprintf(msg,24,"LEN %.2fs",phStepLen[editStep]*0.05f); msgUntil=millis()+700; redraw=true; }
    if(kb.wasPressed("8")){ if(phStepLen[editStep]<30)phStepLen[editStep]++; snprintf(msg,24,"LEN %.2fs",phStepLen[editStep]*0.05f); msgUntil=millis()+700; redraw=true; }
  }
  if(activeTrack==3 && fn && phTotal>0){    // 乐句轨全局裁剪: fn+5/6 起点  fn+7/8 长度
    uint32_t st=phTotal/100; if(st<1)st=1; bool ch=false;
    if(kb.wasPressed("5")){ phStart=(phStart>st)?phStart-st:0; ch=true; }
    if(kb.wasPressed("6")){ phStart+=st; if(phStart+phSeg>phTotal)phStart=(phTotal>phSeg)?phTotal-phSeg:0; ch=true; }
    if(kb.wasPressed("7")){ if(phSeg>phRate/8)phSeg-=phRate/8; ch=true; }
    if(kb.wasPressed("8")){ phSeg+=phRate/8; if(phSeg>PH_SEG_MAX)phSeg=PH_SEG_MAX; ch=true; }
    if(ch){ if(phStart+phSeg>phTotal)phSeg=phTotal-phStart; phLoadSeg(); snprintf(msg,24,"SEG %.2fs",phSeg/(float)phRate); msgUntil=millis()+600; redraw=true; }
  }
  if(activeTrack==3 && fn){    // 乐句轨: fn+9 碎拍STUT · fn+1/2 当前格变调 -/+
    if(kb.wasPressed("9")){ phStepFx[editStep]^=32; bool on=phStepFx[editStep]&32; snprintf(msg,24,"PHR STUT %s",on?"ON":"OFF"); msgUntil=millis()+700; redraw=true; }
    if(kb.wasPressed("1")){ if(phStepPit[editStep]>-12)phStepPit[editStep]--; snprintf(msg,24,"PIT %d",phStepPit[editStep]); msgUntil=millis()+700; redraw=true; }
    if(kb.wasPressed("2")){ if(phStepPit[editStep]<12)phStepPit[editStep]++; snprintf(msg,24,"PIT %d",phStepPit[editStep]); msgUntil=millis()+700; redraw=true; }
  }

  static const char* NK[]={"z","s","x","d","c","v","g","b","h","n","j","m","q","w","e","r","t","y","u","i","o","p"};
  for(auto k:NK){ if(kb.wasPressed(k)){
    if(fn && activeTrack<2){
      int root=-1; bool minor=false; const char* nm="";
      if(!strcmp(k,"z")){root=60;nm="C";} else if(!strcmp(k,"x")){root=62;minor=true;nm="Dm";} else if(!strcmp(k,"c")){root=64;minor=true;nm="Em";}
      else if(!strcmp(k,"v")){root=65;nm="F";} else if(!strcmp(k,"b")){root=67;nm="G";} else if(!strcmp(k,"n")){root=69;minor=true;nm="Am";}
      if(root>=0){ addChord(root,minor); snprintf(msg,24,"CHORD %s",nm); msgUntil=millis()+600; redraw=true; } }
    else if(activeTrack<2){ int n=midiOfKey(k); if(n>=0){ previewT(activeTrack,n);   // 按下必响(像钢琴)
        if(stepHas(activeTrack,editStep,n))stepDel(activeTrack,editStep,n); else stepAdd(activeTrack,editStep,n); redraw=true; } }
    else if(activeTrack==2){ int n=midiOfKey(k); int bit=-1;
      if(n==60)bit=0; else if(n==62)bit=1; else if(n==64)bit=2; else if(n==65)bit=3;
      else if(n==67)bit=4; else if(n==69)bit=5; else if(n==71)bit=6; else if(n==72)bit=7;
      if(bit>=0){ kitLane=bit; trigDrumLane(bit); drum[editStep]^=(1<<bit); redraw=true; } }   // 按下必响; 记住选中 lane(供 0 选录音)
    else { if(!strcmp(k,"z")){ playPhrase(phStepFx[editStep], (uint32_t)(phStepOff[editStep]*0.05f*phRate), phStepLen[editStep]?phStepLen[editStep]*50u:0, phStepPit[editStep]); phTrig[editStep]=!phTrig[editStep]; redraw=true; } } } }

  if(tapeActive){ float f=1.0f-(millis()-tapeT0)/750.0f;   // 磁带停减速降调
    if(f<=0.02f){ seSetBend(1.0f); seAllOff(); playing=false; tapeActive=false; } else seSetBend(f); redraw=true; }
  if(playing && !tapeActive){ uint32_t base=60000/bpm/4; uint32_t sm=(uint32_t)(base*((curStep%2==0)?(1+swing):(1-swing)));
    if(millis()-stepStart>=sm){ stepStart+=sm; curStep=revPlay?(curStep+loopLen-1)%loopLen:(curStep+1)%loopLen; trigStep(curStep); redraw=true; } }
  if(playing && !tapeActive && arpRate>0){   // 琶音: 把当前格的和弦按子拍依次向上弹
    uint32_t stepMs=60000/bpm/4; uint32_t ai=stepMs/arpRate; if(ai<25)ai=25;
    if(millis()-arpClock>=ai){ arpClock=millis(); uint32_t g=ai*0.9f; if(g<20)g=20;
      for(int t=0;t<2;t++){ int notes[MAXN],cnt=0; for(int k=0;k<MAXN;k++){int n=pat[t][curStep][k]; if(n>=0)notes[cnt++]=n;}
        if(cnt>0){ for(int x=0;x<cnt;x++)for(int y=x+1;y<cnt;y++)if(notes[y]<notes[x]){int tp=notes[x];notes[x]=notes[y];notes[y]=tp;}
          noteWithFx(t,notes[arpIdx%cnt],g,ai); } }
      arpIdx++; } }
  { uint32_t now=millis(); for(int i=0;i<eqn;){ if((int32_t)(now-eq[i].due)>=0){ fireEcho(eq[i]); eq[i]=eq[--eqn]; } else i++; } }  // 回声
  if(millis()<msgUntil) redraw=true;
  if(redraw) draw();
}
