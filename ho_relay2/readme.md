# HoCtrl Firmware Releases

Firmware releases for HoCtrl devices.


其中 #define BUTTON_PIN 9 定义了板载按钮，也就是BOOT键的IO口
#define RELAY_PIN 4  定义了板载继电器或MOS管的IO口
模块上还有一个状态指示灯为IO3
另外模块上还额外引出了2个IO，分别为IO0和IO1
*/

#define BUTTON_PIN 9  //boot键IO
#define RELAY_PIN 4  //继电器IO口
#define DEBOUNCE_DELAY 50  // 去抖动延时，单位：毫秒  


