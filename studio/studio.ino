// =====================================================================
//  STUDIO — 无 M5Unified 的二合一固件: 采样器 + 录音机 (同一固件!)
//  框架: LovyanGFX(屏) + I2C键盘(0x34) + ES8311(老式i2s 录+放) + SD
//  录音用 I2S0(RX), 播放用 I2S1(TX); 都不依赖 M5Unified -> 麦克风能录!
// =====================================================================
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "Display.h"
#include "CardputerKeyboard.h"
#include "ES8311Audio.h"
#include "synth_engine.h"
#include "seq.h"

LGFX lcd;
CardputerKeyboard kb;
ES8311Audio audio;
SPIClass sdSPI(HSPI);            // SD 用独立 SPI3, 避开屏幕的 SPI2

#define SD_SCK 40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS  12
#define PIN_SCLK 41
#define PIN_LRCK 43
#define PIN_DIN  46
static const uint32_t SR=16000;
static const int REC_MAX_SEC=20;
char recMsg[28]="";

uint16_t BG,INK,GREY,ACC,HL,RED,SEL,CARD,LINE;
enum St{ MENU, SAMPLER, RECORDER, LIBRARY, SYNTH, SONGS } st=MENU;
int libSel=0, libTop=0; bool renaming=false, confirmDel=false; char nameBuf[20]="";
bool sdOK=false;
// ---- 乐曲库 ----
#define NSLOT 16
int songSel=0, songTop=0; bool songFilled[NSLOT]; int previewSlot=-1;
char songNames[NSLOT][16];

// ---- 样本库 ----
#define MAXS 99
char  sName[MAXS][24];
char  sPath[MAXS][72];
int   sN=0, sCur=0, sPage=0;
int16_t* sBuf=NULL; uint32_t sLen=0, sRate=SR; int sLoaded=-1;
bool trimMode=false; uint32_t trimA=0, trimB=0;   // 录音修剪: 选段 [A,B)
uint32_t smpPeak=1; bool trimNorm=false, trimRev=false; int trimLofi=0;   // 显示峰值; 保存时归一化(默认关,避免变响爆音)/倒放/lo-fi(0-3)
int trimDrive=0; bool trimFade=false;   // drive 暖化失真(0-3) / fade 淡入淡出(去切片咔哒)
char loadDiag[48]="";          // 最近一次载入的诊断(格式/原因)

// ---- 录音缓冲 ----
int16_t* rBuf=NULL; uint32_t rCap=0, rLen=0; bool recording=false; uint32_t recT0=0; int32_t recPeak=0; int32_t recMaxPk=0;
File recFile; uint32_t recFrames=0; char recPath[40]="";   // 边录边写卡
int32_t pkL=0,pkR=0; char diag[40]="";

const char* NOTEKEYS[]={"z","s","x","d","c","v","g","b","h","n","j","m","q","w","e","r","t","y","u","i","o","p"};
int midiOfKey(const char* k){
  static const char* names[]={"z","s","x","d","c","v","g","b","h","n","j","m","q","w","e","r","t","y","u","i","o","p"};
  static const int mid[]={60,61,62,63,64,65,66,67,68,69,70,71,72,74,76,77,79,81,83,84,86,88};
  for(int i=0;i<22;i++) if(strcmp(k,names[i])==0) return mid[i];
  return -1;
}

// ---------- WAV ----------
bool loadWav(const char* path){
  if(sBuf){ free(sBuf); sBuf=NULL; }
  File f=SD.open(path,FILE_READ); if(!f){ strcpy(loadDiag,"cannot open file"); return false; }
  uint8_t h[12]; if(f.read(h,12)!=12||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)){ strcpy(loadDiag,"not a WAV file"); f.close();return false;}
  uint16_t ch=1,bits=16; uint32_t rate=SR,dOff=0,dSz=0;
  while(f.available()>=8){ uint8_t c[8]; if(f.read(c,8)!=8)break;
    uint32_t cs=c[4]|(c[5]<<8)|(c[6]<<16)|((uint32_t)c[7]<<24);
    if(!memcmp(c,"fmt ",4)){ uint8_t fm[16]; uint32_t tr=cs<16?cs:16; f.read(fm,tr);
      ch=fm[2]|(fm[3]<<8); rate=fm[4]|(fm[5]<<8)|(fm[6]<<16)|((uint32_t)fm[7]<<24); bits=fm[14]|(fm[15]<<8);
      if(cs>16)f.seek(f.position()+(cs-16)); if(cs&1)f.seek(f.position()+1);
    } else if(!memcmp(c,"data",4)){ dOff=f.position(); dSz=cs; break; }
    else f.seek(f.position()+cs+(cs&1)); }
  if(dOff==0||dSz==0||bits!=16||ch<1||ch>2){ snprintf(loadDiag,48,"need 16-bit, this is %d-bit %dch",bits,ch); f.close();return false;}
  uint32_t fsz=f.size(); if(dOff+dSz>fsz)dSz=fsz-dOff;
  uint32_t frames=dSz/(2*ch); if(frames==0){ strcpy(loadDiag,"empty (0 samples)"); f.close();return false;}
  sBuf=(int16_t*)ps_malloc(frames*2); if(!sBuf)sBuf=(int16_t*)malloc(frames*2);
  if(!sBuf){ snprintf(loadDiag,48,"OUT OF MEMORY (%lu KB)",(unsigned long)(frames*2/1024)); f.close();return false;}
  f.seek(dOff); static int16_t tmp[512*2]; uint32_t got=0;
  while(got<frames){ uint32_t want=frames-got; if(want>512)want=512;
    int rd=f.read((uint8_t*)tmp,want*2*ch); uint32_t fr=rd/(2*ch); if(fr==0)break;
    for(uint32_t i=0;i<fr;i++) sBuf[got+i]= ch==1? tmp[i] : (int16_t)((tmp[i*2]+tmp[i*2+1])/2);
    got+=fr; }
  f.close(); sLen=got; sRate=rate; snprintf(loadDiag,48,"ok %d-bit %dch %luHz",bits,ch,(unsigned long)rate); return true;
}
void w16(File&f,uint16_t v){f.write((uint8_t)(v&0xff));f.write((uint8_t)(v>>8));}
void w32(File&f,uint32_t v){for(int i=0;i<4;i++)f.write((uint8_t)((v>>(8*i))&0xff));}
void saveWav(const char* path,int16_t* buf,uint32_t n){
  File f=SD.open(path,FILE_WRITE); if(!f)return;
  uint32_t d=n*2; f.write((const uint8_t*)"RIFF",4); w32(f,36+d); f.write((const uint8_t*)"WAVE",4);
  f.write((const uint8_t*)"fmt ",4); w32(f,16); w16(f,1); w16(f,1); w32(f,SR); w32(f,SR*2); w16(f,2); w16(f,16);
  f.write((const uint8_t*)"data",4); w32(f,d);
  uint32_t i=0; while(i<n){ uint32_t c=n-i; if(c>512)c=512; f.write((uint8_t*)(buf+i),c*2); i+=c; }
  f.close();
}

// ---------- SD 扫描 ----------
void scanDir(const char* dir){
  File root=SD.open(dir); if(!root||!root.isDirectory()){if(root)root.close();return;}
  File e;
  while(sN<MAXS && (e=root.openNextFile())){
    const char* n=e.name();
    if(!e.isDirectory()){ const char* d=strrchr(n,'.');
      if(d&&(!strcmp(d,".wav")||!strcmp(d,".WAV"))&&n[0]!='.'){
        if(n[0]=='/') snprintf(sPath[sN],72,"%s",n); else snprintf(sPath[sN],72,"%s/%s",dir,n);
        strncpy(sName[sN],strrchr(sPath[sN],'/')+1,23); sName[sN][23]=0;
        char* dot=strrchr(sName[sN],'.'); if(dot)*dot=0; sN++; } }
    e.close(); }
  root.close();
}
void sortSamples(){   // 按名字字母排序(不分大小写), 便于查找
  for(int i=1;i<sN;i++){ char tn[24]; char tp[72]; strcpy(tn,sName[i]); strcpy(tp,sPath[i]); int j=i-1;
    while(j>=0 && strcasecmp(sName[j],tn)>0){ strcpy(sName[j+1],sName[j]); strcpy(sPath[j+1],sPath[j]); j--; }
    strcpy(sName[j+1],tn); strcpy(sPath[j+1],tp); }
}
void rescan(){ sN=0; scanDir("/samples"); scanDir("/VoiceMemos"); if(sN==0)scanDir("/"); sortSamples(); if(sCur>=sN)sCur=0; }

// ---------- 录音 (I2S0 RX -> 边录边写 TF 卡, 几乎不占内存) ----------
void recStart(){
  if(!sdOK){ strcpy(recMsg,"no SD"); return; }
  if(sBuf){ free(sBuf); sBuf=NULL; sLoaded=-1; }      // 腾内存
  if(rBuf){ free(rBuf); rBuf=NULL; }                  // 不再用大缓冲
  recMsg[0]=0;
  SD.mkdir("/samples");
  for(int i=1;i<999;i++){ snprintf(recPath,40,"/samples/REC%d.wav",i); if(!SD.exists(recPath))break; }
  recFile=SD.open(recPath,FILE_WRITE);
  if(!recFile){ strcpy(recMsg,"SD open fail"); return; }
  // 写占位 WAV 头(数据大小先填0, 停止时回填)
  recFile.write((const uint8_t*)"RIFF",4); w32(recFile,36); recFile.write((const uint8_t*)"WAVE",4);
  recFile.write((const uint8_t*)"fmt ",4); w32(recFile,16); w16(recFile,1); w16(recFile,1); w32(recFile,SR); w32(recFile,SR*2); w16(recFile,2); w16(recFile,16);
  recFile.write((const uint8_t*)"data",4); w32(recFile,0);
  bool codec=audio.testConnection();
  i2s_driver_uninstall(I2S_NUM_1);   // 防御: 确保合成器/SHAKE/LOOP 的 TX 已卸, 否则麦克风会哑
  audio.enableMicForStream();
  i2s_config_t c={}; c.mode=(i2s_mode_t)(I2S_MODE_MASTER|I2S_MODE_RX);
  c.sample_rate=SR; c.bits_per_sample=I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format=I2S_CHANNEL_FMT_RIGHT_LEFT; c.communication_format=I2S_COMM_FORMAT_STAND_I2S;
  c.intr_alloc_flags=ESP_INTR_FLAG_LEVEL1; c.dma_buf_count=8; c.dma_buf_len=128;
  c.use_apll=false; c.tx_desc_auto_clear=true; c.mclk_multiple=I2S_MCLK_MULTIPLE_256; c.bits_per_chan=I2S_BITS_PER_CHAN_16BIT;
  esp_err_t ie=i2s_driver_install(I2S_NUM_0,&c,0,NULL);
  if(ie!=ESP_OK){ snprintf(diag,sizeof(diag),"codec=%s i2s=ERR",codec?"OK":"NO"); strcpy(recMsg,"i2s install fail");
    if(recFile)recFile.close(); return; }   // 安装失败: 不进入录音状态
  i2s_pin_config_t p={}; p.mck_io_num=I2S_PIN_NO_CHANGE; p.bck_io_num=PIN_SCLK; p.ws_io_num=PIN_LRCK; p.data_out_num=I2S_PIN_NO_CHANGE; p.data_in_num=PIN_DIN;
  i2s_set_pin(I2S_NUM_0,&p); i2s_zero_dma_buffer(I2S_NUM_0);
  // 丢弃几个缓冲并测峰值(看麦克风有没有数据)
  static int16_t dsc[256]; size_t br; int32_t pk=0;
  for(int i=0;i<8;i++){ i2s_read(I2S_NUM_0,dsc,sizeof(dsc),&br,80/portTICK_PERIOD_MS);
    int n=br/2; for(int k=0;k<n;k++){ int v=dsc[k]; if(v<0)v=-v; if(v>pk)pk=v; } }
  snprintf(diag,sizeof(diag),"codec=%s i2s=OK pk=%ld", codec?"OK":"NO", (long)pk);
  recFrames=0; recPeak=0; recMaxPk=0; recording=true; recT0=millis();
}
#define REC_GAIN 1        // 软件增益(改固定 codec 增益后不再需要软件放大; 先设 1, 按 mic peak 数字再调)
void recPump(){
  static int16_t stx[256*2]; static int16_t mono[256]; size_t br=0;
  uint32_t maxFrames=(uint32_t)REC_MAX_SEC*SR;
  i2s_read(I2S_NUM_0,stx,sizeof(stx),&br,20/portTICK_PERIOD_MS);
  int got=br/4; int32_t pl=0,pr=0; int mn=0;
  for(int i=0;i<got && recFrames<maxFrames;i++){ int16_t L=stx[i*2], R=stx[i*2+1];
    int32_t al=L<0?-L:L, ar=R<0?-R:R; if(al>pl)pl=al; if(ar>pr)pr=ar;
    int32_t s=(ar>=al)?R:L; s*=REC_GAIN;                 // 软件增益
    if(s>32767)s=32767; else if(s<-32768)s=-32768;       // 削波保护
    mono[mn++]=(int16_t)s; recFrames++; }                // 较响的声道
  if(mn>0 && recFile) recFile.write((uint8_t*)mono, mn*2);
  pkL=pl; pkR=pr; recPeak=pl>pr?pl:pr; if(recPeak>recMaxPk)recMaxPk=recPeak;
  if(recFrames>=maxFrames) recStop();
}
void recStop(){
  recording=false; i2s_driver_uninstall(I2S_NUM_0);
  if(recFile){ uint32_t dataSz=recFrames*2;
    recFile.flush(); recFile.seek(4); w32(recFile,36+dataSz); recFile.seek(40); w32(recFile,dataSz);
    recFile.close(); }
  rescan();
}

// ---------- 绘制 ----------
// 统一页眉: 标题 + 分隔线 + 右上角小提示
void hdr(const char* title,const char* hint){
  lcd.fillScreen(BG);
  lcd.setTextColor(ACC); lcd.setTextSize(2); lcd.setCursor(8,6); lcd.print(title);
  if(hint){ lcd.setTextColor(GREY); lcd.setTextSize(1); int w=strlen(hint)*6; lcd.setCursor(236-w,8); lcd.print(hint); }
  lcd.drawFastHLine(0,25,240,LINE);
}
// 统一底部提示条
void footer(const char* hint){
  lcd.drawFastHLine(0,116,240,LINE);
  lcd.setTextColor(GREY); lcd.setTextSize(1); lcd.setCursor(8,122); lcd.print(hint);
}
void drawMenu(){
  lcd.fillScreen(BG);
  // 标题块
  lcd.setTextColor(ACC); lcd.setTextSize(3); lcd.setCursor(10,8); lcd.print("STUDIO");
  lcd.setTextColor(GREY); lcd.setTextSize(1); lcd.setCursor(12,34); lcd.print("music machine");
  lcd.drawFastHLine(0,46,240,LINE);
  const char* items[5]={"SYNTH","SAMPLER","RECORDER","SONGS","LIBRARY"};
  for(int i=0;i<5;i++){ int col=i%2, row=i/2; int x=8+col*118, y=49+row*16;
    lcd.fillRoundRect(x,y,112,15,3,CARD);
    lcd.fillRoundRect(x+2,y+2,12,11,2,ACC); lcd.setTextColor(BG); lcd.setTextSize(1); lcd.setCursor(x+5,y+4); lcd.printf("%d",i+1);
    lcd.setTextColor(INK); lcd.setCursor(x+21,y+4); lcd.print(items[i]); }
  char hint[40]; snprintf(hint,40, sdOK?"SD ok - %d wav    ` = back":"NO SD CARD", sN);
  footer(hint);
}
void drawSampler(){
  hdr("SAMPLER","` menu");
  if(sN==0){ lcd.setTextColor(HL); lcd.setTextSize(2); lcd.setCursor(20,56); lcd.print("no wav on SD"); footer("put 16-bit WAV in /samples"); return; }
  int pages=(sN+7)/8; if(pages<1)pages=1; if(sPage>=pages)sPage=pages-1;
  int base=sPage*8, shown=sN-base; if(shown>8)shown=8;
  // 大名字卡片
  lcd.fillRoundRect(6,32,228,32,4,CARD);
  lcd.setTextColor(sLoaded==sCur?INK:RED); lcd.setTextSize(2); lcd.setCursor(14,40); lcd.print(sName[sCur]);
  lcd.setTextSize(1); lcd.setCursor(8,70);
  if(sLoaded!=sCur){ lcd.setTextColor(RED); lcd.print(loadDiag[0]?loadDiag:"LOAD FAIL"); }
  else { lcd.setTextColor(GREY); lcd.printf("sample %d/%d    page %d/%d", sCur+1, sN, sPage+1, pages); }
  int x0=8,gap=3,by=88,bw=(224-7*gap)/8;
  for(int i=0;i<shown;i++){ int x=x0+i*(bw+gap); int idx=base+i;
    if(idx==sCur){ lcd.fillRoundRect(x,by,bw,18,3,ACC); lcd.setTextColor(BG);}
    else{ lcd.fillRoundRect(x,by,bw,18,3,CARD); lcd.setTextColor(GREY);}
    lcd.setTextSize(1); lcd.setCursor(x+bw/2-3,by+6); lcd.printf("%d",i+1); }
  footer("keys=play  ,/.=prev/next  1-8 pick  tab page  / = trim");
}
void drawSamplerTrim(){
  hdr("TRIM","/ back");
  lcd.setTextColor(INK); lcd.setTextSize(1); lcd.setCursor(8,30); lcd.print(sName[sCur]);
  int X=8,Y=46,W=224,H=40; lcd.drawRect(X,Y,W,H,GREY);
  uint32_t pkRef = smpPeak<200?200:smpPeak;     // 按本段峰值放大显示, 小录音也看得见
  if(sBuf&&sLen){
    for(int x=0;x<W;x++){ uint32_t i0=(uint64_t)x*sLen/W, i1=(uint64_t)(x+1)*sLen/W; if(i1<=i0)i1=i0+1; if(i1>sLen)i1=sLen;
      int pk=0; for(uint32_t i=i0;i<i1;i+=4){ int v=sBuf[i]; if(v<0)v=-v; if(v>pk)pk=v; }
      int h=(int)((uint64_t)pk*(H/2)/pkRef); if(h>H/2)h=H/2; bool in=(i0>=trimA && i0<=trimB);
      lcd.drawFastVLine(X+x, Y+H/2-h, h*2+1, in?ACC:lcd.color565(70,76,90)); }
    int ax=X+(int)((uint64_t)trimA*W/sLen), bx=X+(int)((uint64_t)trimB*W/sLen);
    lcd.drawFastVLine(ax,Y-2,H+4,HL); lcd.drawFastVLine(bx,Y-2,H+4,RED);
  }
  float dur=(trimB-trimA)/(float)(sRate?sRate:SR);
  lcd.setTextColor(GREY); lcd.setTextSize(1); lcd.setCursor(8,92); lcd.printf("%.2fs norm:%s rev:%s lofi:%d drv:%d fade:%s", dur, trimNorm?"on":"-", trimRev?"on":"-", trimLofi, trimDrive, trimFade?"on":"-");
  footer(",.;'=cut n=norm r=rev []=lofi d=drive f=fade ok=save");
}
// 处理一遍选段(倒放 + lo-fi), f!=NULL 时按 gain 写文件, 否则只测峰值
static void trimProcess(File* f, float gain, int* outPeak, int16_t* outBuf){
  uint32_t n=trimB-trimA; int pk=1;
  float a = trimLofi==1?0.45f : trimLofi==2?0.30f : trimLofi==3?0.18f : 1.0f;   // 低通系数
  int bits = trimLofi==2?8 : trimLofi==3?6 : 16; int holdN = trimLofi==3?2:1;   // bitcrush
  float lp=0; int hold=0; float hv=0;
  uint32_t fl=0; if(trimFade){ fl=n/4; uint32_t mx=(uint32_t)((sRate?sRate:SR)*0.01f); if(fl>mx)fl=mx; if(fl<1)fl=1; }  // 淡变长度(~10ms 或 1/4)
  float drv = trimDrive? (1.0f+trimDrive*2.0f):0.0f; float drvN = drv>0? tanhf(drv):1.0f;                              // drive 量
  static int16_t ob[256]; uint32_t w=0;
  for(uint32_t k=0;k<n;k++){ uint32_t idx = trimRev ? (trimB-1-k) : (trimA+k);
    float x=sBuf[idx];
    if(a<1.0f){ lp+=a*(x-lp); x=lp; }                                            // LPF
    if(bits<16){ float lv=(float)(1<<(bits-1)); x=floorf(x/32768.0f*lv+0.5f)/lv*32768.0f; }  // 降位
    if(holdN>1){ if(hold<=0){ hv=x; hold=holdN; } x=hv; hold--; }                // 降采样保持
    if(drv>0){ float xn=x/32768.0f; xn=tanhf(xn*drv)/drvN; x=xn*32768.0f; }      // drive 软饱和暖化
    float fe=1.0f;                                                               // fade 淡入淡出
    if(fl){ if(k<fl)fe=(float)k/fl; else if(k+fl>=n)fe=(float)(n-k)/fl; if(fe<0)fe=0; if(fe>1)fe=1; }
    int32_t v=(int32_t)(x*gain*fe); if(v>32767)v=32767; else if(v<-32768)v=-32768;
    if(outBuf){ outBuf[k]=(int16_t)v; }                                         // 写到内存(试听用)
    else if(f){ ob[w++]=(int16_t)v; if(w==256){ f->write((const uint8_t*)ob,512); w=0; } }
    else { int av=v<0?-v:v; if(av>pk)pk=av; } }
  if(f&&w) f->write((const uint8_t*)ob,w*2);
  if(outPeak)*outPeak=pk;
}
void saveTrim(){
  if(!sBuf||trimB<=trimA)return;
  uint32_t n=trimB-trimA, d=n*2;
  float g=1.0f;
  if(trimNorm){ int pk; trimProcess(NULL,1.0f,&pk,NULL); g=22000.0f/pk; if(g>40.0f)g=40.0f; if(g<0.2f)g=0.2f; }  // 目标留余量, 不顶满刻度
  char p[40]; for(int i=1;i<999;i++){ snprintf(p,40,"/samples/TRIM%d.wav",i); if(!SD.exists(p))break; }
  File f=SD.open(p,FILE_WRITE); if(!f)return;
  f.write((const uint8_t*)"RIFF",4); w32(f,36+d); f.write((const uint8_t*)"WAVE",4);
  f.write((const uint8_t*)"fmt ",4); w32(f,16); w16(f,1); w16(f,1); w32(f,sRate); w32(f,sRate*2); w16(f,2); w16(f,16);
  f.write((const uint8_t*)"data",4); w32(f,d);
  trimProcess(&f, g, NULL, NULL);
  f.close(); rescan();
}
void drawRecorder(){
  hdr(recording?"REC":"RECORDER", recording?"":"` menu");
  if(recording){
    lcd.fillCircle(214,12,5,RED);   // 红点闪
    float s=(millis()-recT0)/1000.0f;
    lcd.setTextColor(HL); lcd.setTextSize(3); lcd.setCursor(14,40); lcd.printf("%.1f",s);
    lcd.setTextColor(GREY); lcd.setTextSize(1); lcd.setCursor(90,56); lcd.printf("/ %ds",REC_MAX_SEC);
    int w=(int)(224.0f*recPeak/4000.0f); if(w>224)w=224;
    lcd.drawRoundRect(8,76,228,18,3,LINE); lcd.fillRect(10,78,w,14,recPeak>3500?RED:HL);
    lcd.setTextColor(GREY); lcd.setCursor(8,100); lcd.printf("L=%ld R=%ld",(long)pkL,(long)pkR);
    footer("space = stop & save");
  } else {
    lcd.setTextColor(INK); lcd.setTextSize(1); lcd.setCursor(8,40); lcd.print(sdOK?"space = record    ok = play last":"NO SD CARD");
    lcd.setTextColor(GREY); lcd.setCursor(8,58); lcd.print("speak to mic, saves to /samples/RECn.wav");
    if(recMaxPk>0){ lcd.setTextColor(recMaxPk>200?HL:RED); lcd.setCursor(8,76);
      lcd.printf("last rec mic peak = %ld %s",(long)recMaxPk, recMaxPk>200?"(ok)":"(SILENT!)"); }
    if(diag[0]){ lcd.setTextColor(ACC); lcd.setCursor(8,92); lcd.print(diag); }
    lcd.setTextColor(GREY); lcd.setCursor(150,92); lcd.printf("free %luKB",(unsigned long)(ESP.getFreeHeap()/1024));
    if(recMsg[0]){ lcd.setTextColor(RED); lcd.setCursor(8,106); lcd.print(recMsg); }
    footer("space record   ok play   ` menu");
  }
}

// ---------- 录音库 (列表/播放/删除/改名) ----------
void libPlay(){ if(libSel<sN && loadWav(sPath[libSel])){ sLoaded=-1; audio.playFromBuffer(sBuf,sLen,sRate); } }
void libDelete(){ if(libSel<sN){ SD.remove(sPath[libSel]); rescan(); if(libSel>=sN)libSel=sN-1; if(libSel<0)libSel=0; } }
void libRename(){
  if(libSel>=sN||nameBuf[0]==0) return;
  char dir[72]; strncpy(dir,sPath[libSel],71); dir[71]=0;
  char* sl=strrchr(dir,'/'); if(!sl)return; *sl=0;            // 目录
  char np[96]; snprintf(np,sizeof(np),"%s/%s.wav",dir,nameBuf);
  SD.rename(sPath[libSel],np); rescan();
}
void libClampTop(){   // 边缘滚动: 选中项移出窗口才滚动
  if(libTop>sN-6)libTop=sN-6; if(libTop<0)libTop=0;
  if(libSel<libTop)libTop=libSel; else if(libSel>=libTop+6)libTop=libSel-5; if(libTop<0)libTop=0;
}
void drawLibRow(int i){   // 只画一行(自清背景), 滚动时局部刷新用
  int r=i-libTop; if(r<0||r>=6||i<0||i>=sN) return; int y=30+r*14;
  if(i==libSel){ lcd.fillRoundRect(4,y-1,232,14,2,SEL); lcd.setTextColor(BG);} else { lcd.fillRect(4,y-1,232,14,BG); lcd.setTextColor(INK); }
  lcd.setTextSize(1); lcd.setCursor(10,y+2); lcd.printf("%c %s", i==libSel?0x10:' ', sName[i]);
}
void drawLibrary(){
  hdr("LIBRARY","` menu");
  if(sN==0){ lcd.setTextColor(HL); lcd.setTextSize(2); lcd.setCursor(20,56); lcd.print("(no wav yet)"); footer("record some sounds first"); return; }
  libClampTop();
  for(int r=0;r<6 && libTop+r<sN;r++) drawLibRow(libTop+r);
  footer(";/. move  ok play  del delete  / rename");
  if(confirmDel){ lcd.fillRoundRect(20,46,200,44,6,RED); lcd.setTextColor(0xFFFF); lcd.setTextSize(1);
    lcd.setCursor(30,56); lcd.printf("delete %s ?",sName[libSel]); lcd.setCursor(30,72); lcd.print("ok = YES    ` = no"); }
  if(renaming){ lcd.fillRoundRect(16,44,208,48,6,SEL); lcd.setTextColor(BG); lcd.setTextSize(1);
    lcd.setCursor(26,52); lcd.print("rename (type, ok=save, `=cancel):"); lcd.setTextColor(0xFFFF); lcd.setTextSize(2);
    lcd.setCursor(26,68); lcd.printf("%s_",nameBuf); }
}

// ---------- 乐曲库 (合成器存档) ----------
void loadSongNames(){ memset(songNames,0,sizeof(songNames));
  File f=SD.open("/songs/names.dat",FILE_READ); if(f){ f.read((uint8_t*)songNames,sizeof(songNames)); f.close();
    for(int i=0;i<NSLOT;i++) songNames[i][15]=0; } }
void saveSongNames(){ SD.mkdir("/songs"); File f=SD.open("/songs/names.dat",FILE_WRITE);
  if(f){ f.write((uint8_t*)songNames,sizeof(songNames)); f.close(); } }
void scanSongs(){ char p[32];
  for(int i=0;i<NSLOT;i++){ snprintf(p,32,"/songs/SONG%d.dat",i+1); songFilled[i]=SD.exists(p); }
  loadSongNames(); }
void songClampTop(){
  if(songTop>NSLOT-6)songTop=NSLOT-6; if(songTop<0)songTop=0;
  if(songSel<songTop)songTop=songSel; else if(songSel>=songTop+6)songTop=songSel-5; if(songTop<0)songTop=0;
}
void drawSongRow(int i){
  int r=i-songTop; if(r<0||r>=6||i<0||i>=NSLOT) return; int y=30+r*14; bool sel=(i==songSel);
  if(sel) lcd.fillRoundRect(4,y-1,232,14,2,SEL); else lcd.fillRect(4,y-1,232,14,BG);
  if(i==previewSlot){ lcd.setTextColor(HL); lcd.setCursor(8,y+2); lcd.print(">"); }
  char nm[20]; if(songFilled[i]&&songNames[i][0]) snprintf(nm,20,"%s",songNames[i]); else snprintf(nm,20,"SONG %d",i+1);
  lcd.setTextColor(sel?BG:(songFilled[i]?INK:GREY)); lcd.setTextSize(1); lcd.setCursor(18,y+2); lcd.print(nm);
  lcd.setTextColor(sel?BG:GREY); lcd.setCursor(186,y+2); lcd.print(songFilled[i]?"saved":"empty");
}
void drawSongs(){
  hdr("SONGS","` menu");
  songClampTop();
  for(int r=0;r<6 && songTop+r<NSLOT;r++) drawSongRow(songTop+r);
  footer(";/. move  space play  ok edit  / name");
  if(renaming){ lcd.fillRoundRect(16,44,208,48,6,SEL); lcd.setTextColor(BG); lcd.setTextSize(1);
    lcd.setCursor(26,52); lcd.print("name song (ok=save, `=cancel):"); lcd.setTextColor(0xFFFF); lcd.setTextSize(2);
    lcd.setCursor(26,68); lcd.printf("%s_",nameBuf); }
}

void setup(){
  Serial.begin(115200);
  Wire.begin(8,9,400000);
  lcd.init(); lcd.setRotation(1);
  // 复古暖米色主题(与合成器统一)
  BG=lcd.color565(233,227,214); INK=lcd.color565(74,70,62); GREY=lcd.color565(150,144,130);
  ACC=lcd.color565(218,108,40); HL=lcd.color565(188,108,80); RED=lcd.color565(196,72,56);
  SEL=lcd.color565(120,110,92); CARD=lcd.color565(223,216,201); LINE=lcd.color565(196,189,174);
  kb.begin(&Wire,0x34);
  audio.begin(&Wire);
  sdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
  for(int hz=25000000; hz>=4000000 && !sdOK; hz/=2) sdOK=SD.begin(SD_CS,sdSPI,hz);
  if(sdOK) rescan();
  drawMenu();
}

void loop(){
  kb.update();
  if(st==MENU){
    if(kb.wasPressed("1")){ st=SYNTH; if(sBuf){free(sBuf);sBuf=NULL;sLoaded=-1;} if(rBuf){free(rBuf);rBuf=NULL;} seqEnter(); }
    else if(kb.wasPressed("2")){ st=SAMPLER; trimMode=false; if(rBuf){free(rBuf);rBuf=NULL;} if(sN&&sLoaded!=sCur){ if(loadWav(sPath[sCur]))sLoaded=sCur; } drawSampler(); }
    else if(kb.wasPressed("3")){ st=RECORDER; drawRecorder(); }
    else if(kb.wasPressed("4")){ st=SONGS; if(sBuf){free(sBuf);sBuf=NULL;sLoaded=-1;} if(rBuf){free(rBuf);rBuf=NULL;} scanSongs(); songSel=0; previewSlot=-1; renaming=false; drawSongs(); }
    else if(kb.wasPressed("5")){ st=LIBRARY; if(rBuf){free(rBuf);rBuf=NULL;} rescan(); libSel=0; confirmDel=false; renaming=false; drawLibrary(); }
    return;
  }
  // 通用返回 (改名时 ` 用作取消, 不退出)
  if(kb.wasPressed("`") && !(st==LIBRARY && (renaming||confirmDel)) && !(st==SONGS && renaming) && !(st==SAMPLER && trimMode)){
    if(recording) recStop();
    if(st==SYNTH) seqExit();
    if(st==SONGS && seqIsPreviewing()){ seqPreviewStop(); previewSlot=-1; }
    st=MENU; drawMenu(); return;
  }
  if(st==SAMPLER){
    if(trimMode){
      uint32_t step=sLen/128; if(step<1)step=1;
      if(kb.wasPressed("/")||kb.wasPressed("`")){ trimMode=false; drawSampler(); }   // 退回采样列表(不保存)
      else if(kb.wasPressed(",")){ trimA = (trimA>step)?trimA-step:0; if(trimA>=trimB&&trimB>0)trimA=trimB-1; drawSamplerTrim(); }
      else if(kb.wasPressed(".")){ trimA+=step; if(trimA>=trimB)trimA=(trimB>0?trimB-1:0); drawSamplerTrim(); }
      else if(kb.wasPressed(";")){ trimB=(trimB>step)?trimB-step:1; if(trimB<=trimA)trimB=trimA+1; drawSamplerTrim(); }
      else if(kb.wasPressed("'")){ trimB+=step; if(trimB>sLen)trimB=sLen; drawSamplerTrim(); }
      else if(kb.wasPressed("n")){ trimNorm=!trimNorm; drawSamplerTrim(); }
      else if(kb.wasPressed("r")){ trimRev=!trimRev; drawSamplerTrim(); }
      else if(kb.wasPressed("d")){ trimDrive=(trimDrive+1)%4; drawSamplerTrim(); }
      else if(kb.wasPressed("f")){ trimFade=!trimFade; drawSamplerTrim(); }
      else if(kb.wasPressed("[")){ if(trimLofi>0){trimLofi--; drawSamplerTrim();} }
      else if(kb.wasPressed("]")){ if(trimLofi<3){trimLofi++; drawSamplerTrim();} }
      else if(kb.wasPressed("space")){ if(sBuf&&trimB>trimA){      // 试听: 把效果(归一化/倒放/lo-fi/drive/fade)处理后再播
          uint32_t pn=trimB-trimA; int16_t* pb=(int16_t*)malloc(pn*2);
          if(pb){ float g=1.0f; if(trimNorm){ int pk; trimProcess(NULL,1.0f,&pk,NULL); g=22000.0f/pk; if(g>40.0f)g=40.0f; if(g<0.2f)g=0.2f; }
            trimProcess(NULL, g, NULL, pb); audio.playFromBuffer(pb, pn, sRate); free(pb); }
          else audio.playFromBuffer(sBuf+trimA, pn, sRate); } }   // 内存不足时退回原始试听
      else if(kb.wasPressed("ok")){ saveTrim(); trimMode=false; drawSampler(); }
      return;
    }
    bool ch=false;
    int pages=(sN+7)/8; if(pages<1)pages=1;
    if(kb.wasPressed("/") && sLoaded==sCur && sBuf && sLen){ trimMode=true; trimA=0; trimB=sLen; trimNorm=false; trimRev=false; trimLofi=0; trimDrive=0; trimFade=false; trimLofi=0;
      { uint32_t pk=1; for(uint32_t i=0;i<sLen;i++){ int v=sBuf[i]; if(v<0)v=-v; if((uint32_t)v>pk)pk=v; } smpPeak=pk; }   // 本段峰值, 用于波形显示放大
      drawSamplerTrim(); return; }
    if(kb.wasPressed("tab")){ sPage=(sPage+1)%pages; drawSampler(); }
    if(kb.wasPressed(",")&&sN){ sCur=(sCur+sN-1)%sN; ch=true; }   // 上一个
    if(kb.wasPressed(".")&&sN){ sCur=(sCur+1)%sN; ch=true; }      // 下一个
    for(int i=0;i<8;i++){ char d[2]={(char)('1'+i),0}; int idx=sPage*8+i;
      if(idx<sN && kb.wasPressed(d)){ sCur=idx; ch=true; } }
    if(ch){ sPage=sCur/8; if(loadWav(sPath[sCur]))sLoaded=sCur; drawSampler(); }
    for(auto k:NOTEKEYS){ if(kb.wasPressed(k)){ int m=midiOfKey(k);
      if(m>=0 && sBuf && sLen){ uint32_t rate=(uint32_t)(sRate*powf(2.0f,(m-60)/12.0f));
        audio.playFromBuffer(sBuf,sLen,rate); } } }
  } else if(st==RECORDER){
    if(kb.wasPressed("space")){ if(!recording) recStart(); else recStop(); drawRecorder(); }
    if(kb.wasPressed("ok") && !recording && recPath[0]){ if(loadWav(recPath)){ sLoaded=-1; audio.playFromBuffer(sBuf,sLen,sRate); } }  // 试听(从卡读回)
    if(recording){ recPump(); static uint32_t last=0; if(millis()-last>100){ drawRecorder(); last=millis(); } }
  } else if(st==LIBRARY){
    if(renaming){
      // 文字输入: 字母数字追加, del=退格, ok=保存, ` =取消
      static const char* CH="abcdefghijklmnopqrstuvwxyz0123456789";
      for(const char* p=CH; *p; p++){ char s[2]={*p,0}; if(kb.wasPressed(s)){ int l=strlen(nameBuf); if(l<16){nameBuf[l]=*p;nameBuf[l+1]=0;} drawLibrary(); } }
      if(kb.wasPressed("del")){ int l=strlen(nameBuf); if(l>0)nameBuf[l-1]=0; drawLibrary(); }
      if(kb.wasPressed("ok")){ libRename(); renaming=false; drawLibrary(); }
      if(kb.wasPressed("`")){ renaming=false; drawLibrary(); }
    } else if(confirmDel){
      if(kb.wasPressed("ok")){ libDelete(); confirmDel=false; drawLibrary(); }
      if(kb.wasPressed("`")){ confirmDel=false; drawLibrary(); }
    } else {
      if(kb.wasPressed(";")){ if(libSel>0){ int o=libSel; libSel--; int ot=libTop; libClampTop(); if(libTop==ot){drawLibRow(o);drawLibRow(libSel);} else drawLibrary(); } }
      if(kb.wasPressed(".")){ if(libSel<sN-1){ int o=libSel; libSel++; int ot=libTop; libClampTop(); if(libTop==ot){drawLibRow(o);drawLibRow(libSel);} else drawLibrary(); } }
      if(kb.wasPressed("ok") && sN){ libPlay(); }
      if(kb.wasPressed("del") && sN){ confirmDel=true; drawLibrary(); }
      if(kb.wasPressed("/") && sN){ renaming=true; nameBuf[0]=0; drawLibrary(); }
    }
  } else if(st==SYNTH){
    seqLoop();
  } else if(st==SONGS){
    if(renaming){
      static const char* CH="abcdefghijklmnopqrstuvwxyz0123456789";
      for(const char* p=CH; *p; p++){ char s[2]={*p,0}; if(kb.wasPressed(s)){ int l=strlen(nameBuf); if(l<15){nameBuf[l]=*p;nameBuf[l+1]=0;} drawSongs(); } }
      if(kb.wasPressed("del")){ int l=strlen(nameBuf); if(l>0)nameBuf[l-1]=0; drawSongs(); }
      if(kb.wasPressed("ok")||kb.wasPressed("enter")){ strncpy(songNames[songSel],nameBuf,15); songNames[songSel][15]=0; saveSongNames(); renaming=false; drawSongs(); }
      if(kb.wasPressed("`")){ renaming=false; drawSongs(); }
    } else {
      if(kb.wasPressed(";")){ if(songSel>0){ int o=songSel; songSel--; if(seqIsPreviewing()){seqPreviewStop();previewSlot=-1;} int ot=songTop; songClampTop(); if(songTop==ot){drawSongRow(o);drawSongRow(songSel);} else drawSongs(); } }
      if(kb.wasPressed(".")){ if(songSel<NSLOT-1){ int o=songSel; songSel++; if(seqIsPreviewing()){seqPreviewStop();previewSlot=-1;} int ot=songTop; songClampTop(); if(songTop==ot){drawSongRow(o);drawSongRow(songSel);} else drawSongs(); } }
      if(kb.wasPressed("space") && songFilled[songSel]){
        if(seqIsPreviewing()){ seqPreviewStop(); previewSlot=-1; }
        else { if(sBuf){free(sBuf);sBuf=NULL;sLoaded=-1;} if(rBuf){free(rBuf);rBuf=NULL;} seqPreviewStart(songSel); previewSlot=songSel; }
        drawSongs(); }
      if(kb.wasPressed("/") && songFilled[songSel]){
        if(seqIsPreviewing()){ seqPreviewStop(); previewSlot=-1; }
        renaming=true; strncpy(nameBuf,songNames[songSel],15); nameBuf[15]=0; drawSongs(); }
      if((kb.wasPressed("ok")||kb.wasPressed("enter")) && songFilled[songSel]){
        if(seqIsPreviewing()){ seqPreviewStop(); previewSlot=-1; }
        st=SYNTH; if(sBuf){free(sBuf);sBuf=NULL;sLoaded=-1;} if(rBuf){free(rBuf);rBuf=NULL;}
        seqQueueLoad(songSel); seqEnter(); }
      if(seqIsPreviewing()) seqPreviewPump();
    }
  }
  delay(8);
}
