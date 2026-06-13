#pragma once
void seqEnter();   // 进入音序器(启动引擎+初始化)
void seqQueueLoad(int slot);  // 进入前预约载入某存档槽(0..7)
void seqPreviewStart(int slot);  // 乐曲库试听: 载入并播放(不进编辑器)
void seqPreviewStop();
void seqPreviewPump();           // 每帧推进试听
bool seqIsPreviewing();
void seqExit();    // 退出(停引擎)
void seqLoop();    // 每帧处理(键盘+时序+绘制)
