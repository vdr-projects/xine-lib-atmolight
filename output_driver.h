/*
 * Copyright (C) 2009, 2010 Andreas Auras
 *
 * This file is part of the atmo post plugin, a plugin for the free xine video player.
 *
 * atmo post plugin is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * atmo post plugin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 */

#include <termios.h>
#include <regex.h>
#include <fcntl.h>
#include <math.h>

#define NUM_AREAS       9       /* Number of different areas (top, bottom ...) */

typedef struct { int sum, top, bottom, left, right, center, top_left, top_right, bottom_left, bottom_right; } num_channels_t;

extern long int lround(double); /* Missing in math.h? */


/*
 * abstraction for output drivers
 */
typedef struct output_driver_s output_driver_t;
struct output_driver_s {
    /* open device and configure for number of channels */
  int (*open)(output_driver_t *this, const char *param, num_channels_t *channels);

    /* configure device for number of channels */
  int (*configure)(output_driver_t *this, num_channels_t *channels);

    /* close device */
  void (*close)(output_driver_t *this);

    /*
     * send RGB color values to device
     * last_colors is NULL when first initial color packet is send
     * order for 'colors' is: top 1,2,3..., bottom 1,2,3..., left 1,2,3..., right 1,2,3..., center, top left, top right, bottom left, bottom right
     */
  void (*output_colors)(output_driver_t *this, rgb_color_t *new_colors, rgb_color_t *last_colors);

    /* provide detailed error message here if open of device fails */
  char errmsg[128];
};



/**********************************************************************************************************
 *    File output driver
 **********************************************************************************************************/

typedef struct {
  output_driver_t output_driver;
  num_channels_t num_channels;
  FILE *fd;
  int id;
} file_output_driver_t;


static int file_driver_open(output_driver_t *this_gen, const char *param, num_channels_t *n) {
  file_output_driver_t *this = (file_output_driver_t *) this_gen;

  this->num_channels = *n;
  this->id = 0;

  if (param && strlen(param))
    this->fd = fopen(param, "a");
  else
    this->fd = fopen("xine_atmo_data.out", "a");

  if (this->fd == NULL) {
    strerror_r(errno, this->output_driver.errmsg, sizeof(this->output_driver.errmsg));
    return -1;
  }
  return 0;
}

static int file_driver_configure(output_driver_t *this_gen, num_channels_t *n) {
  file_output_driver_t *this = (file_output_driver_t *) this_gen;
  this->num_channels = *n;
  return 0;
}

static void file_driver_close(output_driver_t *this_gen) {
  file_output_driver_t *this = (file_output_driver_t *) this_gen;

  if (this->fd) {
    fclose(this->fd);
    this->fd = NULL;
  }
}


static void file_driver_output_colors(output_driver_t *this_gen, rgb_color_t *colors, rgb_color_t *last_colors) {
  file_output_driver_t *this = (file_output_driver_t *) this_gen;
  FILE *fd = this->fd;
  struct timeval tvnow;
  int c;

  if (fd) {
    gettimeofday(&tvnow, NULL);
    fprintf(fd, "%d: %ld.%03ld ---\n", this->id++, tvnow.tv_sec, tvnow.tv_usec / 1000);

    for (c = 1; c <= this->num_channels.top; ++c, ++colors)
      fprintf(fd,"      top %2d: %3d %3d %3d\n", c, colors->r, colors->g, colors->b);

    for (c = 1; c <= this->num_channels.bottom; ++c, ++colors)
      fprintf(fd,"   bottom %2d: %3d %3d %3d\n", c, colors->r, colors->g, colors->b);

    for (c = 1; c <= this->num_channels.left; ++c, ++colors)
      fprintf(fd,"     left %2d: %3d %3d %3d\n", c, colors->r, colors->g, colors->b);

    for (c = 1; c <= this->num_channels.right; ++c, ++colors)
      fprintf(fd,"    right %2d: %3d %3d %3d\n", c, colors->r, colors->g, colors->b);

    if (this->num_channels.center) {
      fprintf(fd,"      center: %3d %3d %3d\n", colors->r, colors->g, colors->b);
      ++colors;
    }
    if (this->num_channels.top_left) {
      fprintf(fd,"    top left: %3d %3d %3d\n", colors->r, colors->g, colors->b);
      ++colors;
    }
    if (this->num_channels.top_right) {
      fprintf(fd,"    top right: %3d %3d %3d\n", colors->r, colors->g, colors->b);
      ++colors;
    }
    if (this->num_channels.bottom_left) {
      fprintf(fd,"  bottom left: %3d %3d %3d\n", colors->r, colors->g, colors->b);
      ++colors;
    }
    if (this->num_channels.bottom_right) {
      fprintf(fd," bottom right: %3d %3d %3d\n", colors->r, colors->g, colors->b);
    }
    fflush(fd);
  }
}



/*************************************************************************************************
 *    Serial output driver
 *************************************************************************************************/

typedef struct {
  output_driver_t output_driver;
  num_channels_t num_channels;
  int devfd;
} serial_output_driver_t;


static int serial_driver_open(output_driver_t *this_gen, const char *param, num_channels_t *n) {
  serial_output_driver_t *this = (serial_output_driver_t *) this_gen;
  char buf[256], buf1[64], *s;
  const char *devname = NULL;
  regex_t preg;
  int rc;

  this->num_channels = *n;
  this->devfd = -1;

  if (!param || !strlen(param))
  {
    strcpy(this->output_driver.errmsg, "no device parameter");
    return -1;
  }

  if (strncmp(param, "usb:", 4) == 0) {
      /* Lookup serial USB device name */
    rc = regcomp(&preg, param + 4, REG_EXTENDED | REG_NOSUB);
    if (rc) {
      regerror(rc, &preg, buf, sizeof(buf));
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "illegal device identification pattern '%s': %s", param + 4, buf);
      regfree(&preg);
      return -1;
    }

    FILE *procfd = fopen("/proc/tty/driver/usbserial", "r");
    if (!procfd) {
      strerror_r(errno, buf, sizeof(buf));
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "could not open /proc/tty/driver/usbserial: %s", buf);
      regfree(&preg);
      return -1;
    }

    while (fgets(buf, sizeof(buf), procfd)) {
      if (!regexec(&preg, buf, 0, NULL, 0) && (s = index(buf, ':'))) {
        *s = 0;
        snprintf(buf1, sizeof(buf1), "/dev/ttyUSB%s", buf);
        devname = buf1;
        break;
      }
    }
    fclose(procfd);
    regfree(&preg);
    if (!devname) {
      strcpy(this->output_driver.errmsg, "could not find usb device in /proc/tty/driver/usbserial");
      return -1;
    }
    llprintf(LOG_1, "USB tty device for '%s' is '%s'\n", param + 4, devname);
  } else {
    devname = param;
  }

    /* open serial port */
  int devfd = open(devname, O_RDWR | O_NOCTTY);
  if (devfd < 0) {
    strerror_r(errno, buf, sizeof(buf));
    snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "could not open serial port: %s", buf);
    return -1;
  }

    /* configure serial port */
  const int bconst = B38400;
  struct termios tio;
  memset(&tio, 0, sizeof(tio));
  tio.c_cflag = (CS8 | CREAD | CLOCAL);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 1;
  cfsetispeed(&tio, bconst);
  cfsetospeed(&tio, bconst);
  if (tcsetattr(devfd, TCSANOW, &tio)) {
    strerror_r(errno, buf, sizeof(buf));
    snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "configuration of serial port failed: %s", buf);
    close(devfd);
    return -1;
  }
  tcflush(devfd, TCIOFLUSH);

  this->devfd = devfd;
  return 0;
}


static int serial_driver_configure(output_driver_t *this_gen, num_channels_t *n) {
  serial_output_driver_t *this = (serial_output_driver_t *) this_gen;
  this->num_channels = *n;
  return 0;
}


static void serial_driver_close(output_driver_t *this_gen) {
  serial_output_driver_t *this = (serial_output_driver_t *) this_gen;

  if (this->devfd < 0)
    return;

  close(this->devfd);
  this->devfd = -1;
}



/**************************************************************************************
 *  Protocol for "classic" atmolight serial 2ch controller
 **************************************************************************************/

static void classic_driver_output_colors(output_driver_t *this_gen, rgb_color_t *colors, rgb_color_t *last_colors) {
  serial_output_driver_t *this = (serial_output_driver_t *) this_gen;
  uint8_t msg[19];

  if (this->devfd < 0)
    return;

    /* create command */
  memset(msg, 0, sizeof(msg));
  msg[0] = 0xFF;       /* start byte */
  msg[1] = msg[2] = 0; /* start channel */
  msg[3] = 15;         /* number of channels (5 * RGB) */

    /* top channel */
  if (this->num_channels.top)
  {
    msg[13] = colors->r;
    msg[14] = colors->g;
    msg[15] = colors->b;
    colors += this->num_channels.top;
  }

    /* bottom channel */
  if (this->num_channels.bottom)
  {
    msg[16] = colors->r;
    msg[17] = colors->g;
    msg[18] = colors->b;
    colors += this->num_channels.bottom;
  }

    /* left channel */
  if (this->num_channels.left)
  {
    msg[7] = colors->r;
    msg[8] = colors->g;
    msg[9] = colors->b;
    colors += this->num_channels.left;
  }

    /* right channel */
  if (this->num_channels.right)
  {
    msg[10] = colors->r;
    msg[11] = colors->g;
    msg[12] = colors->b;
    colors += this->num_channels.right;
  }

    /* center channel */
  if (this->num_channels.center)
  {
    msg[4] = colors->r;
    msg[5] = colors->g;
    msg[6] = colors->b;
  }

    /* send data to target */
  if (write(this->devfd, msg, sizeof(msg)) > 0)
    tcdrain(this->devfd); /* flush buffer */
}



/********************************************************************************
 * Protocol for my own designed 4ch serial controller
 ********************************************************************************/

static void df4ch_driver_output_colors(output_driver_t *this_gen, rgb_color_t *colors, rgb_color_t *last_colors) {
  serial_output_driver_t *this = (serial_output_driver_t *) this_gen;
  uint8_t msg[15];

  if (this->devfd < 0)
    return;

    /* create command */
  memset(msg, 0, sizeof(msg));
  msg[0] = 0xFF; /* start byte */
  msg[1] = 0;    /* start channel */
  msg[2] = 12;   /* number of channels (4 * RGB) */

    /* top channel */
  if (this->num_channels.top)
  {
    msg[9] = colors->r;
    msg[10] = colors->g;
    msg[11] = colors->b;
    colors += this->num_channels.top;
  }

    /* bottom channel */
  if (this->num_channels.bottom)
  {
    msg[12] = colors->r;
    msg[13] = colors->g;
    msg[14] = colors->b;
    colors += this->num_channels.bottom;
  }

    /* left channel */
  if (this->num_channels.left)
  {
    msg[3] = colors->r;
    msg[4] = colors->g;
    msg[5] = colors->b;
    colors += this->num_channels.left;
  }

    /* right channel */
  if (this->num_channels.right)
  {
    msg[6] = colors->r;
    msg[7] = colors->g;
    msg[8] = colors->b;
  }

    /* send data to target */
  if (write(this->devfd, msg, sizeof(msg)) > 0)
    tcdrain(this->devfd); /* flush buffer */
}



/***************************************************************************************************
 *    DF10CH output driver for my own designed "next generation" 10ch RGB Controller
 ***************************************************************************************************/

#include <libusb.h>
#include "df10ch_usb_proto.h"

#define DF10CH_USB_CFG_VENDOR_ID     0x16c0
#define DF10CH_USB_CFG_PRODUCT_ID    0x05dc
#define DF10CH_USB_CFG_VENDOR_NAME   "yak54@gmx.net"
#define DF10CH_USB_CFG_PRODUCT       "DF10CH"
#define DF10CH_USB_CFG_SERIAL        "AP"
#define DF10CH_USB_DEFAULT_TIMEOUT   100

#define DF10CH_MAX_CHANNELS     30
#define DF10CH_SIZE_CONFIG      (14 + DF10CH_MAX_CHANNELS * 6)
#define DF10CH_CONFIG_VALID_ID  0xA0A1

enum { DF10CH_AREA_TOP, DF10CH_AREA_BOTTOM, DF10CH_AREA_LEFT, DF10CH_AREA_RIGHT, DF10CH_AREA_CENTER, DF10CH_AREA_TOP_LEFT, DF10CH_AREA_TOP_RIGHT, DF10CH_AREA_BOTTOM_LEFT, DF10CH_AREA_BOTTOM_RIGHT };

typedef struct df10ch_gamma_tab_s {
  struct df10ch_gamma_tab_s *next;
  uint8_t gamma;
  uint16_t white_cal;
  uint16_t tab[256];
} df10ch_gamma_tab_t;

typedef struct {
  int req_channel;          // Channel number in request
  int area;                 // Source area
  int area_num;             // Source area number
  int color;                // Source color
  df10ch_gamma_tab_t *gamma_tab;  // Corresponding gamma table
} df10ch_channel_config_t;

typedef struct df10ch_ctrl_s {
  struct df10ch_ctrl_s *next;
  libusb_device_handle *dev;
  int idx_serial_number;
  uint16_t config_version;      // Version number of configuration data
  uint16_t pwm_res;             // PWM resolution
  int num_req_channels;         // Number of channels in request
  df10ch_channel_config_t *channel_config;      // List of channel configurations
  char id[32];                  // ID of Controller
  struct libusb_transfer *transfer;
  uint8_t *transfer_data;
  int pending_submit;
} df10ch_ctrl_t;

typedef struct {
  output_driver_t output_driver;
  libusb_context *ctx;
  num_channels_t num_channels;          // Global channel layout
  df10ch_ctrl_t *ctrls;                 // List of found controllers
  df10ch_gamma_tab_t *gamma_tabs;       // List of calculated gamma tables
  int max_transmit_latency;
  int avg_transmit_latency;
} df10ch_output_driver_t;


static const char *df10ch_usb_errmsg(int rc) {
  switch (rc) {
  case LIBUSB_SUCCESS:
    return ("Success (no error)");
  case LIBUSB_ERROR_IO:
    return ("Input/output error");
  case LIBUSB_ERROR_INVALID_PARAM:
    return ("Invalid parameter");
  case LIBUSB_ERROR_ACCESS:
    return ("Access denied (insufficient permissions)");
  case LIBUSB_ERROR_NO_DEVICE:
    return ("No such device (it may have been disconnected)");
  case LIBUSB_ERROR_NOT_FOUND:
    return ("Entity not found");
  case LIBUSB_ERROR_BUSY:
    return ("Resource busy");
  case LIBUSB_ERROR_TIMEOUT:
    return ("Operation timed out");
  case LIBUSB_ERROR_OVERFLOW:
    return ("Overflow");
  case LIBUSB_ERROR_PIPE:
    return ("Pipe error");
  case LIBUSB_ERROR_INTERRUPTED:
    return ("System call interrupted (perhaps due to signal)");
  case LIBUSB_ERROR_NO_MEM:
    return ("Insufficient memory");
  case LIBUSB_ERROR_NOT_SUPPORTED:
    return ("Operation not supported or unimplemented on this platform");
  case LIBUSB_ERROR_OTHER:
    return ("Other error");
  }
  return ("?");
}


static const char * df10ch_usb_transfer_errmsg(int s) {
  switch (s) {
  case LIBUSB_TRANSFER_COMPLETED:
    return ("Transfer completed without error");
  case LIBUSB_TRANSFER_ERROR:
    return ("Transfer failed");
  case LIBUSB_TRANSFER_TIMED_OUT:
    return ("Transfer timed out");
  case LIBUSB_TRANSFER_CANCELLED:
    return ("Transfer was cancelled");
  case LIBUSB_TRANSFER_STALL:
    return ("Control request stalled");
  case LIBUSB_TRANSFER_NO_DEVICE:
    return ("Device was disconnected");
  case LIBUSB_TRANSFER_OVERFLOW:
    return ("Device sent more data than requested");
  }
  return ("?");
}


static int df10ch_control_in_transfer(df10ch_ctrl_t *ctrl, uint8_t req, uint16_t val, uint16_t index, unsigned int timeout, uint8_t *buf, uint16_t buflen)
{
      // Use a return buffer always so that the controller is able to send a USB reply status
      // This is special for VUSB at controller side
    unsigned char rcbuf[1];
    int len = buflen;
    if (!len)
    {
        buf = rcbuf;
        len = 1;
    }

      // Because VUSB at controller sends ACK reply before CRC check of received data we have to retry sending request our self if data is corrupted
    int n = 0, retrys = 0;
    while (retrys < 3)
    {
        n = libusb_control_transfer(ctrl->dev, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE, req, val, index, buf, len, timeout);
        if (n != LIBUSB_ERROR_INTERRUPTED)
        {
            if (n >= 0 || n != LIBUSB_ERROR_PIPE)
                break;
            ++retrys;
        }
    }

    if (n < 0)
    {
        llprintf(LOG_1, "%s: sending USB control transfer message %d failed: %s\n", ctrl->id, req, df10ch_usb_errmsg(n));
        return -1;
    }

    if (n != buflen)
    {
        llprintf(LOG_1, "%s: sending USB control transfer message %d failed: read %d bytes but expected %d bytes\n", ctrl->id, req, n, buflen);
        return -1;
    }

    return 0;
}


static void df10ch_dispose(df10ch_output_driver_t *this) {
  df10ch_ctrl_t *ctrl = this->ctrls;
  while (ctrl) {
    libusb_free_transfer(ctrl->transfer);
    libusb_release_interface(ctrl->dev, 0);
    libusb_close(ctrl->dev);

    df10ch_ctrl_t *next = ctrl->next;
    free(ctrl->transfer_data);
    free(ctrl->channel_config);
    free(ctrl);
    ctrl = next;
  }

  if (this->ctx)
    libusb_exit(this->ctx);

  df10ch_gamma_tab_t *gt = this->gamma_tabs;
  while (gt) {
    df10ch_gamma_tab_t *next = gt->next;
    free(gt);
    gt = next;
  }

  this->ctrls = NULL;
  this->ctx = NULL;
  this->gamma_tabs = NULL;
}


static void df10ch_wait_for_replys(df10ch_output_driver_t *this) {
    // wait for end of all pending transfers
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = (DF10CH_USB_DEFAULT_TIMEOUT + 50) * 1000;
  df10ch_ctrl_t *ctrl = this->ctrls;
  while (ctrl) {
    if (ctrl->pending_submit) {
      int rc = libusb_handle_events_timeout(this->ctx, &timeout);
      if (rc && rc != LIBUSB_ERROR_INTERRUPTED) {
        llprintf(LOG_1, "handling USB events failed: %s\n", df10ch_usb_errmsg(rc));
        break;
      }
    }
    else
      ctrl = ctrl->next;
  }
}


static void df10ch_reply_cb(struct libusb_transfer *transfer) {
  df10ch_ctrl_t *ctrl = (df10ch_ctrl_t *) transfer->user_data;
  ctrl->pending_submit = 0;
  if (transfer->status != LIBUSB_TRANSFER_COMPLETED && transfer->status != LIBUSB_TRANSFER_CANCELLED)
    llprintf(LOG_1, "%s: submitting USB control transfer message failed: %s\n", ctrl->id, df10ch_usb_transfer_errmsg(transfer->status));
}


static int df10ch_driver_open(output_driver_t *this_gen, const char *param, num_channels_t *num_channels) {
  df10ch_output_driver_t *this = (df10ch_output_driver_t *) this_gen;

  if (libusb_init(&this->ctx) < 0) {
    strcpy(this->output_driver.errmsg, "can't initialize USB library");
    return -1;
  }

  libusb_device **list = NULL;
  size_t cnt = libusb_get_device_list(this->ctx, &list);
  if (cnt < 0) {
    snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "getting list of USB devices failed: %s", df10ch_usb_errmsg(cnt));
    df10ch_dispose(this);
    return -1;
  }

    // Note: Because controller uses obdev's free USB product/vendor ID's we have to do special lookup for finding
    // the controllers. See file "USB-IDs-for-free.txt" of VUSB distribution.
  int rc;
  size_t i;
  for (i = 0; i < cnt; i++) {
    libusb_device *d = list[i];
    struct libusb_device_descriptor desc;

    int busnum = libusb_get_bus_number(d);
    int devnum = libusb_get_device_address(d);

    rc = libusb_get_device_descriptor(d, &desc);
    if (rc < 0)
      llprintf(LOG_1, "USB[%d,%d]: getting USB device descriptor failed: %s\n", busnum, devnum, df10ch_usb_errmsg(rc));
    else if (desc.idVendor == DF10CH_USB_CFG_VENDOR_ID && desc.idProduct == DF10CH_USB_CFG_PRODUCT_ID) {
      libusb_device_handle *hdl = NULL;
      rc = libusb_open(d, &hdl);
      if (rc < 0)
        llprintf(LOG_1, "USB[%d,%d]: open of USB device failed: %s\n", busnum, devnum, df10ch_usb_errmsg(rc));
      else {
        unsigned char buf[256];
        rc = libusb_get_string_descriptor_ascii(hdl, desc.iManufacturer, buf, sizeof(buf));
        if (rc < 0)
          llprintf(LOG_1, "USB[%d,%d]: getting USB manufacturer string failed: %s\n", busnum, devnum, df10ch_usb_errmsg(rc));
        else if (rc == sizeof(DF10CH_USB_CFG_VENDOR_NAME) - 1 && !memcmp(buf, DF10CH_USB_CFG_VENDOR_NAME, rc)) {
          rc = libusb_get_string_descriptor_ascii(hdl, desc.iProduct, buf, sizeof(buf));
          if (rc < 0)
            llprintf(LOG_1, "USB[%d,%d]: getting USB product string failed: %s\n", busnum, devnum, df10ch_usb_errmsg(rc));
          else if (rc == sizeof(DF10CH_USB_CFG_PRODUCT) - 1 && !memcmp(buf, DF10CH_USB_CFG_PRODUCT, rc)) {
            char id[32];
            snprintf(id, sizeof(id), "DF10CH[%d,%d]", busnum, devnum);
            rc = libusb_set_configuration(hdl, 1);
            if (rc < 0)
              llprintf(LOG_1, "%s: setting USB configuration failed: %s\n", id, df10ch_usb_errmsg(rc));
            else {
              rc = libusb_claim_interface(hdl, 0);
              if (rc < 0)
                llprintf(LOG_1, "%s: claiming USB interface failed: %s\n", id, df10ch_usb_errmsg(rc));
              else {
                df10ch_ctrl_t *ctrl = (df10ch_ctrl_t *) calloc(1, sizeof(df10ch_ctrl_t));
                ctrl->next = this->ctrls;
                this->ctrls = ctrl;
                ctrl->dev = hdl;
                ctrl->idx_serial_number = desc.iSerialNumber;
                strcpy(ctrl->id, id);
                llprintf(LOG_1, "%s: device opened\n", id);
                continue;
              }
            }
          }
        }
        libusb_close(hdl);
      }
    }
  }

  libusb_free_device_list(list, 1);

  if (!this->ctrls) {
    strcpy(this->output_driver.errmsg, "USB: no DF10CH devices found!");
    df10ch_dispose(this);
    return -1;
  }

    // Ignore channel configuration defined by plugin parameters
  memset(num_channels, 0, sizeof(num_channels_t));

    // Read controller configuration
  df10ch_ctrl_t *ctrl = this->ctrls;
  while (ctrl) {
    uint8_t data[256];

      // Check that USB controller is running application firmware and not bootloader
    rc = libusb_get_string_descriptor_ascii(ctrl->dev, ctrl->idx_serial_number, data, sizeof(data) - 1);
    if (rc < 0) {
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: getting USB serial number string failed: %s", ctrl->id, df10ch_usb_errmsg(rc));
      df10ch_dispose(this);
      return -1;
    }
    if (rc != sizeof(DF10CH_USB_CFG_SERIAL) - 1 || memcmp(data, DF10CH_USB_CFG_SERIAL, rc)) {
      data[rc] = 0;
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: application firmware of USB controller is not running! Current mode is: %s", ctrl->id, data);
      df10ch_dispose(this);
      return -1;
    }

      // check that PWM controller is running application firmware and not bootloader
    if (df10ch_control_in_transfer(ctrl, PWM_REQ_GET_VERSION, 0, 0, DF10CH_USB_DEFAULT_TIMEOUT, data, 2)) {
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: reading PWM controller version fails!", ctrl->id);
      df10ch_dispose(this);
      return -1;
    }
    if (data[0] != PWM_VERS_APPL) {
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: application firmware of PWM controller is not running! Current mode is: %d", ctrl->id, data[0]);
      df10ch_dispose(this);
      return -1;
    }

      // read eeprom configuration data
    uint8_t eedata[DF10CH_SIZE_CONFIG];
    if (df10ch_control_in_transfer(ctrl, REQ_READ_EE_DATA, 0, 1, DF10CH_USB_DEFAULT_TIMEOUT, eedata, sizeof(eedata))) {
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: reading eeprom config data fails!", ctrl->id);
      df10ch_dispose(this);
      return -1;
    }

      // check that configuration data is valid
    int cfg_valid_id = eedata[0] + (eedata[1] << 8);
    if (cfg_valid_id != DF10CH_CONFIG_VALID_ID) {
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: controller is not configured! Please run setup program first", ctrl->id);
      df10ch_dispose(this);
      return -1;
    }

    ctrl->config_version = eedata[2] + (eedata[3] << 8);

      // Determine channel layout
    int n;
    n = eedata[4 + DF10CH_AREA_TOP];
    if (n > num_channels->top)
      num_channels->top = n;
    n = eedata[4 + DF10CH_AREA_BOTTOM];
    if (n > num_channels->bottom)
      num_channels->bottom = n;
    n = eedata[4 + DF10CH_AREA_LEFT];
    if (n > num_channels->left)
      num_channels->left = n;
    n = eedata[4 + DF10CH_AREA_RIGHT];
    if (n > num_channels->right)
      num_channels->right = n;
    n = eedata[4 + DF10CH_AREA_CENTER];
    if (n > num_channels->center)
      num_channels->center = n;
    n = eedata[4 + DF10CH_AREA_TOP_LEFT];
    if (n > num_channels->top_left)
      num_channels->top_left = n;
    n = eedata[4 + DF10CH_AREA_TOP_RIGHT];
    if (n > num_channels->top_right)
      num_channels->top_right = n;
    n = eedata[4 + DF10CH_AREA_BOTTOM_LEFT];
    if (n > num_channels->bottom_left)
      num_channels->bottom_left = n;
    n = eedata[4 + DF10CH_AREA_BOTTOM_RIGHT];
    if (n > num_channels->bottom_right)
      num_channels->bottom_right = n;

    ctrl->num_req_channels = eedata[13];
    if (ctrl->num_req_channels > DF10CH_MAX_CHANNELS)
      ctrl->num_req_channels = DF10CH_MAX_CHANNELS;

       // Read PWM resolution
    if (df10ch_control_in_transfer(ctrl, PWM_REQ_GET_MAX_PWM, 0, 0, DF10CH_USB_DEFAULT_TIMEOUT, data, 2)) {
      snprintf(this->output_driver.errmsg, sizeof(this->output_driver.errmsg), "%s: reading PWM resolution data fails!", ctrl->id);
      df10ch_dispose(this);
      return -1;
    }
    ctrl->pwm_res = data[0] + (data[1] << 8);

      // Build channel configuration list
    int nch = ctrl->num_req_channels;
    df10ch_channel_config_t *ccfg = (df10ch_channel_config_t *) calloc(nch, sizeof(df10ch_channel_config_t));
    ctrl->channel_config = ccfg;
    int eei = 14;
    while (nch) {
      ccfg->req_channel = eedata[eei];
      ccfg->area = eedata[eei + 1] >> 2;
      ccfg->color = eedata[eei + 1] & 0x03;
      ccfg->area_num = eedata[eei + 2];
      uint8_t gamma = eedata[eei + 3];
      if (gamma < 10)
        gamma = 10;
      uint16_t white_cal = eedata[eei + 4] + (eedata[eei + 5] << 8);

        // Lookup gamma table for gamma and white calibration value
      df10ch_gamma_tab_t *gt = this->gamma_tabs;
      while (gt && gamma != gt->gamma && white_cal != gt->white_cal)
        gt = gt->next;
      if (!gt) {
          // Calculate new gamma table
        gt = (df10ch_gamma_tab_t *) calloc(1, sizeof(df10ch_gamma_tab_t));
        gt->next = this->gamma_tabs;
        this->gamma_tabs = gt;
        gt->gamma = gamma;
        gt->white_cal = white_cal;
        double dgamma = gamma / 10.0;
        double dwhite_cal = white_cal;
        int v;
        for (v = 0; v < 256; ++v) {
          gt->tab[v] = (uint16_t) (lround(pow((v / 255.0), dgamma) * dwhite_cal));
          if (gt->tab[v] > ctrl->pwm_res)
            gt->tab[v] = ctrl->pwm_res;
        }
      }
      ccfg->gamma_tab = gt;

      ++ccfg;
      eei += 6;
      --nch;
    }

      // Prepare USB request for sending brightness values
    ctrl->transfer_data = calloc(1, (LIBUSB_CONTROL_SETUP_SIZE + ctrl->num_req_channels * 2));
    libusb_fill_control_setup(ctrl->transfer_data, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE, PWM_REQ_SET_BRIGHTNESS, 0, 0, ctrl->num_req_channels * 2);
    ctrl->transfer = libusb_alloc_transfer(0);
    libusb_fill_control_transfer(ctrl->transfer, ctrl->dev, ctrl->transfer_data, df10ch_reply_cb, ctrl, DF10CH_USB_DEFAULT_TIMEOUT);
    ctrl->pending_submit = 0;

    ctrl = ctrl->next;
  }

  this->num_channels = *num_channels;
  this->max_transmit_latency = 0;
  this->avg_transmit_latency = 0;
  return 0;
}


static int df10ch_driver_configure(output_driver_t *this_gen, num_channels_t *num_channels) {
  df10ch_output_driver_t *this = (df10ch_output_driver_t *) this_gen;

    // Ignore channel configuration defined by plugin parameters
  num_channels->top = this->num_channels.top;
  num_channels->bottom = this->num_channels.bottom;
  num_channels->left = this->num_channels.left;
  num_channels->right = this->num_channels.right;
  num_channels->center = this->num_channels.center;
  num_channels->top_left = this->num_channels.top_left;
  num_channels->top_right = this->num_channels.top_right;
  num_channels->bottom_left = this->num_channels.bottom_left;
  num_channels->bottom_right = this->num_channels.bottom_right;

  this->num_channels = *num_channels;
  return 0;
}


static void df10ch_driver_close(output_driver_t *this_gen) {
  df10ch_output_driver_t *this = (df10ch_output_driver_t *) this_gen;

  llprintf(LOG_1, "average transmit latency: %d [us]\n", this->avg_transmit_latency);

    // Cancel all pending requests
  df10ch_ctrl_t *ctrl = this->ctrls;
  while (ctrl) {
    if (ctrl->pending_submit)
      libusb_cancel_transfer(ctrl->transfer);
    ctrl = ctrl->next;
  }

  df10ch_wait_for_replys(this);
  df10ch_dispose(this);
}


static void df10ch_driver_output_colors(output_driver_t *this_gen, rgb_color_t *colors, rgb_color_t *last_colors) {
  df10ch_output_driver_t *this = (df10ch_output_driver_t *) this_gen;
  struct timeval tvnow, tvlast, tvdiff;

    // Build area mapping table
  rgb_color_t *area_map[9];
  rgb_color_t *c = colors;
  area_map[DF10CH_AREA_TOP] = c;
  c += this->num_channels.top;
  area_map[DF10CH_AREA_BOTTOM] = c;
  c += this->num_channels.bottom;
  area_map[DF10CH_AREA_LEFT] = c;
  c += this->num_channels.left;
  area_map[DF10CH_AREA_RIGHT] = c;
  c += this->num_channels.right;
  area_map[DF10CH_AREA_CENTER] = c;
  c += this->num_channels.center;
  area_map[DF10CH_AREA_TOP_LEFT] = c;
  c += this->num_channels.top_left;
  area_map[DF10CH_AREA_TOP_RIGHT] = c;
  c += this->num_channels.top_right;
  area_map[DF10CH_AREA_BOTTOM_LEFT] = c;
  c += this->num_channels.bottom_left;
  area_map[DF10CH_AREA_BOTTOM_RIGHT] = c;

  if (LOG_1)
    gettimeofday(&tvlast, NULL);

    // Generate transfer messages and send it to controllers
  df10ch_ctrl_t *ctrl = this->ctrls;
  while (ctrl) {
      // Generate payload data (brightness values)
    int do_submit = 0;
    uint8_t *payload = ctrl->transfer_data + LIBUSB_CONTROL_SETUP_SIZE;
    df10ch_channel_config_t *cfg = ctrl->channel_config;
    int nch = ctrl->num_req_channels;
    while (nch) {
      c = area_map[cfg->area] + cfg->area_num;
      rgb_color_t *lc = last_colors ? last_colors + (c - colors): NULL;
      int v = 0;
      switch (cfg->color) {
      case 0: // Red
        v = c->r;
        if (!lc || v != lc->r)
          do_submit = 1;
        break;
      case 1: // Green
        v = c->g;
        if (!lc || v != lc->g)
          do_submit = 1;
        break;
      case 2: // Blue
        v = c->b;
        if (!lc || v != lc->b)
          do_submit = 1;
      }

        // gamma and white calibration correction
      uint16_t bv = cfg->gamma_tab->tab[v];
      payload[cfg->req_channel * 2] = bv;
      payload[cfg->req_channel * 2 + 1] = bv >> 8;

      ++cfg;
      --nch;
    }

      // initiate asynchron data transfer to controller
    if (do_submit) {
      int rc = libusb_submit_transfer(ctrl->transfer);
      if (rc)
        llprintf(LOG_1, "%s: submitting USB control transfer message failed: %s\n", ctrl->id, df10ch_usb_errmsg(rc));
      else
        ctrl->pending_submit = 1;
    }

    ctrl = ctrl->next;
  }

    // wait for end of all pending transfers
  df10ch_wait_for_replys(this);

  if (LOG_1) {
    gettimeofday(&tvnow, NULL);
    timersub(&tvnow, &tvlast, &tvdiff);
    this->avg_transmit_latency = (this->avg_transmit_latency + tvdiff.tv_usec) / 2;
    if (tvdiff.tv_usec > this->max_transmit_latency) {
      this->max_transmit_latency = tvdiff.tv_usec;
      llprintf(LOG_1, "max/avg transmit latency: %d/%d [us]\n", this->max_transmit_latency, this->avg_transmit_latency);
    }
  }
}


/*
 * Output Drivers
 */

#define NUM_DRIVERS     4       /* Number of registered drivers */
static char *driver_enum[NUM_DRIVERS+1] = { "none", "file", "classic", "df4ch", "df10ch" };

typedef union {
  output_driver_t output_driver;
  file_output_driver_t file_output_driver;
  serial_output_driver_t serial_output_driver;
  df10ch_output_driver_t df10ch_output_driver;
} output_drivers_t;


static output_driver_t * get_output_driver(output_drivers_t *output_drivers, int driver) {
  memset(output_drivers, 0, sizeof(output_drivers_t));

  output_driver_t *output_driver = &output_drivers->output_driver;
  switch(driver) {
  case 1: /* file */
    output_driver->open = file_driver_open;
    output_driver->configure = file_driver_configure;
    output_driver->close = file_driver_close;
    output_driver->output_colors = file_driver_output_colors;
    break;
  case 2: /* classic */
    output_driver->open = serial_driver_open;
    output_driver->configure = serial_driver_configure;
    output_driver->close = serial_driver_close;
    output_driver->output_colors = classic_driver_output_colors;
    output_drivers->serial_output_driver.devfd = -1;
    break;
  case 3: /* df4ch */
    output_driver->open = serial_driver_open;
    output_driver->configure = serial_driver_configure;
    output_driver->close = serial_driver_close;
    output_driver->output_colors = df4ch_driver_output_colors;
    output_drivers->serial_output_driver.devfd = -1;
    break;
  case 4: /* df10ch */
    output_driver->open = df10ch_driver_open;
    output_driver->configure = df10ch_driver_configure;
    output_driver->close = df10ch_driver_close;
    output_driver->output_colors = df10ch_driver_output_colors;
    break;
  default: /* none */
    output_driver = NULL;
  }
  return output_driver;
}
