#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/video1"
#define DEFAULT_PORT 8081
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 480
#define BUFFER_COUNT 4

typedef struct {
    void *start;
    size_t length;
} v4l2_buffer_t;

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    v4l2_buffer_t bufs[BUFFER_COUNT];
    uint32_t buf_count;
} v4l2_ctx_t;

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, req, arg);
    } while(ret == -1 && errno == EINTR);
    return ret;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while(len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if(n < 0) {
            if(errno == EINTR) {
                continue;
            }
            return -1;
        }
        if(n == 0) {
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int v4l2_open_and_init(v4l2_ctx_t *ctx, const char *dev)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;

    ctx->fd = open(dev, O_RDWR | O_NONBLOCK, 0);
    if(ctx->fd < 0) {
        perror("open camera");
        return -1;
    }

    if(xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }

    if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
       !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "device is not capture/streaming capable\n");
        return -1;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = ctx->width;
    fmt.fmt.pix.height = ctx->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if(xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT MJPEG");
        return -1;
    }

    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if(xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    ctx->buf_count = req.count;
    if(ctx->buf_count < 2) {
        fprintf(stderr, "insufficient V4L2 buffers\n");
        return -1;
    }

    for(uint32_t i = 0; i < ctx->buf_count; ++i) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;

        if(xioctl(ctx->fd, VIDIOC_QUERYBUF, &b) < 0) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        ctx->bufs[i].length = b.length;
        ctx->bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, b.m.offset);
        if(ctx->bufs[i].start == MAP_FAILED) {
            perror("mmap");
            return -1;
        }
    }

    for(uint32_t i = 0; i < ctx->buf_count; ++i) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;

        if(xioctl(ctx->fd, VIDIOC_QBUF, &b) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    return 0;
}

static int v4l2_stream_on(v4l2_ctx_t *ctx)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if(xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        return -1;
    }
    return 0;
}

static void v4l2_close_all(v4l2_ctx_t *ctx)
{
    if(ctx->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }

    for(uint32_t i = 0; i < ctx->buf_count; ++i) {
        if(ctx->bufs[i].start && ctx->bufs[i].start != MAP_FAILED) {
            munmap(ctx->bufs[i].start, ctx->bufs[i].length);
            ctx->bufs[i].start = NULL;
            ctx->bufs[i].length = 0;
        }
    }

    if(ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

static int create_server_socket(int port)
{
    int fd;
    int on = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        perror("socket");
        return -1;
    }

    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

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

static int send_mjpeg_header(int client_fd)
{
    static const char *hdr =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

    return send_all(client_fd, hdr, strlen(hdr));
}

static int stream_to_client(v4l2_ctx_t *ctx, int client_fd)
{
    if(send_mjpeg_header(client_fd) != 0) {
        return -1;
    }

    while(g_running) {
        fd_set fds;
        struct timeval tv;
        struct v4l2_buffer b;
        int ret;

        FD_ZERO(&fds);
        FD_SET(ctx->fd, &fds);

        tv.tv_sec = 2;
        tv.tv_usec = 0;

        ret = select(ctx->fd + 1, &fds, NULL, NULL, &tv);
        if(ret < 0) {
            if(errno == EINTR) {
                continue;
            }
            perror("select");
            return -1;
        }
        if(ret == 0) {
            continue;
        }

        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;

        if(xioctl(ctx->fd, VIDIOC_DQBUF, &b) < 0) {
            if(errno == EAGAIN) {
                continue;
            }
            perror("VIDIOC_DQBUF");
            return -1;
        }

        {
            char part_hdr[128];
            int n = snprintf(part_hdr, sizeof(part_hdr),
                             "--frame\r\n"
                             "Content-Type: image/jpeg\r\n"
                             "Content-Length: %u\r\n\r\n",
                             b.bytesused);
            if(n <= 0 || send_all(client_fd, part_hdr, (size_t)n) != 0 ||
               send_all(client_fd, ctx->bufs[b.index].start, b.bytesused) != 0 ||
               send_all(client_fd, "\r\n", 2) != 0) {
                xioctl(ctx->fd, VIDIOC_QBUF, &b);
                return -1;
            }
        }

        if(xioctl(ctx->fd, VIDIOC_QBUF, &b) < 0) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-d /dev/videoX] [-p port] [-W width] [-H height]\n"
            "Example: %s -d /dev/video1 -p 8081 -W 640 -H 480\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    const char *dev = DEFAULT_DEVICE;
    int port = DEFAULT_PORT;
    v4l2_ctx_t ctx;
    int server_fd = -1;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fd = -1;
    ctx.width = DEFAULT_WIDTH;
    ctx.height = DEFAULT_HEIGHT;

    for(int i = 1; i < argc; ++i) {
        if(strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dev = argv[++i];
        } else if(strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if(strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            ctx.width = (uint32_t)atoi(argv[++i]);
        } else if(strcmp(argv[i], "-H") == 0 && i + 1 < argc) {
            ctx.height = (uint32_t)atoi(argv[++i]);
        } else if(strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    if(v4l2_open_and_init(&ctx, dev) != 0) {
        v4l2_close_all(&ctx);
        return 1;
    }

    if(v4l2_stream_on(&ctx) != 0) {
        v4l2_close_all(&ctx);
        return 1;
    }

    server_fd = create_server_socket(port);
    if(server_fd < 0) {
        v4l2_close_all(&ctx);
        return 1;
    }

    printf("[mjpeg_streamer] camera=%s %ux%u port=%d\n", dev, ctx.width, ctx.height, port);
    printf("[mjpeg_streamer] open http://<board-ip>:%d in browser or VLC\n", port);

    while(g_running) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli, &cli_len);
        if(client_fd < 0) {
            if(errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        printf("[mjpeg_streamer] client connected: %s\n", inet_ntoa(cli.sin_addr));
        (void)stream_to_client(&ctx, client_fd);
        close(client_fd);
        printf("[mjpeg_streamer] client disconnected\n");
    }

    if(server_fd >= 0) {
        close(server_fd);
    }
    v4l2_close_all(&ctx);

    return 0;
}
