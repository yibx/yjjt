#include <iostream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
// sudo apt install libgpiod2 libgpiod-dev
// sudo apt install gpiod
#include <gpiod.hpp>
#include <unistd.h>
// sudo apt-get install libspdlog-dev
#include "spdlog/spdlog.h"
#include "spdlog/sinks/daily_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"
// build   ./configure --with-mqtt --with-openssl && make clean && make
// cmake .. 修改 CMakeLists.txt 将mqtt设置为on
#include "hv/TcpServer.h"
#include "hv/requests.h"
#include "hv/json.hpp"
#include "hv/hv.h"
#include "hv/mqtt_client.h"
using namespace nlohmann;

using namespace std;
using namespace hv;

#include <mutex>

std::mutex mux;

// GPIO参数
const char* chip_name = "gpiochip0";
const int   signal_1 = 17;  // 中控室主控箱和船端控制箱连接状态异常
const int   signal_2 = 27;  // 中控室急停
const int   signal_3 = 22;  // 船端急停
const int   signal_4 = 23;  // 遥控器模拟报警
const int   signal_5 = 16;  // 自动手动切换，自动拍停
// consumer是GPIO引脚的使用者标识符，可以自定义设置。
const char* consumer = "gpio_alarm";

const int alarm1 = 12;  // 中控室灯闪
const int alarm2 = 6;   // 中控室蜂鸣器报警
const int alarm3 = 13;  // 自动/手动切换，一键拍停，输出高电平
const int alarm4 = 26;  // 船端报警，闪灯和蜂鸣器报警

// 定义返回值
#define SUCCESS 0    
#define FAILURE -1
bool signal1_open = false;
bool signal2_open = false;
bool signal3_open = false;
bool signal4_open = false;
bool signal5_open = false;

bool blight = false;
std::thread light_alarm_thread;

static bool subscribed_ = false; // 此处全部和类局部相同，用来判断是否订阅数据

// 版本号
#define VERSION_MAJOR 1
#define VERSION_MINOR 0
#define VERSION_PATCH 0

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#define VERSION_STRING TOSTRING(VERSION_MAJOR) "." TOSTRING(VERSION_MINOR) "." TOSTRING(VERSION_PATCH)

#define BUILD_DATE_VERSION __DATE__ " " __TIME__

// gpio初始化

gpiod_chip* chip = nullptr;

gpiod_line* line12 = nullptr;
gpiod_line* line6 = nullptr;
gpiod_line* line26 = nullptr;

bool bwarnRecord = false;
bool bstopRecord = false;
bool bremoveRecord = false;

/**
 * 保存系统状态
 * 
 * 停止类型 0：未停止 5：手动遥控器停止 10：控制箱停止 15：船端停止 20：系统自动停止
 * 急停状态 0：未急停 5：黄色报警 10：已急停
 */
int request_sql(string type, string status) {
    // 定义POST请求的表单数据
    std::string form_data = std::string("stopType=") + type + std::string("&stopStatus=") + status;
    http_headers headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";
    auto resp = requests::post("192.168.1.112:9092/stop/record/add", form_data, headers);
    if (resp == NULL) {
        spdlog::error("发送POST请求失败");
        return FAILURE;
    } else {
        spdlog::info("请求成功，返回值:{}", resp->body.c_str());
    }
    return SUCCESS;
}

/**
 * 保存系统状态
 * 
 * 停止类型 0：未停止 5：手动遥控器停止 10：控制箱停止 15：船端停止 20：系统自动停止
 * 急停状态 0：未急停 5：黄色报警 10：已急停
 */
int request_pt(string status) {
    // 定义POST请求的表单数据
    std::string form_data = std::string("isAutoStop=") + status;
    http_headers headers;
    headers["Content-Type"] = "application/x-www-form-urlencoded";
    auto resp = requests::post("192.168.1.112:9092/stop/record/autoStop", form_data, headers);
    if (resp == NULL) {
        spdlog::error("发送POST请求失败");
        return FAILURE;
    } else {
        spdlog::info("请求成功，返回值:{}", resp->body.c_str());
    }
    return SUCCESS;
}

// 写入文件
void write_to_file(const std::string& filename, const std::string& content) {
    std::ofstream file;
    file.open(filename.c_str(), std::ios::app);
    
    if (file.is_open()) {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        
        // 将时间转换为字符串并去掉末尾的换行符
        std::string time_str = std::ctime(&time);
        time_str = time_str.substr(0, time_str.length()-1);
        
        // 写入格式化的内容
        file << time_str << " " << content << std::endl;
        file.close();
        spdlog::info("成功写入文件: {}", content);
    } else {
        spdlog::error("无法打开文件");
    }
}

/**
 * 在libgpiod中，line_num = 18 表示的是BCM (Broadcom) GPIO编号系统。
 * 
 * 树莓派的三种常用GPIO编号方式：
 * 
 * 物理引脚编号 (Physical Pin): 1-40号，按板子上的物理位置顺序编号
 * BCM编号 (Broadcom): 这是GPIO的芯片级编号，例如GPIO18实际对应物理引脚12
 * WiringPi编号: WiringPi库使用的编号系统
 * 
 * 这里是一个对照表，以GPIO18为例：
 * 物理引脚号: 12
 * BCM编号: 18
 * WiringPi编号: 1
 * 
 * 注意：gpio引脚同时只能有一个使用，不能同时作为输入和输出
 */
 int set_gpio_alarm(int alarm_no, int waits) {
    if (!chip) {
        chip = gpiod_chip_open_by_name(chip_name);
        if (!chip) {
            spdlog::error("Open chip0 failed.");
            return FAILURE;
        }
    }
    gpiod_line* line_alarm = gpiod_chip_get_line(chip, alarm_no);
    if (!line_alarm) {
        spdlog::info("获取GPIO线路失败");
        return FAILURE;
    }
    if (gpiod_line_request_output(line_alarm, consumer, 0) < 0) {
        spdlog::info("设置GPIO输出模式失败");
        return FAILURE;
    }

    gpiod_line_set_value(line_alarm, 1);  // 报警
    std::this_thread::sleep_for(std::chrono::seconds(waits));
    gpiod_line_set_value(line_alarm, 0);  // 停止报警

    if (line_alarm) {
        gpiod_line_release(line_alarm);
        line_alarm = nullptr;
    }
    spdlog::info("报警完成 {} ", alarm_no);
    return SUCCESS;
 }

 int open_gpio_light() {
    if (!chip) {
        chip = gpiod_chip_open_by_name(chip_name);
        if (!chip) {
            spdlog::error("Open chip0 failed.");
            return FAILURE;
        }
    }
    if (!line12) {
        line12 = gpiod_chip_get_line(chip, 12);
        if (!line12) {
            spdlog::info("获取GPIO线路失败");
            return FAILURE;
        }
        spdlog::info("获取GPIO12线路成功");
    }
    if (gpiod_line_request_output(line12, consumer, 0) < 0) {
        spdlog::info("设置GPIO输出模式失败");
        return FAILURE;
    }
    blight = true;
    while (blight)
    {
        if (line12) {
            spdlog::info("灯光闪烁");
            gpiod_line_set_value(line12, 1);  // 闪灯
            std::this_thread::sleep_for(std::chrono::seconds(2));
            gpiod_line_set_value(line12, 0);  // 停止闪灯
            std::this_thread::sleep_for(std::chrono::seconds(2));
        } else {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    return SUCCESS;
 }

 int close_gpio_light() {
    if (!chip) {
        chip = gpiod_chip_open_by_name(chip_name);
        if (!chip) {
            spdlog::error("Open chip0 failed.");
            return FAILURE;
        }
    }
    if (line12) {
        gpiod_line_set_value(line12, 0);  // 停止报警
        gpiod_line_release(line12);
        line12 = nullptr;
    }
    spdlog::info("关闭灯光");
    return SUCCESS;
 }

int open_fmq_alarm() {
   if (!chip) {
       chip = gpiod_chip_open_by_name(chip_name);
       if (!chip) {
           spdlog::error("Open chip0 failed.");
           return FAILURE;
       }
   }
   if (!line6) {
       line6 = gpiod_chip_get_line(chip, 6);
       if (!line6) {
           spdlog::info("获取GPIO线路失败");
           return FAILURE;
       }
       spdlog::info("获取GPIO6线路成功");
   }
   if (gpiod_line_request_output(line6, consumer, 0) < 0) {
       spdlog::info("设置GPIO输出模式失败");
       return FAILURE;
   }
   gpiod_line_set_value(line6, 1);  // 报警
   spdlog::info("蜂鸣器报警");
   return SUCCESS;
}

int close_fmq_alarm() {
   if (!chip) {
       chip = gpiod_chip_open_by_name(chip_name);
       if (!chip) {
           spdlog::error("Open chip0 failed.");
           return FAILURE;
       }
   }
   if (line6) {
       gpiod_line_set_value(line6, 0);  // 停止报警
       gpiod_line_release(line6);
       line6 = nullptr;
   }
   spdlog::info("关闭蜂鸣器报警");
   return SUCCESS;
}

int open_ship_alarm() {
   if (!chip) {
       chip = gpiod_chip_open_by_name(chip_name);
       if (!chip) {
           spdlog::error("Open chip0 failed.");
           return FAILURE;
       }
   }
   if (!line26) {
        line26 = gpiod_chip_get_line(chip, 26);
       if (!line26) {
           spdlog::info("获取GPIO线路失败");
           return FAILURE;
       }
       spdlog::info("获取GPIO26线路成功");
   }
   if (gpiod_line_request_output(line26, consumer, 0) < 0) {
       spdlog::info("设置GPIO输出模式失败");
       return FAILURE;
   }
   gpiod_line_set_value(line26, 1);  // 报警
   spdlog::info("蜂鸣器报警");
   return SUCCESS;
}

int close_ship_alarm() {
   if (!chip) {
       chip = gpiod_chip_open_by_name(chip_name);
       if (!chip) {
           spdlog::error("Open chip0 failed.");
           return FAILURE;
       }
   }
   if (line26) {
       gpiod_line_set_value(line26, 0);  // 停止报警
       gpiod_line_release(line26);
       line26 = nullptr;
   }
   spdlog::info("关闭蜂鸣器报警");
   return SUCCESS;
}

// 蜂鸣器报警
void fmq_alarm() {
    if(SUCCESS != set_gpio_alarm(6, 20)) {
        set_gpio_alarm(6, 30);
    }
}

// 船端报警
void ship_alarm() {
    if(SUCCESS != set_gpio_alarm(26, 20)) {
        set_gpio_alarm(26, 30);
    }
}

// 闪灯
void light_alarm() {
    if(SUCCESS != set_gpio_alarm(12,20)) {
        set_gpio_alarm(12, 30);
    }
}

// 一键拍停
void yjpt_alarm() {
    if(SUCCESS != set_gpio_alarm(13,1)) {
        set_gpio_alarm(13, 1);
    }
}

// mqtt订阅
class HVMqtt {
    private:
        mqtt_client_t* cli_;
        bool running_;
        std::string host_;
        int port_;
        std::string topic_;
        
    
    public:
        static void on_mqtt(mqtt_client_t* cli, int type) {
            mqtt_message_t* msg;

            switch(type) {
            case MQTT_TYPE_CONNECT:
                spdlog::info("mqtt 连接");
                break;
            case MQTT_TYPE_DISCONNECT:
                spdlog::info("mqtt 断开");
                subscribed_ = false;  // 断开时重置订阅状态
                mqtt_client_connect(cli, "localhost", 1883, 0);
                break;
            case MQTT_TYPE_CONNACK:
                spdlog::info("mqtt 连接建立");
                if (!subscribed_) {
                    mqtt_client_subscribe(cli, "warnRecord", 1);
                    mqtt_client_subscribe(cli, "autoStop", 1);
                    subscribed_ = true;  // 标记已订阅
                }
                break;
            case MQTT_TYPE_PUBLISH:
                msg = &cli->message;
                spdlog::info("订阅: {} {}", msg->topic_len, msg->topic);
                spdlog::info("荷载: {} {}", msg->payload_len, msg->payload);
                
                if (std::strstr(msg->topic, "warnRecord") != NULL) {
                    // 系统监测值异常输出 6，26报警
                    open_fmq_alarm();
                    spdlog::info("蜂鸣器警告记录");
                    open_ship_alarm();
                    spdlog::info("船端警告记录");
                    if (!light_alarm_thread.joinable()) { 
                        light_alarm_thread = std::thread(open_gpio_light);
                        spdlog::info("Light alarm thread started.");
                    }
                } else if (std::strstr(msg->topic, "autoStop") != NULL) {
                    if (SUCCESS != request_sql("20", "10")) {
                        request_sql("20", "10");
                    }
                    open_fmq_alarm();
                    spdlog::info("蜂鸣器警告记录");
                    open_ship_alarm();
                    spdlog::info("船端警告记录");
                    if (!light_alarm_thread.joinable()) { 
                        light_alarm_thread = std::thread(open_gpio_light);
                        spdlog::info("Light alarm thread started.");
                    }
                    yjpt_alarm();
                }
                break;
            }
        }

        bool init(std::string host, int port, std::string topic) {
            host_ = host;
            port_ = port;
            topic_ = topic;
            return true;
        }
    
        int start_subscribe() {
            running_ = true;
            cli_ = mqtt_client_new(NULL);
            cli_->keepalive = 10;
            
            char client_id[64];
            snprintf(client_id, sizeof(client_id), "mqtt_sub_%ld", hv_getpid());
            mqtt_client_set_id(cli_, client_id);
            mqtt_client_set_callback(cli_, on_mqtt);
    
            mqtt_client_connect(cli_, host_.c_str(), port_, 0);
            //mqtt_client_subscribe(cli_, topic_.c_str(), 1);
    
            while(running_) {
                mqtt_client_run(cli_);
            }
            return SUCCESS;
        }
    
        void stop_subscribe() {
            running_ = false;
            if(cli_) {
                mqtt_client_disconnect(cli_);
                mqtt_client_free(cli_);
                cli_ = nullptr;
            }
        }
    };
    
// 初始化日志库
void init_logger() {
    try {
        // 创建每日轮转的日志文件，最大大小5MB，保留3个备份文件
        auto max_size = 1024*1024 * 5;
        auto max_files = 3;
        auto daily_logger = spdlog::daily_logger_mt("daily_logger", "logs/gpio_server.log", 0, 0);
        spdlog::set_default_logger(daily_logger);
        
        // 设置日志格式
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        
        // 设置日志级别
        spdlog::set_level(spdlog::level::debug);
        
        // 开启异步日志
        spdlog::flush_every(std::chrono::seconds(3));
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }
}

/**
 * 命令：gpioinfo  pinout
 * GPIO 2-3: I2C专用
 * GPIO 14-15: UART专用
 * GPIO 18,19,21: PCM/PWM
 * GPIO 7-11: SPI专用
 * GPIO 17,27,22,23,24,25: 通用GPIO
 * GPIO 4: 通用GPIO，也可用于1-wire
 * GPIO 12,13,16,20,26: 通用GPIO
 * 
 * 网址：
 * https://pinout.xyz/pinout/ground
 */

#if 0
void get_line_val(const int& signal_number, gpiod_line* line, bool& signal_open) {
    auto val = gpiod_line_get_value(line);
    if (val == 0) {
        signal_open = false;
    } else if (val == 1) {
        signal_open = true;
    } else if (val < 0) {
        spdlog::error("Get signal {} status error", signal_number);
        return;
    }
    // 获取当前时间并格式化为字符串
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm* tm_time = std::localtime(&time);
    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%Y%m%d", tm_time);
    // 组合文件名
    //std::string filename = std::string("gpio") + std::to_string(signal_number) + "_" + time_str + ".txt";
    if (signal_open) {
        // 中控室主控箱急停按钮
        if ((signal_number == 2) && (signal2_open == false)) {
            signal2_open = true;
            spdlog::info("中控室主控箱急停按下 signal {}  open", signal_number);
            if(SUCCESS != request_sql("10", "10")) {
                request_sql("10", "10");
            }
        } else if ((signal_number == 3) && (signal3_open == false)) {
            // 船端按下急停按钮
            spdlog::info("船端按下急停按钮 signal {}  open", signal_number);
            signal3_open = true;
            std::thread light_alarm_thread(light_alarm);
            light_alarm_thread.detach(); 
            if(SUCCESS != request_sql("15", "10")) {
                request_sql("15", "10");
            }
        } else if ((signal_number == 4) && (signal4_open == false)) {
            signal4_open = true;
            // 中控室复位
            spdlog::info("中控室复位 signal {}  open", signal_number);
            if(SUCCESS != request_sql("0", "0")) {
                request_sql("0", "0");
            }
        }  else if ((signal_number == 1) && (signal1_open == false)) {
            signal1_open = true;
            spdlog::info("中控室主控箱和船端控制箱连接状态正常 signal {}  open", signal_number);
        } else if ((signal_number == 5) && (signal5_open == false)) {
            signal5_open = true;
            yjpt_alarm();
            spdlog::info("自动拍停开始 signal {}  open", signal_number);
            if(SUCCESS != request_pt("0")) {
                request_pt("0");
            }
        }
    } else {
        // 中控室主控箱和船端控制箱连接状态异常
        if ((signal_number == 1) && (signal1_open == true)) {
            spdlog::error("中控室主控箱和船端控制箱连接状态异常 signal {}  closed", signal_number);
            signal1_open = false;
        } else if ((signal_number == 2) && (signal2_open == true)) {
            spdlog::error("中控室主控箱急停关闭 signal {}  closed", signal_number);
            signal2_open = false;
        } else if ((signal_number == 3) && (signal3_open == true)) {
            spdlog::error("船端按下急停关闭 signal {}  closed", signal_number);
            signal3_open = false;
        } else if ((signal_number == 4) && (signal4_open == true)) {
            spdlog::error("中控室复位关闭 signal {}  closed", signal_number);
            signal4_open = false;
        } else if ((signal_number == 5) && (signal5_open == true)) {
            signal5_open = false;
            spdlog::info("自动拍停关闭 signal {}  open", signal_number);
            if(SUCCESS != request_pt("1")) {
                request_pt("1");
            }
        }
    }
}
#endif
void get_line_val(const int& signal_number, gpiod_line* line, bool& signal_open) {
    enum SignalState {
        CLOSED,
        OPEN
    };

    static SignalState signal_states[5] = {CLOSED, CLOSED, CLOSED, CLOSED, CLOSED};

    auto val = gpiod_line_get_value(line);
    if (val < 0) {
        spdlog::error("Get signal {} status error", signal_number);
        return;
    }

    SignalState current_state = (val == 1) ? OPEN : CLOSED;

    if (signal_states[signal_number - 1] != current_state) {
        signal_states[signal_number - 1] = current_state;

        if (current_state == OPEN) {
            switch (signal_number) {
                case 1:
                {
                    signal1_open = true;
                    spdlog::info("中控室主控箱和船端控制箱连接状态正常 signal {} open", signal_number);
                    break;
                }
                case 2:
                {
                    signal2_open = true;
                    spdlog::info("中控室主控箱急停按下 signal {} open", signal_number);
                    open_fmq_alarm();
                    spdlog::info("蜂鸣器警告记录");
                    open_ship_alarm();
                    spdlog::info("船端警告记录");

                    if (!light_alarm_thread.joinable()) { 
                        light_alarm_thread = std::thread(open_gpio_light);
                        spdlog::info("Light alarm thread started.");
                    }
                    if (SUCCESS != request_sql("10", "10")) {
                        request_sql("10", "10");
                    }
                    break;
                }
                case 3:
                {
                    signal3_open = true;
                    spdlog::info("船端按下急停按钮 signal {} open", signal_number);
                    open_fmq_alarm();
                    spdlog::info("蜂鸣器警告记录");
                    open_ship_alarm();
                    spdlog::info("船端警告记录");
                    if (!light_alarm_thread.joinable()) { 
                        light_alarm_thread = std::thread(open_gpio_light);
                        spdlog::info("Light alarm thread started.");
                    }
                    if (SUCCESS != request_sql("15", "10")) {
                        request_sql("15", "10");
                    }
                    break;
                }
                case 4:
                {
                    signal4_open = true;
                    spdlog::info("中控室复位 signal {} open", signal_number);
                    close_fmq_alarm();
                    spdlog::info("停止蜂鸣器警告记录");
                    close_ship_alarm();
                    spdlog::info("停止船端警告记录");
                    blight = false;
                    if (light_alarm_thread.joinable()) {
                        light_alarm_thread.join(); // 等待线程结束
                        spdlog::info("Light alarm thread joined.");
                    }
                    close_gpio_light();
                    if (SUCCESS != request_sql("0", "0")) {
                        request_sql("0", "0");
                    }
                    break;
                }
                case 5:
                {
                    signal5_open = true;
                    spdlog::info("自动拍停开始 signal {} open", signal_number);
                    if (SUCCESS != request_pt("0")) {
                        request_pt("0");
                    }
                    break;
                }
                default:
                    break;
            }
        } else {
            switch (signal_number) {
                case 1:
                {
                    signal1_open = false;
                    spdlog::error("中控室主控箱和船端控制箱连接状态异常 signal {} closed", signal_number);
                    break;
                }
                case 2:
                {
                    signal2_open = false;
                    close_fmq_alarm();
                    spdlog::info("停止蜂鸣器警告记录");
                    close_ship_alarm();
                    spdlog::info("停止船端警告记录");
                    blight = false;
                    if (light_alarm_thread.joinable()) {
                        light_alarm_thread.join(); // 等待线程结束
                        spdlog::info("Light alarm thread joined.");
                    }
                    close_gpio_light();
                    spdlog::error("中控室主控箱急停关闭 signal {} closed", signal_number);
                    break;
                }
                case 3:
                {
                    signal3_open = false;
                    spdlog::error("船端按下急停关闭 signal {} closed", signal_number);
                    close_fmq_alarm();
                    spdlog::info("停止蜂鸣器警告记录");
                    close_ship_alarm();
                    spdlog::info("停止船端警告记录");
                    blight = false;
                    if (light_alarm_thread.joinable()) {
                        light_alarm_thread.join(); // 等待线程结束
                        spdlog::info("Light alarm thread joined.");
                    }
                    close_gpio_light();
                    break;
                }
                case 4:
                {
                    signal4_open = false;
                    spdlog::error("中控室复位关闭 signal {} closed", signal_number);
                    break;
                }
                case 5:
                {
                    signal5_open = false;
                    spdlog::info("自动拍停关闭 signal {} closed", signal_number);
                    if (SUCCESS != request_pt("1")) {
                        request_pt("1");
                    }
                    break;
                }
            }
        }
    }
}
// 获取gpio状态
int get_gpio_value() {
    if (!chip) {
        chip = gpiod_chip_open_by_name(chip_name);
        if (!chip) {
            spdlog::error("Open chip0 failed.");
            return FAILURE;
        }
    }

    // No.1 GPIO17
    gpiod_line* line_17 = gpiod_chip_get_line(chip, signal_1);
    // No.2 GPIO27
    gpiod_line* line_27 = gpiod_chip_get_line(chip, signal_2);
    // No.3 GPIO22
    gpiod_line* line_22 = gpiod_chip_get_line(chip, signal_3);
    // No.4 GPIO23
    gpiod_line* line_23 = gpiod_chip_get_line(chip, signal_4);
    // No.5 GPIO5
    gpiod_line* line_5 = gpiod_chip_get_line(chip, signal_5);

    if (!line_17 || !line_27 || !line_22 || !line_23 || !line_5) {
        spdlog::error("Get chip0 lines failed.");
        return FAILURE;
    }
    gpiod_line_request_input(line_17, consumer);
    gpiod_line_request_input(line_27, consumer);
    gpiod_line_request_input(line_22, consumer);
    gpiod_line_request_input(line_23, consumer);
    gpiod_line_request_input(line_5, consumer);
    // 获取GPIO状态
    bool signal_open[5] = {false};
    gpiod_line*  signal_lines[5] = {line_17, line_27, line_22, line_23, line_5};

    while (true)
    {
        get_line_val(1, line_17, signal_open[0]);
        get_line_val(2, line_27, signal_open[1]);
        get_line_val(3, line_22, signal_open[2]);
        get_line_val(4, line_23, signal_open[3]);
        get_line_val(5, line_5, signal_open[4]);
        sleep(1);
    }

    return SUCCESS;
}

// json解析
std::string parse_json(std::string msg) {
    spdlog::info("Received message: {}", msg);
    
    try {
        hv::Json json = hv::Json::parse(msg);
        
        // 解析 JSON 字段
        if (json.contains("gpio")) {
            int gpio_num = json["gpio"].get<int>();
            int value = json["value"].get<int>();
            // 返回成功响应
            hv::Json response = {
                {"status", "success"},
                {"gpio", gpio_num},
                {"value", value}
            };
            //channel->write(response.dump());
            return response.dump();
        }
        return "Invalid Value";
    }
    catch (const std::exception& e) {
        spdlog::error("JSON parse error: {}", e.what());
        
        // 返回错误响应
        hv::Json error = {
            {"status", "error"},
            {"message", "Invalid JSON format"}
        };
        //channel->write(error.dump());
        return error.dump();
    }
}

// tcp服务
int recv_alarm() {
    int port = 8090;
    TcpServer srv;
    int listenfd = srv.createsocket(port);
    if (listenfd < 0) {
        return -1;
    }
    printf("server listen on port %d, listenfd=%d ...\n", port, listenfd);
    spdlog::info("server listen on port {}, listenfd={} ...", port, listenfd);
    srv.onConnection = [](const SocketChannelPtr& channel) {
        std::string peeraddr = channel->peeraddr();
        if (channel->isConnected()) {
            printf("%s connected! connfd=%d\n", peeraddr.c_str(), channel->fd());
            spdlog::info("{} connected! connfd={}", peeraddr, channel->fd());
        } else {
            printf("%s disconnected! connfd=%d\n", peeraddr.c_str(), channel->fd());
            spdlog::info("{} disconnected! connfd={}", peeraddr, channel->fd());
        }
    };
    srv.onMessage = [](const SocketChannelPtr& channel, Buffer* buf) {
        // echo
        std::string msg((const char*)buf->data(), buf->size());
        channel->write(parse_json(msg));
    };
    srv.setThreadNum(4);
    srv.start();

    // press Enter to stop
    while (getchar() != '\n');
    return 0;
}

int main(int argc, char* argv[]) {   
    init_logger();
    std::cout << "Software Version: " << string(VERSION_STRING) << std::endl;
    spdlog::info("Software Version: {}", string(VERSION_STRING));
    std::cout << "Build Date and Time: " << string(BUILD_DATE_VERSION) << std::endl;
	spdlog::info("Build Date and Time: {}", string(BUILD_DATE_VERSION));
    std::cout << "Server starting..." << std::endl;
    spdlog::info("Server starting...");
  
    // 启动GPIO监测
    std::thread gpio_thread(get_gpio_value);
    gpio_thread.detach();  
    
    // 启动TCP服务器
    //recv_alarm();
    // MQTT连接
    HVMqtt mqtt;
    mqtt.init("localhost", 1883, "warnRecord");
    // 启动订阅
    std::thread sub_thread(&HVMqtt::start_subscribe, &mqtt);
    sub_thread.join();
    // 停止订阅
    mqtt.stop_subscribe();
    std::cout << "Server stopped" << std::endl;
    spdlog::info("Server stopped");
    return 0;
}
