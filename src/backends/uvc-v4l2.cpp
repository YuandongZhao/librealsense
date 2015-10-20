#include "../uvc.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <string>
#include <sstream>
#include <fstream>
#include <thread>

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

// debug
#include <iostream>

namespace rsimpl
{
    namespace uvc
    {
        static void throw_error(const char * s)
        {
            std::ostringstream ss;
            ss << s << " error " << errno << ", " << strerror(errno);
            throw std::runtime_error(ss.str());
        }

        static void warn_error(const char * s)
        {
            std::cerr << s << " error " << errno << ", " << strerror(errno) << std::endl;
        }

        static int xioctl(int fh, int request, void *arg)
        {
            int r;
            do {
                r = ioctl(fh, request, arg);
            } while (r < 0 && errno == EINTR);
            return r;
        }

        struct buffer { void * start; size_t length; };

        struct context
        {
            // TODO: V4L2-specific information

            context() {} // TODO: Init
            ~context() {} // TODO: Cleanup
        };

        struct subdevice
        {
            std::string dev_name;
            int vid, pid, mi;
            int fd;
            std::vector<buffer> buffers;

            int width, height, format, fps;
            std::function<void(const void *)> callback;

            subdevice(const std::string & name) : dev_name("/dev/" + name), vid(), pid(), fd(), width(), height(), format()
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
            int get_mi() const { return mi; }

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

            void set_format(int width, int height, int fourcc, int fps, std::function<void(const void * data)> callback)
            {
                this->width = width;
                this->height = height;
                this->format = fourcc;
                this->fps = fps;
                this->callback = callback;
            }

            void start_capture()
            {
                v4l2_format fmt = {};
                fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                fmt.fmt.pix.width       = width;
                fmt.fmt.pix.height      = height;
                fmt.fmt.pix.pixelformat = format;
                fmt.fmt.pix.field       = V4L2_FIELD_NONE;
                if(xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) throw_error("VIDIOC_S_FMT");

                v4l2_streamparm parm = {};
                parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if(xioctl(fd, VIDIOC_G_PARM, &parm) < 0) throw_error("VIDIOC_G_PARM");
                parm.parm.capture.timeperframe.numerator = 1;
                parm.parm.capture.timeperframe.denominator = fps;
                if(xioctl(fd, VIDIOC_S_PARM, &parm) < 0) throw_error("VIDIOC_S_PARM");

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

                struct timeval tv = {0,10000};
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

                        std::cout << sub->fd << " - " << buf.bytesused << std::endl;
                        if(sub->fd == 5)
                        {
                            int x = 9;
                        }
                        sub->callback(sub->buffers[buf.index].start);

                        if(xioctl(sub->fd, VIDIOC_QBUF, &buf) < 0) throw_error("VIDIOC_QBUF");
                    }
                }
            }
        };

        struct device
        {
            const std::shared_ptr<context> parent;
            std::vector<std::unique_ptr<subdevice>> subdevices;
            std::thread thread;
            volatile bool stop;

            libusb_device * usb_device;
            libusb_device_handle * usb_handle;
            std::vector<int> claimed_interfaces;

            device(std::shared_ptr<context> parent) : parent(parent), stop(), usb_device(), usb_handle() {} // TODO: Init
            ~device()
            {
                if(thread.joinable())
                {
                    stop = true;
                    thread.join();
                }

                for(auto interface_number : claimed_interfaces)
                {
                    int status = libusb_release_interface(usb_handle, interface_number);
                    if(status < 0) DEBUG_ERR("libusb_release_interface(...) returned " << libusb_error_name(status));
                }
                if(usb_handle) libusb_close(usb_handle);
                if(usb_device) libusb_unref_device(usb_device);
            }

            bool has_mi(int mi) const
            {
                for(auto & sub : subdevices)
                {
                    if(sub->get_mi() == mi) return true;
                }
                return false;
            }
        };

        ////////////
        // device //
        ////////////

        int get_vendor_id(const device & device) { return device.subdevices[0]->get_vid(); }
        int get_product_id(const device & device) { return device.subdevices[0]->get_pid(); }

        void init_controls(device & device, int subdevice, const guid & xu_guid) {}
        void get_control(const device & device, int subdevice, uint8_t ctrl, void * data, int len)
        {
            device.subdevices[subdevice]->get_control(ctrl, data, len);
        }
        void set_control(device & device, int subdevice, uint8_t ctrl, void * data, int len)
        {
            device.subdevices[subdevice]->set_control(ctrl, data, len);
        }

        void claim_interface(device & device, const guid & interface_guid, int interface_number)
        {
            int status = libusb_claim_interface(device.usb_handle, interface_number);
            if(status < 0) throw std::runtime_error(to_string() << "libusb_claim_interface(...) returned " << libusb_error_name(status));
            device.claimed_interfaces.push_back(interface_number);
        }

        void bulk_transfer(device & device, unsigned char endpoint, void * data, int length, int *actual_length, unsigned int timeout)
        {
            int status = libusb_bulk_transfer(device.usb_handle, endpoint, (unsigned char *)data, length, actual_length, timeout);
            if(status < 0) throw std::runtime_error(to_string() << "libusb_bulk_transfer(...) returned " << libusb_error_name(status));
        }

        void set_subdevice_mode(device & device, int subdevice_index, int width, int height, uint32_t fourcc, int fps, std::function<void(const void * frame)> callback)
        {
            device.subdevices[subdevice_index]->set_format(width, height, (const big_endian<int> &)fourcc, fps, callback);
        }
        void start_streaming(device & device, int num_transfer_bufs)
        {
            std::vector<subdevice *> subs;
            for(auto & sub : device.subdevices)
            {
                if(sub->callback)
                {
                    sub->start_capture();
                    subs.push_back(sub.get());
                }
            }

            device.thread = std::thread([&device, subs]()
            {
                while(!device.stop) subdevice::poll(subs);
            });

        }
        void stop_streaming(device & device)
        {
            if(device.thread.joinable())
            {
                device.stop = true;
                device.thread.join();
                device.stop = false;
            }
        }
        
        void set_pu_control(device & device, int subdevice, rs_option option, int value) {}
        int get_pu_control(const device & device, int subdevice, rs_option option) { return 0; }

        /////////////
        // context //
        /////////////

        std::shared_ptr<context> create_context()
        {
            return std::make_shared<context>();
        }

        std::vector<std::shared_ptr<device>> query_devices(std::shared_ptr<context> context)
        {                                            
            // Enumerate all subdevices present on the system
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

            // Group subdevices by vid/pid, and start a new device if we encounter a duplicate mi
            std::vector<std::shared_ptr<device>> devices;
            for(auto & sub : subdevices)
            {
                if(devices.empty() || sub->get_vid() != get_vendor_id(*devices.back())
                    || sub->get_pid() != get_product_id(*devices.back()) || devices.back()->has_mi(sub->mi))
                {
                    devices.push_back(std::make_shared<device>(context));
                }
                devices.back()->subdevices.push_back(move(sub));
            }

            return devices;
        }
    }
}
