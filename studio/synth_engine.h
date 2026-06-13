#pragma once
#include <Arduino.h>
// 实时合成引擎 (后台任务喂 i2s TX), 给 STUDIO 用, 不依赖 M5Unified。
void seBegin(uint32_t sampleRate=16000);   // 启动: 配 codec + i2s TX + 音频任务
void seEnd();                              // 停止释放
void seNoteOn(int midi, int wave);         // wave: 0锯齿 1方波 2三角 3正弦 (持续音, 配 seNoteOff)
void seNoteOff(int midi);
void seTrig(int midi, int wave, uint32_t gateMs, float peak=1.0f, float detune=1.0f);  // 定时触发(音序器用)
void seSampleTrig(int midi, const int16_t* buf, uint32_t len, uint32_t baseRate, uint32_t gateMs, float peak=1.0f, float detune=1.0f, bool rev=false);  // 录音当音色 (rev=反向)
void seDrum(int type,int kit=0);           // type 0-7, kit 0=ACOU 1=808 2=909
void seAllOff();
void seSetVolume(float v);                 // 0..1 主音量
void seSetTone(int h);                     // 2暗..14亮 波形谐波数
void seSetDrumVol(float v);                // 鼓音量倍数
void seSetCrush(int level);                // bitcrush: 0关 1/2/3 越来越脏
void seSetBend(float f);                    // 全局播放速度倍率(1正常, 0停) 给磁带停用
