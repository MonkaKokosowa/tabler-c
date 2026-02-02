# MPK Częstochowa Pi-Dashboard
A lightweight, bare-metal bus arrival dashboard for Raspberry Pi. This project bypasses heavy desktop environments (X11/Wayland) to render real-time transit data directly to the Linux Framebuffer using LVGL and C.

!["Example"](https://raw.githubusercontent.com/MonkaKokosowa/tabler-c/refs/heads/main/tabler.jpg)

## Features
- Direct Framebuffer Rendering: No window manager required; runs straight from the TTY.

- 4x Integer Scaling: Renders at a logical 640x360 and scales to a physical 1440x900 for massive, readable text.

- Smart Deep Cache: Fetches up to 10 upcoming departures and automatically slides new ones into view as buses depart.

- Dynamic Updates:

    - Clock: Per-second updates (Green).

    - Timetable: Recalculates "minutes left" every 2 seconds.

    - Auto-Refetch: Updates every 2 minutes OR immediately if the list drops below 2 entries.

    - Low Resource Usage: Uses <10MB of RAM and minimal CPU.

## Hardware Requirements
- Raspberry Pi (or any linux computer)

- Monitor: Optimized for 1440x900 (adjustable in main.c).

- Internet Connection: For fetching real-time MPK API data.

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
The project uses Pixel Tripling/Quadrupling to overcome the memory limitations of scaling fonts in software on embedded hardware.

- LVGL (Light and Versatile Graphics Library): Handles the UI logic and text rendering.

- libcurl: Handles synchronous API requests to the Częstochowa Live MPK API.

- cJSON: Parses the transit timetable.

- mmap: Maps /dev/fb0 into memory for fast pixel manipulation.
