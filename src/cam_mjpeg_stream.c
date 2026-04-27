#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/videodev2.h>

#define DEFAULT_DEV "/dev/video1"
#define DEFAULT_PORT 8080
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define BUFFER_COUNT 4
#define BOUNDARY "frame"

typedef struct {
    void *start;
    size_t length;
} mmap_buf_t;

typedef struct {
    int fd;
    mmap_buf_t bufs[BUFFER_COUNT];
    uint32_t buf_count;
    uint32_t width;
    uint32_t height;
} camera_t;

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while(ret == -1 && errno == EINTR);
    return ret;
}

static int send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while(sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if(n <= 0) {
            if(errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int camera_init(camera_t *cam, const char *dev)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    uint32_t i;

    memset(cam, 0, sizeof(*cam));
    cam->fd = open(dev, O_RDWR);
    if(cam->fd < 0) {
        perror("open camera");
        return -1;
    }

    if(xioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }

    if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
       !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "camera is not capture/streaming capable\n");
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = DEFAULT_WIDTH;
    fmt.fmt.pix.height = DEFAULT_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if(xioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        return -1;
    }

    if(fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "camera does not output MJPEG after S_FMT\n");
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(xioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    cam->buf_count = req.count;
    for(i = 0; i < cam->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if(xioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        cam->bufs[i].length = buf.length;
        cam->bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, cam->fd, buf.m.offset);
        if(cam->bufs[i].start == MAP_FAILED) {
            perror("mmap");
            return -1;
        }
    }

    for(i = 0; i < cam->buf_count; i++) {
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

static void camera_deinit(camera_t *cam)
{
    uint32_t i;
    if(cam->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(cam->fd, VIDIOC_STREAMOFF, &type);
    }

    for(i = 0; i < cam->buf_count; i++) {
        if(cam->bufs[i].start && cam->bufs[i].start != MAP_FAILED) {
            munmap(cam->bufs[i].start, cam->bufs[i].length);
        }
    }

    if(cam->fd >= 0) close(cam->fd);
    memset(cam, 0, sizeof(*cam));
    cam->fd = -1;
}

static int camera_stream_on(camera_t *cam)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(xioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}

static int capture_one(camera_t *cam, const uint8_t **frame, size_t *frame_len, struct v4l2_buffer *out_buf)
{
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    int ret = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if(ret <= 0) {
        if(ret == 0) fprintf(stderr, "capture timeout\n");
        else perror("select");
        return -1;
    }

    memset(out_buf, 0, sizeof(*out_buf));
    out_buf->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out_buf->memory = V4L2_MEMORY_MMAP;

    if(xioctl(cam->fd, VIDIOC_DQBUF, out_buf) < 0) {
        if(errno == EAGAIN) return 1;
        perror("VIDIOC_DQBUF");
        return -1;
    }

    if(out_buf->index >= cam->buf_count) {
        fprintf(stderr, "invalid buffer index\n");
        return -1;
    }

    *frame = (const uint8_t *)cam->bufs[out_buf->index].start;
    *frame_len = out_buf->bytesused;
    return 0;
}

static int create_server_socket(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        perror("socket");
        return -1;
    }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if(listen(fd, 4) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

static int stream_client(camera_t *cam, int client_fd)
{
    const char *resp =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=" BOUNDARY "\r\n\r\n";

    if(send_all(client_fd, resp, strlen(resp)) < 0) {
        return -1;
    }

    while(1) {
        const uint8_t *frame = NULL;
        size_t frame_len = 0;
        struct v4l2_buffer buf;
        char header[256];

        int ret = capture_one(cam, &frame, &frame_len, &buf);
        if(ret < 0) return -1;
        if(ret > 0) continue;

        int header_len = snprintf(header, sizeof(header),
                                  "--" BOUNDARY "\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %zu\r\n\r\n",
                                  frame_len);

        if(header_len <= 0 || (size_t)header_len >= sizeof(header) ||
           send_all(client_fd, header, (size_t)header_len) < 0 ||
           send_all(client_fd, frame, frame_len) < 0 ||
           send_all(client_fd, "\r\n", 2) < 0) {
            xioctl(cam->fd, VIDIOC_QBUF, &buf);
            return -1;
        }

        if(xioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }
}

int main(int argc, char **argv)
{
    const char *dev = DEFAULT_DEV;
    uint16_t port = DEFAULT_PORT;
    camera_t cam;
    int server_fd;

    if(argc >= 2) dev = argv[1];
    if(argc >= 3) {
        int p = atoi(argv[2]);
        if(p > 0 && p < 65536) port = (uint16_t)p;
    }

    signal(SIGPIPE, SIG_IGN);

    cam.fd = -1;
    if(camera_init(&cam, dev) != 0) {
        camera_deinit(&cam);
        return 1;
    }

    if(camera_stream_on(&cam) != 0) {
        camera_deinit(&cam);
        return 1;
    }

    server_fd = create_server_socket(port);
    if(server_fd < 0) {
        camera_deinit(&cam);
        return 1;
    }

    printf("[mjpeg] listening on 0.0.0.0:%u, camera=%s, %ux%u MJPEG\n", port, dev, cam.width, cam.height);
    printf("[mjpeg] open http://<board-ip>:%u/ on PC browser or VLC\n", port);

    while(1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = accept(server_fd, (struct sockaddr *)&cli, &len);
        if(cfd < 0) {
            if(errno == EINTR) continue;
            perror("accept");
            break;
        }

        printf("[mjpeg] client %s:%u connected\n", inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
        (void)stream_client(&cam, cfd);
        close(cfd);
        printf("[mjpeg] client disconnected\n");
    }

    close(server_fd);
    camera_deinit(&cam);
    return 0;
}
