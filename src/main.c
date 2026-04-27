/*******************************************************************
 *
 * main.c - LVGL simulator for GNU/Linux
 *
 * Based on the original file from the repository
 *
 * @note eventually this file won't contain a main function and will
 * become a library supporting all major operating systems
 *
 * To see how each driver is initialized check the
 * 'src/lib/display_backends' directory
 *
 * - Clean up
 * - Support for multiple backends at once
 *   2025 EDGEMTech Ltd.
 *
 * Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 ******************************************************************/
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/lib/driver_backends.h"
#include "src/lib/simulator_util.h"
#include "src/lib/simulator_settings.h"

#include "src/cjson/cJSON.h"

/* 声明外部字体 */
LV_FONT_DECLARE(my_font)

// 开发板对应路径
#define LED_PATH   "/sys/class/leds/sys-led/brightness"
#define BACKLIGHT_PATH   "/sys/class/backlight/backlight/brightness" // 改回你之前的正确路径
#define TEMP_PATH "/sys/class/thermal/thermal_zone0/temp"

// 指定电脑端的ip端口
#define SERVER_IP "192.168.56.10"
#define SERVER_PORT 8080

// 摄像头默认参数
#define CAMERA_DEV "/dev/video1"
#define CAMERA_WIDTH 640
#define CAMERA_HEIGHT 480
#define CAMERA_BUF_COUNT 4
#define CAMERA_CONVERT_FPS 15

// ================= 全局变量 =================
static lv_obj_t *chart;
static lv_chart_series_t *ser1;
static lv_obj_t *temp_label;
static lv_obj_t *weather_label; // 【新增】修复未声明错误
static lv_obj_t *camera_status_label;
static lv_obj_t *camera_metrics_label;

typedef struct {
    void * start;
    size_t length;
} camera_buffer_t;

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    camera_buffer_t buffers[CAMERA_BUF_COUNT];
    uint32_t buffer_count;

    pthread_t thread;
    bool running;

    uint16_t * latest_rgb565;
    uint16_t * work_rgb565;
    size_t latest_rgb565_bytes;
    bool frame_ready;
    uint32_t frame_seq;
    uint32_t last_render_seq;
    uint32_t converted_frames;
    uint64_t last_frame_ready_us;
    pthread_mutex_t frame_mutex;

    float convert_fps;
    float render_fps;
    float render_latency_ms;
    float process_cpu_percent;
    float snapshot_ms;

    lv_obj_t * canvas;
    uint8_t * canvas_buf;
} camera_ctx_t;

static camera_ctx_t g_camera = {
    .fd = -1,
    .width = CAMERA_WIDTH,
    .height = CAMERA_HEIGHT,
};

// 全局天气数据结构
typedef struct {
    char city[32];
    char weather_str[32];
    int temperature;
    bool is_updated;
    pthread_mutex_t mutex; // 互斥锁
} weather_data_t; // 【新增】加上了名字和分号

static weather_data_t g_weather = { .is_updated = false };

static int xioctl(int fd, unsigned long request, void * arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while(ret == -1 && errno == EINTR);
    return ret;
}

static inline uint8_t clamp_u8(int v)
{
    if(v < 0) return 0;
    if(v > 255) return 255;
    return (uint8_t)v;
}

static uint64_t now_us_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static float read_process_cpu_percent(void)
{
    static uint64_t prev_total_ticks = 0;
    static uint64_t prev_proc_ticks = 0;
    uint64_t total_ticks = 0;
    uint64_t proc_ticks = 0;
    FILE *fp;
    char line[512];
    char *rp;
    char state;
    long ppid, pgrp, session, tty_nr, tpgid;
    unsigned long long flags, minflt, cminflt, majflt, cmajflt, utime, stime;
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0, guest = 0, guest_nice = 0;
    long ncpu;

    fp = fopen("/proc/self/stat", "r");
    if(fp == NULL) return 0.0f;
    if(fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return 0.0f;
    }
    fclose(fp);

    rp = strrchr(line, ')');
    if(rp == NULL) return 0.0f;

    if(sscanf(rp + 2,
              "%c %ld %ld %ld %ld %ld %llu %llu %llu %llu %llu %llu",
              &state, &ppid, &pgrp, &session, &tty_nr, &tpgid,
              &flags, &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime) != 12) {
        return 0.0f;
    }
    proc_ticks = (uint64_t)utime + (uint64_t)stime;

    fp = fopen("/proc/stat", "r");
    if(fp == NULL) return 0.0f;
    if(fgets(line, sizeof(line), fp) == NULL) {
        fclose(fp);
        return 0.0f;
    }
    fclose(fp);

    if(sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
              &user, &nice, &sys, &idle, &iowait, &irq, &softirq,
              &steal, &guest, &guest_nice) < 4) {
        return 0.0f;
    }
    total_ticks = (uint64_t)user + (uint64_t)nice + (uint64_t)sys + (uint64_t)idle +
                  (uint64_t)iowait + (uint64_t)irq + (uint64_t)softirq +
                  (uint64_t)steal + (uint64_t)guest + (uint64_t)guest_nice;

    if(prev_total_ticks == 0 || prev_proc_ticks == 0 || total_ticks <= prev_total_ticks) {
        prev_total_ticks = total_ticks;
        prev_proc_ticks = proc_ticks;
        return 0.0f;
    }

    {
        uint64_t dt_total = total_ticks - prev_total_ticks;
        uint64_t dt_proc = (proc_ticks >= prev_proc_ticks) ? (proc_ticks - prev_proc_ticks) : 0;
        float cpu_percent;

        prev_total_ticks = total_ticks;
        prev_proc_ticks = proc_ticks;

        if(dt_total == 0) return 0.0f;

        ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if(ncpu < 1) ncpu = 1;

        cpu_percent = (float)dt_proc * 100.0f / (float)dt_total;
        cpu_percent *= (float)ncpu;
        if(cpu_percent < 0.0f) cpu_percent = 0.0f;
        return cpu_percent;
    }
}

/* 纯整型 YUYV -> RGB565，避免浮点开销 */
static void yuyv_to_rgb565(const uint8_t * yuyv, uint16_t * rgb565, uint32_t pixel_count)
{
    uint32_t i;
    for(i = 0; i + 1 < pixel_count; i += 2) {
        int y0 = (int)yuyv[0];
        int u = (int)yuyv[1] - 128;
        int y1 = (int)yuyv[2];
        int v = (int)yuyv[3] - 128;

        int c0 = y0 - 16;
        int c1 = y1 - 16;
        if(c0 < 0) c0 = 0;
        if(c1 < 0) c1 = 0;

        int r0 = (298 * c0 + 409 * v + 128) >> 8;
        int g0 = (298 * c0 - 100 * u - 208 * v + 128) >> 8;
        int b0 = (298 * c0 + 516 * u + 128) >> 8;

        int r1 = (298 * c1 + 409 * v + 128) >> 8;
        int g1 = (298 * c1 - 100 * u - 208 * v + 128) >> 8;
        int b1 = (298 * c1 + 516 * u + 128) >> 8;

        r0 = clamp_u8(r0);
        g0 = clamp_u8(g0);
        b0 = clamp_u8(b0);

        r1 = clamp_u8(r1);
        g1 = clamp_u8(g1);
        b1 = clamp_u8(b1);

        rgb565[0] = (uint16_t)(((r0 & 0xF8) << 8) | ((g0 & 0xFC) << 3) | (b0 >> 3));
        rgb565[1] = (uint16_t)(((r1 & 0xF8) << 8) | ((g1 & 0xFC) << 3) | (b1 >> 3));

        yuyv += 4;
        rgb565 += 2;
    }
}

static int camera_init(camera_ctx_t * cam, const char * dev)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    uint32_t i;

    cam->fd = open(dev, O_RDWR, 0);
    if(cam->fd < 0) {
        perror("open camera");
        return -1;
    }

    if(xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
       !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device is not V4L2 capture/streaming capable\n");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cam->width;
    fmt.fmt.pix.height = cam->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if(xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    cam->latest_rgb565_bytes = (size_t)cam->width * cam->height * 2;
    cam->latest_rgb565 = malloc(cam->latest_rgb565_bytes);
    if(cam->latest_rgb565 == NULL) {
        perror("malloc latest_rgb565");
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    cam->work_rgb565 = malloc(cam->latest_rgb565_bytes);
    if(cam->work_rgb565 == NULL) {
        perror("malloc work_rgb565");
        free(cam->latest_rgb565);
        cam->latest_rgb565 = NULL;
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    memset(&req, 0, sizeof(req));
    req.count = CAMERA_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        free(cam->latest_rgb565);
        cam->latest_rgb565 = NULL;
        close(cam->fd);
        cam->fd = -1;
        return -1;
    }

    cam->buffer_count = req.count;
    for(i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if(xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buf.m.offset);
        if(cam->buffers[i].start == MAP_FAILED) {
            perror("mmap");
            return -1;
        }
    }

    for(i = 0; i < cam->buffer_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if(xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    return 0;
}

static void camera_deinit(camera_ctx_t * cam)
{
    uint32_t i;

    if(cam->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    }

    for(i = 0; i < cam->buffer_count; i++) {
        if(cam->buffers[i].start && cam->buffers[i].start != MAP_FAILED) {
            munmap(cam->buffers[i].start, cam->buffers[i].length);
            cam->buffers[i].start = NULL;
            cam->buffers[i].length = 0;
        }
    }

    if(cam->fd >= 0) {
        close(cam->fd);
        cam->fd = -1;
    }

    if(cam->latest_rgb565) {
        free(cam->latest_rgb565);
        cam->latest_rgb565 = NULL;
    }
    if(cam->work_rgb565) {
        free(cam->work_rgb565);
        cam->work_rgb565 = NULL;
    }
    cam->buffer_count = 0;
    cam->frame_ready = false;
}

static void * camera_thread_entry(void * arg)
{
    camera_ctx_t * cam = (camera_ctx_t *)arg;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    uint64_t last_convert_us = 0;
    const uint64_t min_interval_us = 1000000ULL / CAMERA_CONVERT_FPS;

    if(xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        cam->running = false;
        return NULL;
    }

    while(cam->running) {
        fd_set fds;
        struct timeval tv;
        int ret;
        struct v4l2_buffer buf;

        FD_ZERO(&fds);
        FD_SET(cam->fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ret = select(cam->fd + 1, &fds, NULL, NULL, &tv);
        if(ret < 0) {
            if(errno == EINTR) continue;
            perror("camera select");
            break;
        }
        if(ret == 0) continue;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if(xioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
            if(errno == EAGAIN) continue;
            perror("VIDIOC_DQBUF");
            break;
        }

        {
            uint64_t now_us = now_us_monotonic();
            if(last_convert_us == 0 || now_us - last_convert_us >= min_interval_us) {
                uint16_t * tmp;

                yuyv_to_rgb565((const uint8_t *)cam->buffers[buf.index].start,
                               cam->work_rgb565,
                               cam->width * cam->height);

                pthread_mutex_lock(&cam->frame_mutex);
                tmp = cam->latest_rgb565;
                cam->latest_rgb565 = cam->work_rgb565;
                cam->work_rgb565 = tmp;
                cam->frame_ready = true;
                cam->frame_seq++;
                cam->converted_frames++;
                cam->last_frame_ready_us = now_us;
                pthread_mutex_unlock(&cam->frame_mutex);

                last_convert_us = now_us;
            }
        }

        if(xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    return NULL;
}

static int camera_start(camera_ctx_t * cam, const char * dev)
{
    cam->running = false;
    cam->frame_ready = false;
    cam->frame_seq = 0;
    cam->last_render_seq = 0;
    cam->converted_frames = 0;
    cam->last_frame_ready_us = 0;
    cam->convert_fps = 0.0f;
    cam->render_fps = 0.0f;
    cam->render_latency_ms = 0.0f;
    cam->process_cpu_percent = 0.0f;
    cam->snapshot_ms = 0.0f;
    if(camera_init(cam, dev) != 0) {
        return -1;
    }

    cam->running = true;
    if(pthread_create(&cam->thread, NULL, camera_thread_entry, cam) != 0) {
        perror("pthread_create camera");
        cam->running = false;
        camera_deinit(cam);
        return -1;
    }

    return 0;
}

static void camera_stop(camera_ctx_t * cam)
{
    if(cam->running) {
        cam->running = false;
        pthread_join(cam->thread, NULL);
    }
    camera_deinit(cam);
}

static int camera_save_snapshot(camera_ctx_t * cam, const char * path)
{
    FILE * fp;
    uint16_t * snapshot_buf;
    size_t snapshot_bytes;

    pthread_mutex_lock(&cam->frame_mutex);
    if(!cam->frame_ready || cam->latest_rgb565 == NULL) {
        pthread_mutex_unlock(&cam->frame_mutex);
        return -1;
    }

    snapshot_bytes = cam->latest_rgb565_bytes;
    snapshot_buf = malloc(snapshot_bytes);
    if(snapshot_buf == NULL) {
        pthread_mutex_unlock(&cam->frame_mutex);
        return -1;
    }
    memcpy(snapshot_buf, cam->latest_rgb565, snapshot_bytes);
    pthread_mutex_unlock(&cam->frame_mutex);

    fp = fopen(path, "wb");
    if(!fp) {
        free(snapshot_buf);
        return -1;
    }

    fwrite(snapshot_buf, 1, snapshot_bytes, fp);
    fclose(fp);
    free(snapshot_buf);

    return 0;
}

/* Internal functions */
static void configure_simulator(int argc, char ** argv);
static void print_lvgl_version(void);
static void print_usage(void);

static char * selected_backend;
extern simulator_settings_t settings;

// ================= 天气获取线程 =================
void * weather_thread_entry(void * arg)
{
    (void)arg; // 消除 unused parameter 警告
    int sockfd;
    struct sockaddr_in servaddr;
    char request[256];

    // 预先构造请求字符串
    snprintf(request, sizeof(request), "GET /weather.json HTTP/1.1\r\nHost: %s\r\n\r\n", SERVER_IP);

    while(1) {
        printf("[Net] Connecting to Weather Server %s...\n", SERVER_IP);

        // 1. 创建 Socket
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("socket creation failed");
            sleep(5); continue;
        }

        // 设置超时 (防止连不上卡死)
        struct timeval timeout;
        timeout.tv_sec = 3; timeout.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        memset(&servaddr, 0, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(SERVER_PORT);
        servaddr.sin_addr.s_addr = inet_addr(SERVER_IP);

        // 2. 连接
        if (connect(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
            perror("Connection failed"); // 如果这里报错，检查电脑防火墙或IP
            close(sockfd);
            sleep(5); continue;
        }

        // 3. 发送 HTTP 请求
        write(sockfd, request, strlen(request));

        // 4. 循环读取响应，避免只读到头
        char buffer[4096];
        int total = 0, n = 0;
        memset(buffer, 0, sizeof(buffer));

        while ((n = recv(sockfd, buffer + total, sizeof(buffer) - 1 - total, 0)) > 0) {
            total += n;
            buffer[total] = '\0';
            if (strchr(buffer, '{')) break; // 已收到 JSON
        }

        printf("[Net] Raw Response (%d bytes):\n%s\n", total, buffer);

        if (total > 0) {
            char *json_body = strchr(buffer, '{');
            if (json_body) {
                printf("[Net] Received JSON: %s\n", json_body);

                cJSON *root = cJSON_Parse(json_body);
                if (root) {
                    cJSON *city = cJSON_GetObjectItem(root, "city");
                    cJSON *weather = cJSON_GetObjectItem(root, "weather");
                    cJSON *temp = cJSON_GetObjectItem(root, "temp");

                    pthread_mutex_lock(&g_weather.mutex);
                    if(city) strncpy(g_weather.city, city->valuestring, sizeof(g_weather.city)-1);
                    if(weather) strncpy(g_weather.weather_str, weather->valuestring, sizeof(g_weather.weather_str)-1);
                    if(temp) g_weather.temperature = temp->valueint;
                    g_weather.is_updated = true;
                    pthread_mutex_unlock(&g_weather.mutex);

                    cJSON_Delete(root);
                }
            } else {
                printf("[Net] No JSON body found.\n");
            }
        }

        close(sockfd);

        // 每 10 秒更新一次天气
        sleep(10);
    }
    return NULL;
}

static void sysfs_write_int(const char * path, int val)
{
    FILE * fp = fopen(path, "w");
    if(fp == NULL){
        // printf("Error Cannot open %s\n", path); // 调试时可打开
        return ;
    }
    fprintf(fp, "%d\n", val);
    fflush(fp); 
    fclose(fp);
}

static float get_cpu_temp(void){
    FILE *fp;
    char temp_str[16];
    float temp_val = 0.0;

    fp = fopen(TEMP_PATH,"r");
    if(fp == NULL){
        return 0.0;
    }
    if(fgets(temp_str, sizeof(temp_str), fp) != NULL){
        temp_val = atof(temp_str) / 1000.0;
    }
    fclose(fp);
    return temp_val;
}

/**
 * @brief 定时器回调函数 - 负责更新 UI
 */
static void temp_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer); 
    
    // 1. 更新 CPU 温度
    float t = get_cpu_temp();
    if(temp_label) lv_label_set_text_fmt(temp_label, "CPU: %.1f C", t);
    if(chart && ser1) lv_chart_set_next_value(chart, ser1, (int)t);

    // 2. 更新网络天气 (检查锁)
    if (pthread_mutex_trylock(&g_weather.mutex) == 0) { // 尝试加锁
        if(g_weather.is_updated && weather_label) {
            printf("[UI] city=%s weather=%s temp=%d\n",
                   g_weather.city, g_weather.weather_str, g_weather.temperature);

            lv_label_set_text_fmt(weather_label, "%s\n%s %d C",
                                  g_weather.city,
                                  g_weather.weather_str,
                                  g_weather.temperature);
            g_weather.is_updated = false;
        }
        pthread_mutex_unlock(&g_weather.mutex);
    }
}

static void led_switch_event_cb(lv_event_t * e)
{
    lv_obj_t * sw = lv_event_get_target(e);
    bool state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    sysfs_write_int(LED_PATH, state ? 1:0);
    printf("LED switched to : %d\n", state);
}

static void brightness_slider_event_cb(lv_event_t * e)
{
    lv_obj_t * slider = lv_event_get_target(e);
    int val = (int)lv_slider_get_value(slider);
    sysfs_write_int(BACKLIGHT_PATH, val);
}

static void camera_refresh_timer_cb(lv_timer_t * timer)
{
    LV_UNUSED(timer);
    static uint64_t last_stat_us = 0;
    static uint32_t prev_conv_frames = 0;
    static uint32_t prev_render_frames = 0;
    static uint32_t render_frames = 0;
    static uint64_t latency_sum_us = 0;
    static uint32_t latency_samples = 0;
    uint64_t now_us;

    if(g_camera.canvas == NULL || g_camera.canvas_buf == NULL) return;

    now_us = now_us_monotonic();

    if(pthread_mutex_trylock(&g_camera.frame_mutex) == 0) {
        if(g_camera.frame_ready && g_camera.latest_rgb565 &&
           g_camera.frame_seq != g_camera.last_render_seq) {
            uint64_t frame_ready_us = g_camera.last_frame_ready_us;
            memcpy(g_camera.canvas_buf, g_camera.latest_rgb565, g_camera.latest_rgb565_bytes);
            g_camera.last_render_seq = g_camera.frame_seq;
            render_frames++;
            if(frame_ready_us > 0 && now_us >= frame_ready_us) {
                latency_sum_us += (now_us - frame_ready_us);
                latency_samples++;
            }
            lv_obj_invalidate(g_camera.canvas);
        }
        pthread_mutex_unlock(&g_camera.frame_mutex);
    }

    if(last_stat_us == 0) {
        last_stat_us = now_us;
        return;
    }

    if(now_us - last_stat_us >= 1000000ULL) {
        float sec = (float)(now_us - last_stat_us) / 1000000.0f;
        uint32_t conv_frames;
        float avg_latency_ms = 0.0f;

        pthread_mutex_lock(&g_camera.frame_mutex);
        conv_frames = g_camera.converted_frames;
        pthread_mutex_unlock(&g_camera.frame_mutex);

        g_camera.convert_fps = (float)(conv_frames - prev_conv_frames) / sec;
        g_camera.render_fps = (float)(render_frames - prev_render_frames) / sec;
        g_camera.process_cpu_percent = read_process_cpu_percent();

        if(latency_samples > 0) {
            avg_latency_ms = (float)latency_sum_us / (float)latency_samples / 1000.0f;
        }
        g_camera.render_latency_ms = avg_latency_ms;

        if(camera_metrics_label) {
            lv_label_set_text_fmt(camera_metrics_label,
                                  "conv %.1f fps  render %.1f fps\n"
                                  "latency %.1f ms  cpu %.1f%%\n"
                                  "snapshot %.1f ms",
                                  g_camera.convert_fps,
                                  g_camera.render_fps,
                                  g_camera.render_latency_ms,
                                  g_camera.process_cpu_percent,
                                  g_camera.snapshot_ms);
        }

        prev_conv_frames = conv_frames;
        prev_render_frames = render_frames;
        last_stat_us = now_us;
        latency_sum_us = 0;
        latency_samples = 0;
    }
}

static void camera_snap_event_cb(lv_event_t * e)
{
    uint64_t t0_us;
    uint64_t t1_us;

    LV_UNUSED(e);
    t0_us = now_us_monotonic();
    if(camera_save_snapshot(&g_camera, "/tmp/cam_snapshot_rgb565.raw") == 0) {
        t1_us = now_us_monotonic();
        g_camera.snapshot_ms = (float)(t1_us - t0_us) / 1000.0f;
        if(camera_status_label) lv_label_set_text(camera_status_label, "Snapshot OK: /tmp/cam_snapshot_rgb565.raw");
    }
    else {
        t1_us = now_us_monotonic();
        g_camera.snapshot_ms = (float)(t1_us - t0_us) / 1000.0f;
        if(camera_status_label) lv_label_set_text(camera_status_label, "Snapshot failed");
    }
}

// 界面构建函数
void create_tab_monitor(lv_obj_t * parent)
{
    // 左侧：CPU 温度
    temp_label = lv_label_create(parent);
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_28, 0); 
    lv_label_set_text(temp_label, "CPU: --.- C");
    lv_obj_align(temp_label, LV_ALIGN_TOP_LEFT, 10, 10);

    // 右侧：网络天气（先用英文内置字体排除字体问题）
    weather_label = lv_label_create(parent);
    lv_obj_set_style_text_font(weather_label, &lv_font_montserrat_16, 0); // 临时替换
    lv_label_set_text(weather_label, "Waiting...");
    lv_obj_align(weather_label, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_text_align(weather_label, LV_TEXT_ALIGN_RIGHT, 0);

    // 下方：图表
    chart = lv_chart_create(parent);
    lv_obj_set_size(chart, 400, 140);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 20, 80);
    lv_chart_set_point_count(chart, 20);
    ser1 = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_RED), LV_CHART_AXIS_PRIMARY_Y);
}

void create_tab_control(lv_obj_t * parent)
{
    // LED
    lv_obj_t * label_led = lv_label_create(parent);
    lv_label_set_text(label_led, "System LED");
    lv_obj_align(label_led, LV_ALIGN_TOP_LEFT, 20, 20);

    lv_obj_t * sw = lv_switch_create(parent);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -40, 10);
    lv_obj_set_size(sw, 60, 30);
    lv_obj_add_event_cb(sw, led_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // Backlight
    lv_obj_t * label_bl = lv_label_create(parent);
    lv_label_set_text(label_bl, "Brightness");
    lv_obj_align(label_bl, LV_ALIGN_LEFT_MID, 20, 20);

    lv_obj_t * slider = lv_slider_create(parent);
    lv_obj_set_size(slider, 200, 15);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, 20);
    lv_slider_set_range(slider, 1, 7); 
    lv_slider_set_value(slider, 5, LV_ANIM_OFF); 
    lv_obj_add_event_cb(slider, brightness_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(slider, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
}

void create_tab_camera(lv_obj_t * parent)
{
    lv_obj_t * snap_btn;
    lv_obj_t * btn_label;

    camera_status_label = lv_label_create(parent);
    lv_label_set_text(camera_status_label, "Camera: initializing...");
    lv_obj_align(camera_status_label, LV_ALIGN_TOP_LEFT, 10, 10);

    camera_metrics_label = lv_label_create(parent);
    lv_label_set_text(camera_metrics_label,
                      "conv --.- fps  render --.- fps\n"
                      "latency --.- ms  cpu --.-%\n"
                      "snapshot --.- ms");
    lv_obj_align(camera_metrics_label, LV_ALIGN_TOP_LEFT, 10, 34);

    g_camera.canvas = lv_canvas_create(parent);
    lv_obj_set_size(g_camera.canvas, g_camera.width, g_camera.height);
    lv_obj_align(g_camera.canvas, LV_ALIGN_BOTTOM_MID, 0, -10);

    g_camera.canvas_buf = malloc((size_t)g_camera.width * g_camera.height * 2);
    if(g_camera.canvas_buf) {
        lv_canvas_set_buffer(g_camera.canvas, g_camera.canvas_buf,
                             g_camera.width, g_camera.height,
                             LV_COLOR_FORMAT_RGB565);
        lv_canvas_fill_bg(g_camera.canvas, lv_color_hex(0x202020), LV_OPA_COVER);
    }
    else {
        lv_label_set_text(camera_status_label, "Camera: no canvas memory");
    }

    snap_btn = lv_button_create(parent);
    lv_obj_align(snap_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_add_event_cb(snap_btn, camera_snap_event_cb, LV_EVENT_CLICKED, NULL);

    btn_label = lv_label_create(snap_btn);
    lv_label_set_text(btn_label, "Snapshot");
    lv_obj_center(btn_label);
}

static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n",
            LVGL_VERSION_MAJOR,
            LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH,
            LVGL_VERSION_INFO);
}

static void print_usage(void)
{
    fprintf(stdout, "\nlvglsim ... \n");
}

static void configure_simulator(int argc, char ** argv)
{
    // ... 原有的配置逻辑，稍微简化 ...
    int opt = 0;
    selected_backend = NULL;
    driver_backends_register();
    
    // 默认值
    settings.window_width = 800;
    settings.window_height = 480;

    // 简单解析，保留必要功能
    while((opt = getopt(argc, argv, "b:fmW:H:BVh")) != -1) {
        switch(opt) {
            case 'f': settings.fullscreen = true; break;
            case 'W': settings.window_width = atoi(optarg); break;
            case 'H': settings.window_height = atoi(optarg); break;
            case 'b':
                if(driver_backends_is_supported(optarg) == 0) {
                    die("error no such backend: %s\n", optarg);
                }
                selected_backend = strdup(optarg);
                break;
            case 'm':
                settings.maximize = true;
                break;
            case 'V':
                print_lvgl_version();
                exit(EXIT_SUCCESS);
                break;
            case 'B':
                driver_backends_print_supported();
                exit(EXIT_SUCCESS);
                break;
            case 'h':
                print_usage();
                exit(EXIT_SUCCESS);
                break;
            case ':':
                print_usage();
                die("Option -%c requires an argument.\n", optopt);
                break;
            case '?':
                print_usage();
                die("Unknown option -%c.\n", optopt);
        }
    }
}

int main(int argc, char ** argv)
{
    configure_simulator(argc, argv);

    /* Initialize LVGL. */
    lv_init();
    
    // 初始化互斥锁
    pthread_mutex_init(&g_weather.mutex, NULL);
    pthread_mutex_init(&g_camera.frame_mutex, NULL);

    // 启动网络线程
    pthread_t tid;
    pthread_create(&tid, NULL, weather_thread_entry, NULL);

    // 驱动
    lv_display_t * disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");

    lv_indev_t * touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");
    lv_indev_set_display(touch, disp);

    // 创建多页面容器
    lv_obj_t * tabview = lv_tabview_create(lv_scr_act());
    lv_tabview_set_tab_bar_position(tabview, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tabview, 40);

    // 两个页面
    lv_obj_t * tab1 = lv_tabview_add_tab(tabview, "Monitor");
    lv_obj_t * tab2 = lv_tabview_add_tab(tabview, "Control");
    lv_obj_t * tab3 = lv_tabview_add_tab(tabview, "Camera");

    create_tab_monitor(tab1);
    create_tab_control(tab2);
    create_tab_camera(tab3);

    if(camera_start(&g_camera, CAMERA_DEV) == 0) {
        lv_label_set_text(camera_status_label, "Camera: /dev/video1 (YUYV)");
        lv_timer_create(camera_refresh_timer_cb, 66, NULL);
        printf("[Camera] started %s %ux%u\n", CAMERA_DEV, g_camera.width, g_camera.height);
    }
    else {
        lv_label_set_text(camera_status_label, "Camera: start failed");
        printf("[Camera] start failed on %s\n", CAMERA_DEV);
    }

    lv_timer_create(temp_timer_cb, 1000, NULL);
    printf("V3.0 HMI Started.\n");

    while(1) {
        lv_timer_handler();
        usleep(5000); 
    }

    camera_stop(&g_camera);
    if(g_camera.canvas_buf) free(g_camera.canvas_buf);
    pthread_mutex_destroy(&g_camera.frame_mutex);
    pthread_mutex_destroy(&g_weather.mutex);

    return 0;
}
