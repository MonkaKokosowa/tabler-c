# MPK Częstochowa Pi-Dashboard
A lightweight, bare-metal bus arrival dashboard for Raspberry Pi. This project bypasses heavy desktop environments (X11/Wayland) to render real-time transit data directly to the Linux Framebuffer using LVGL and C.

!["Example"](https://raw.githubusercontent.com/MonkaKokosowa/tabler-c/refs/heads/main/tabler.jpg)

!["ExampleSS"](https://raw.githubusercontent.com/MonkaKokosowa/tabler-c/refs/heads/main/tabler2.png)

## Features
- Direct framebuffer rendering, no window manager, runs straight from the TTY
- Renders natively at 1440x900, no upscaling blur
- Pulls upcoming departures and slides new ones in as buses leave
- Clock updates every second, timetable recalculates "minutes left" every 2s
- Refetches from the API every 2 minutes
- Low resource usage, fine on a Pi

## Hardware Requirements
- Raspberry Pi (or any linux computer)
- A display, tuned for 1440x900 (change HOR_RES/VER_RES in main.c for other resolutions)
- Internet connection for the MPK API

## Installation
### 1. Install Dependencies
```bash
sudo apt-get update
sudo apt-get install git gcc make libcurl4-openssl-dev libcjson-dev
```
### 2. Setup Project
```bash
git clone https://github.com/MonkaKokosowa/tabler-c.git
cd tabler-c
git clone -b v8.3.10 https://github.com/lvgl/lvgl.git
```
### 3. System Configuration
To ensure a clean dashboard without kernel text or a blinking cursor:

Edit /boot/cmdline.txt: Append the following to the existing line:

```ini
video=HDMI-A-1:1440x900@60 vt.global_cursor_default=0 loglevel=3 quiet logo.nologo console=tty3
```
Disable the Login Prompt:

```bash
sudo systemctl disable --now getty@tty1.service
```
Configure environment:
```bash
cp .env.example .env
nano .env
```
## Building and Running
```bash
# Build the project (uses 4 cores)
make -j4

# Run manually
sudo ./dashboard
```
## Automatic Startup (Systemd)
To make the dashboard start automatically on boot, create the service file:

sudo nano /etc/systemd/system/mpk.service

```ini
[Unit]
Description=MPK Czestochowa Dashboard
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=root
WorkingDirectory=/home/user/tabler-c
ExecStart=/home/user/tabler-c/dashboard
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```
```bash
sudo systemctl daemon-reload
sudo systemctl enable mpk.service
```
## Technical Architecture
- LVGL handles the UI and text rendering
- libcurl does the (synchronous) API requests to the Częstochowa Live MPK API
- cJSON parses the timetable/weather responses
- mmap maps /dev/fb0 for direct pixel writes
