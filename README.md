## Ubuntu 20.04 on Raspberry Pi 4B

一、监测系统

该监测系统主要监测外部设备的运行状态以及报警信息。
包括两部分，一部分是通过订阅mq主题，当收到报警信息之后，触发报警，
另外一部分是接收到操作信号，发送post请求保存操作信息。

使用的树莓派的输入引脚：17，27，22，23

输出引脚 12 6

mq订阅主题：

warnRecord，对应的日志是：warnRecord.txt

removeRecord，对应的日志是：removeRecord.txt

stopRecord，对应的日志是：stopRecord.txt

总的日志在：logs文件夹中


二、开机自启动
```bash
sudo nano /etc/systemd/system/gpio-server.service
```
```bash
[Unit]
Description=GPIO Server Service
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/path/to/your/program
ExecStart=/path/to/your/program/server
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```
启动服务
```bash
sudo systemctl enable gpio-server.service
sudo systemctl start gpio-server.service
```
监测服务状态:
```bash
sudo systemctl status gpio-server.service
```
查看日志
```bash
journalctl -u gpio-server.service
```

三、远程访问现场的树莓派4B

硬件设备需求：

4G模块选择  
华为ME909s-821  
移远EC20  
中兴ME3630 任选其一即可  

SIM卡

需要公网IP的物联网卡  
建议选择电信或联通的物联网卡  

软件配置方案：

4G模块驱动安装：
```bash
sudo apt update
sudo apt install ppp usb-modeswitch
```
安装远程访问工具：
```bash
sudo apt install openssh-server
```

安装内网穿透工具(任选其一)：
```bash
wget https://github.com/fatedier/frp/releases/download/v0.37.1/frp_0.37.1_linux_arm64.tar.gz
```

实现方案：

方案1：使用公网IP直接访问

获取4G分配的公网IP  
配置SSH远程访问  
设置端口转发  

方案2：使用内网穿透

需要一台具有公网IP的服务器  
在服务器上部署frps  
在树莓派上部署frpc  
配置转发规则  

配置文件示例：
```bash
[common]
server_addr = 你的服务器IP
server_port = 7000
token = 你的密钥

[ssh]
type = tcp
local_ip = 127.0.0.1
local_port = 22
remote_port = 6000
```
frpc.ini

安全建议：
 
修改默认SSH端口  
使用密钥登录替代密码  
配置防火墙规则  
定期更新系统   

监控建议：

安装网络监控工具
```bash
sudo apt install netdata
```
设置自动重连脚本
```bash
sudo apt install autossh
```