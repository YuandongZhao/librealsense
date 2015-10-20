#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <functional>

#include <GLFW/glfw3.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <linux/videodev2.h>

#include <libusb.h>

static int xioctl(int fh, int request, void *arg)
{
    int r;
    do {
        r = ioctl(fh, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

struct buffer { void * start; size_t length; };
#pragma pack(push, 1)
struct z16y8_pixel { uint16_t z; uint8_t y; };
#pragma pack(pop)

void throw_error(const char * s)
{
    std::ostringstream ss;
    ss << s << " error " << errno << ", " << strerror(errno);
    throw std::runtime_error(ss.str());
}

void warn_error(const char * s)
{
    std::cerr << s << " error " << errno << ", " << strerror(errno) << std::endl;
}

class subdevice
{
    std::string dev_name;
    int vid, pid, mi;
    int fd;
    std::vector<buffer> buffers;
    std::function<void(const void *, size_t)> callback;
public:
    subdevice(const std::string & name) : dev_name("/dev/" + name), fd()
    {
        struct stat st;
        if(stat(dev_name.c_str(), &st) < 0)
        {
            std::ostringstream ss; ss << "Cannot identify '" << dev_name << "': " << errno << ", " << strerror(errno);
            throw std::runtime_error(ss.str());
        }
        if(!S_ISCHR(st.st_mode)) throw std::runtime_error(dev_name + " is no device");

        std::string modalias;
        if(!(std::ifstream("/sys/class/video4linux/" + name + "/device/modalias") >> modalias))
            throw std::runtime_error("Failed to read modalias");
        if(modalias.size() < 14 || modalias.substr(0,5) != "usb:v" || modalias[9] != 'p')
            throw std::runtime_error("Not a usb format modalias");
        if(!(std::istringstream(modalias.substr(5,4)) >> std::hex >> vid))
            throw std::runtime_error("Failed to read vendor ID");
        if(!(std::istringstream(modalias.substr(10,4)) >> std::hex >> pid))
            throw std::runtime_error("Failed to read product ID");
        if(!(std::ifstream("/sys/class/video4linux/" + name + "/device/bInterfaceNumber") >> std::hex >> mi))
            throw std::runtime_error("Failed to read interface number");

        std::cout << dev_name << " has vendor id " << std::hex << vid << std::endl;
        std::cout << dev_name << " has product id " << std::hex << pid << std::endl;
        std::cout << dev_name << " provides interface number " << std::dec << mi << std::endl;

        fd = open(dev_name.c_str(), O_RDWR | O_NONBLOCK, 0);
        if(fd < 0)
        {
            std::ostringstream ss; ss << "Cannot open '" << dev_name << "': " << errno << ", " << strerror(errno);
            throw std::runtime_error(ss.str());
        }

        v4l2_capability cap = {};
        if(xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
        {
            if(errno == EINVAL) throw std::runtime_error(dev_name + " is no V4L2 device");
            else throw_error("VIDIOC_QUERYCAP");
        }
        if(!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) throw std::runtime_error(dev_name + " is no video capture device");
        if(!(cap.capabilities & V4L2_CAP_STREAMING)) throw std::runtime_error(dev_name + " does not support streaming I/O");

        // Select video input, video standard and tune here.
        v4l2_cropcap cropcap = {};
        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(xioctl(fd, VIDIOC_CROPCAP, &cropcap) == 0)
        {
            v4l2_crop crop = {};
            crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            crop.c = cropcap.defrect; // reset to default
            if(xioctl(fd, VIDIOC_S_CROP, &crop) < 0)
            {
                switch (errno)
                {
                case EINVAL: break; // Cropping not supported
                default: break; // Errors ignored
                }
            }
        } else {} // Errors ignored
    }

    ~subdevice()
    {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        // Will warn for subdev fds that are not streaming
        if(xioctl(fd, VIDIOC_STREAMOFF, &type) < 0) warn_error("VIDIOC_STREAMOFF");

        for(int i = 0; i < buffers.size(); i++)
        {
            if(munmap(buffers[i].start, buffers[i].length) < 0) warn_error("munmap");
        }

        // Close memory mapped IO
        struct v4l2_requestbuffers req = {};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if(xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
        {
            if(errno == EINVAL) throw std::runtime_error(dev_name + " does not support memory mapping");
            else throw_error("VIDIOC_REQBUFS");
        }

        std::cout << "Closing... " << fd << std::endl;
        if(close(fd) < 0) warn_error("close");
    }

    int get_vid() const { return vid; }
    int get_pid() const { return pid; }

    void get_control(int control, void * data, size_t size)
    {
        uvc_xu_control_query q = {2, control, UVC_GET_CUR, size, reinterpret_cast<uint8_t *>(data)};
        if(xioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0) throw_error("UVCIOC_CTRL_QUERY:UVC_GET_CUR");
    }

    void set_control(int control, void * data, size_t size)
    {
       uvc_xu_control_query q = {2, control, UVC_SET_CUR, size, reinterpret_cast<uint8_t *>(data)};
       if(xioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0) throw_error("UVCIOC_CTRL_QUERY:UVC_SET_CUR");
    }

    void start_capture(int width, int height, int fourcc, std::function<void(const void * data, size_t size)> callback)
    {
        v4l2_format fmt = {};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width       = width;
        fmt.fmt.pix.height      = height;
        fmt.fmt.pix.pixelformat = fourcc;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
        if(xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) throw_error("VIDIOC_S_FMT");
        // Note VIDIOC_S_FMT may change width and height

        // Init memory mapped IO
        v4l2_requestbuffers req = {};
        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if(xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
        {
            if(errno == EINVAL) throw std::runtime_error(dev_name + " does not support memory mapping");
            else throw_error("VIDIOC_REQBUFS");
        }
        if(req.count < 2)
        {
            throw std::runtime_error("Insufficient buffer memory on " + dev_name);
        }

        buffers.resize(req.count);
        for(int i=0; i<buffers.size(); ++i)
        {
            v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if(xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) throw_error("VIDIOC_QUERYBUF");

            buffers[i].length = buf.length;
            buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
            if(buffers[i].start == MAP_FAILED) throw_error("mmap");
        }

        // Start capturing
        for(int i = 0; i < buffers.size(); ++i)
        {
            v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if(xioctl(fd, VIDIOC_QBUF, &buf) < 0) throw_error("VIDIOC_QBUF");
        }

        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if(xioctl(fd, VIDIOC_STREAMON, &type) < 0) throw_error("VIDIOC_STREAMON");

        this->callback = callback;
    }

    static void poll(const std::vector<subdevice *> & subdevices)
    {
        int max_fd = 0;
        fd_set fds;
        FD_ZERO(&fds);
        for(auto * sub : subdevices)
        {
            FD_SET(sub->fd, &fds);
            max_fd = std::max(max_fd, sub->fd);
        }

        struct timeval tv = {};
        if(select(max_fd + 1, &fds, NULL, NULL, &tv) < 0)
        {
            if (errno == EINTR) return;
            throw_error("select");
        }

        for(auto * sub : subdevices)
        {
            if(FD_ISSET(sub->fd, &fds))
            {
                v4l2_buffer buf = {};
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                if(xioctl(sub->fd, VIDIOC_DQBUF, &buf) < 0)
                {
                    if(errno == EAGAIN) return;
                    throw_error("VIDIOC_DQBUF");
                }
                assert(buf.index < sub->buffers.size());

                sub->callback(sub->buffers[buf.index].start, buf.bytesused);

                if(xioctl(sub->fd, VIDIOC_QBUF, &buf) < 0) throw_error("VIDIOC_QBUF");
            }
        }
    }
};

class texture
{
    GLuint name = 0;
    int width, height;
public:
    void upload(int width, int height, GLenum format, GLenum type, const void * data)
    {
        if(!name)
        {
            glGenTextures(1, &name);
            glBindTexture(GL_TEXTURE_2D, name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        }
        glBindTexture(GL_TEXTURE_2D, name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, format, type, data);
        glBindTexture(GL_TEXTURE_2D, 0);
        this->width = width;
        this->height = height;
    }

    void draw(int x, int y) const
    {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, name);
        glBegin(GL_QUADS);
        glTexCoord2f(0,0); glVertex2i(x,y);
        glTexCoord2f(1,0); glVertex2i(x+width,y);
        glTexCoord2f(1,1); glVertex2i(x+width,y+height);
        glTexCoord2f(0,1); glVertex2i(x,y+height);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
    }
};

bool check_usb(const char * call, int code)
{
    if(code < 0)
    {
        std::cout << "\n" << call << "(...) returned " << std::dec << code << " (" << libusb_error_name(code) << ")" << std::endl;
        return false;
    }
    return true;
}

#define CHECK(C, ...) check_usb(#C, C(__VA_ARGS__))

#include <iomanip>

int main(int argc, char * argv[])
{
    libusb_context * ctx;
    if(!CHECK(libusb_init, &ctx)) return EXIT_FAILURE;

    libusb_device ** devices;
    if(!CHECK(libusb_get_device_list, ctx, &devices)) return EXIT_FAILURE;

    for(int i=0; devices[i]; ++i)
    {
        auto dev = devices[i];
        libusb_device_descriptor desc;

        if(!CHECK(libusb_get_device_descriptor, dev, &desc)) continue;
        if(desc.idVendor != 0x8086) continue;

        libusb_device_handle * handle;
        if(!CHECK(libusb_open, dev, &handle)) continue;

        unsigned char buffer[1024];
        if(!CHECK(libusb_get_string_descriptor_ascii, handle, desc.iSerialNumber, buffer, 1024)) continue;
        std::cout << std::hex << desc.idVendor << ":" << desc.idProduct;
        std::cout << ":" << buffer << std::endl;

        libusb_close(handle);
    }
    libusb_free_device_list(devices, 1);

    std::vector<std::unique_ptr<subdevice>> subdevices;

    DIR * dir = opendir("/sys/class/video4linux");
    if(!dir) throw std::runtime_error("Cannot access /sys/class/video4linux");
    while (dirent * entry = readdir(dir))
    {
        std::string name = entry->d_name;
        if(name == "." || name == "..") continue;
        std::unique_ptr<subdevice> sub(new subdevice(name));
        subdevices.push_back(move(sub));
    }
    closedir(dir);

    texture texColor, texDepth;

    std::vector<subdevice *> devs;
    if(subdevices.size() >= 2 && subdevices[0]->get_vid() == 0x8086 && subdevices[0]->get_pid() == 0xa66)
    {
        std::cout << "F200 detected!" << std::endl;
        subdevices[0]->start_capture(640, 480, V4L2_PIX_FMT_YUYV, [&](const void * data, size_t size)
        {
            texColor.upload(640, 480, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, data);
        });
        subdevices[1]->start_capture(640, 480, v4l2_fourcc('I','N','V','R'), [&](const void * data, size_t size)
        {
            texDepth.upload(640, 480, GL_LUMINANCE, GL_UNSIGNED_SHORT, data);
        });
        devs = {subdevices[0].get(), subdevices[1].get()};
    }
    else if(subdevices.size() >= 3 && subdevices[0]->get_vid() == 0x8086 && subdevices[0]->get_pid() == 0xa80)
    {
        std::cout << "R200 detected!" << std::endl;

        uint8_t intent = 5;// STATUS_BIT_Z_STREAMING | STATUS_BIT_WEB_STREAMING;
        subdevices[0].get()->set_control(3, &intent, sizeof(uint8_t));

        subdevices[1]->start_capture(628, 469, v4l2_fourcc('Z','1','6',' '), [&](const void * data, size_t size)
        {
            glPixelTransferf(GL_RED_SCALE, 64.0f);
            texDepth.upload(628, 469, GL_LUMINANCE, GL_UNSIGNED_SHORT, data);
            glPixelTransferf(GL_RED_SCALE, 1.0f);
        });
        subdevices[2]->start_capture(640, 480, V4L2_PIX_FMT_YUYV, [&](const void * data, size_t size)
        {
            texColor.upload(640, 480, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, data);
        });

        //devs = {subdevices[1].get()};
        devs = {subdevices[1].get(), subdevices[2].get()};

        //const int STATUS_BIT_Z_STREAMING = 1 << 0;
        //const int STATUS_BIT_LR_STREAMING = 1 << 1;
        //const int STATUS_BIT_WEB_STREAMING = 1 << 2;
        //const int CONTROL_STREAM_INTENT = 3;


    }
    else if(!subdevices.empty())
    {
        std::cout << "Unknown V4L2 device detected, vid=0x" << std::hex << subdevices[0]->get_vid() << ", pid=0x" << subdevices[0]->get_pid() << std::endl;
    }

    // Open a GLFW window
    glfwInit();
    GLFWwindow * win = glfwCreateWindow(1280, 480, "V4L2 test", 0, 0);
    glfwMakeContextCurrent(win);

    int frameCount = 0;
    // While window is open
    while (!glfwWindowShouldClose(win))
    {
        glfwPollEvents();

        subdevice::poll(devs);

        int w,h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        glPushMatrix();
        glfwGetWindowSize(win, &w, &h);
        glOrtho(0, w, h, 0, -1, +1);

        texColor.draw(0, 0);
        texDepth.draw(628, 0);

        glPopMatrix();
        glfwSwapBuffers(win);
        frameCount++;
    }

    glfwDestroyWindow(win);
    glfwTerminate();

    return EXIT_SUCCESS;
}
