#pragma once
#include <Arduino.h>
// 实时合成引擎 (后台任务喂 i2s TX), 给 STUDIO 用, 不依赖 M5Unified。
void seBegin(uint32_t sampleRate=16000);   // 启动: 配 codec + i2s TX + 音频任务
void seEnd();                              // 停止释放
void seNoteOn(int midi, int wave);         // wave: 0锯齿 1方波 2三角 3正弦 (持续音, 配 seNoteOff)
void seNoteOff(int midi);
void seTrig(int midi, int wave, uint32_t gateMs, float peak=1.0f, float detune=1.0f);  // 定时触发(音序器用)
void seTrigGlide(int midi, int wave, uint32_t gateMs, float peak, float detune, int fromMidi, uint32_t glideMs);  // 滑音触发(从 fromMidi 滑到 midi)
void seSampleTrig(int midi, const int16_t* buf, uint32_t len, uint32_t baseRate, uint32_t gateMs, float peak=1.0f, float detune=1.0f, bool rev=false);  // 录音当音色 (rev=反向)
void seDrum(int type,int kit=0);           // type 0-7, kit 0=ACOU 1=808 2=909
void seAllOff();
void seSetVolume(float v);                 // 0..1 主音量
void seSetTone(int h);                     // 2暗..14亮 波形谐波数
void seSetDrumVol(float v);                // 鼓音量倍数
void seSetCrush(int level);                // bitcrush: 0关 1/2/3 越来越脏
void seFxSet(int16_t* buf, uint32_t len, float wet, float fb, float damp);  // 空间效果(主延迟): app 提供缓冲(进 SYNTH 分配/退出释放)
void seFxClear();                          // 关闭空间效果(释放缓冲前先调)
void seSetBend(float f);                    // 全局播放速度倍率(1正常, 0停) 给磁带停用
void seDrone(float freqHz, float amp);      // 持续可控正弦(频率/音量平滑过渡) 给人偶低音/滑音 (=layer0)
void seDroneLayer(int layer, float freqHz, float amp);  // 多层 drone(层0-2) 给 SHAKE 太空 pad
void seLead(float freqHz, float amp, float tone);       // 连续单音(快速跟踪, 滑音/颤音由调用方控) tone:0暗1亮 给滑音合成器
// 内部 looper(LOOP station): 缓冲由 app 提供, 采集现场乐器输出
void seLooperSet(int16_t* buf, uint32_t maxLen);
void seLooperRec();      // 开始录第一层
void seLooperClose();    // 定长并开始循环
void seLooperOverdub(bool on);
void seLooperClear();
int  seLooperState();    // 0空 1录 2放 3叠录
uint32_t seLooperPos();
uint32_t seLooperLen();
void seDroneTone(float t);                  // drone 音色 0=暗(正弦)..1=亮(锯齿) 平滑过渡 给 SHAKE
void seBlip(float freqHz, uint32_t ms, float amp, int wave=3);   // 短促脉冲(wave:3正弦 4噪声) 给人偶炸开
void seGrain(const int16_t* buf, uint32_t len, uint32_t baseRate, float detune, float peak);  // 加窗颗粒(淡入淡出,防咔哒) 给 SHAKE 颗粒合成
// 无缝循环播放器(整段连续播放, 速度可实时连续调) 给 SHAKE 变速
void seLoopStart(const int16_t* buf, uint32_t len, uint32_t baseRate);
void seLoopRate(float rate);   // 播放速度倍率(含变调): 1=原速
void seLoopAmp(float amp);     // 0..1 音量(平滑)
void seLoopStop();
