// SPDX-License-Identifier: GPL-2.0-only
/*
 * kmb-imx335.c - KeemBay Camera imx335 Sensor Driver.
 *
 * Copyright (C) 2020 Intel Corporation
 */

#include <asm/unaligned.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/kmb-isp-ctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define KMB_IMX335_DRV_NAME	"kmb-imx335-sensor"

/* Streaming Mode */
#define KMB_IMX335_REG_MODE_SELECT	0x3000
#define KMB_IMX335_MODE_STANDBY		0x01
#define KMB_IMX335_MODE_STREAMING	0x00

/* Lines per frame */
#define KMB_IMX335_REG_LPFR		0x3030
#define KMB_IMX335_LPFR_MAX		0xFFFFF

/* Chip ID */
#define KMB_IMX335_REG_ID	0x3912
#define KMB_IMX335_ID		0x0000
#define KMB_IMX335_Y_SIZE_ID	0x3056
#define KMB_IMX335_Y_SIZE	0xAC07

/* Exposure control */
#define KMB_IMX335_REG_SHUTTER		0x3058
#define KMB_IMX335_EXPOSURE_MIN		1
#define KMB_IMX335_EXPOSURE_STEP	1
#define KMB_IMX335_EXPOSURE_DEFAULT	0x0648

#define KMB_IMX335_REG_SHUTTER_S	0x305C
#define KMB_IMX335_EXPOSURE_S_MIN	4
#define KMB_IMX335_EXPOSURE_S_STEP	1
#define KMB_IMX335_EXPOSURE_S_DEFAULT	0x54

#define KMB_IMX335_REG_SHUTTER_VS	0x3060
#define KMB_IMX335_EXPOSURE_VS_MIN	6
#define KMB_IMX335_EXPOSURE_VS_STEP	1
#define KMB_IMX335_EXPOSURE_VS_DEFAULT	0x06

/* Analog gain control */
#define KMB_IMX335_REG_AGAIN		0x30E8
#define KMB_IMX335_AGAIN_MIN		0
#define KMB_IMX335_AGAIN_MAX		240
#define KMB_IMX335_AGAIN_STEP		1
#define KMB_IMX335_AGAIN_DEFAULT	0

#define KMB_IMX335_REG_AGAIN_S		0x30EA
#define KMB_IMX335_AGAIN_S_MIN		0
#define KMB_IMX335_AGAIN_S_MAX		240
#define KMB_IMX335_AGAIN_S_STEP		1
#define KMB_IMX335_AGAIN_S_DEFAULT	0

#define KMB_IMX335_REG_AGAIN_VS		0x30EC
#define KMB_IMX335_AGAIN_VS_MIN		0
#define KMB_IMX335_AGAIN_VS_MAX		240
#define KMB_IMX335_AGAIN_VS_STEP	1
#define KMB_IMX335_AGAIN_VS_DEFAULT	0

/* Group hold register */
#define KMB_IMX335_REG_HOLD	0x3001

/* Input clock rate */
#define KMB_IMX335_INCLK_RATE	24000000

#define KMB_IMX335_REG_MIN	0x00
#define KMB_IMX335_REG_MAX	0xFFFFF

/**
 * struct kmb_imx335_reg - KMB imx335 Sensor register
 * @address: Register address
 * @val: Register value
 */
struct kmb_imx335_reg {
	u16 address;
	u8 val;
};

/**
 * struct kmb_imx335_reg_list - KMB imx335 Sensor register list
 * @num_of_regs: Number of registers in the list
 * @regs: Pointer to register list
 */
struct kmb_imx335_reg_list {
	u32 num_of_regs;
	const struct kmb_imx335_reg *regs;
};

enum kmb_imx335_streaming_mode {
	KMB_IMX335_ULL = 0,
	KMB_IMX335_2DOL_HDR = 1,
	KMB_IMX335_3DOL_HDR = 2,
};

/**
 * struct kmb_imx335_mode - KMB imx335 Sensor mode structure
 * @width: Frame width
 * @height: Frame height
 * @code: Format code
 * @ppln: Pixels per line
 * @lpfr: Lines per frame
 * @skip_lines: Top lines to be skipped
 * @pclk: Sensor pixel clock
 * @row_time: Row time in ns
 * @def: Default frames per second
 * @min: Min frames per second
 * @max: Max frames per second
 * @step: Frame rate step
 * @reg_list: Register list for sensor mode
 */
struct kmb_imx335_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 ppln;
	u32 lpfr[3];
	u32 skip_lines;
	u64 pclk;
	u32 row_time;
	struct {
		u32 def;
		u32 min;
		u32 max;
		u32 step;
	} fps;
	struct kmb_imx335_reg_list reg_list;
};

/**
 * struct kmb_imx335 - KMB imx335 Sensor device structure
 * @dev: pointer to generic device
 * @client: pointer to i2c client
 * @sd: V4L2 sub-device
 * @pad: Media pad. Only one pad supported
 * @reset_gpio: Sensor reset gpio
 * @inclk: Sensor input clock
 * @ctrl_handler: V4L2 control handler
 * @pclk_ctrl: Pointer to pixel clock control
 * @hblank_ctrl: Pointer to horizontal blanking control
 * @vblank_ctrl: Pointer to vertical blanking control
 * @row_time_ctrl: Pointer to row time control
 * @exp_ctrl: Pointer to exposure control
 * @again_ctrl: Pointer to analog gain control
 * @exp1_ctrl: Pointer to short exposure control
 * @again1_ctrl: Pointer to short analog gain control
 * @exp2_ctrl: Pointer to very short exposure control
 * @again2_ctrl: Pointer to very short analog gain control
 * @fps: FPS to be applied on next stream on
 * @fps_min: lower FPS from all supported modes
 * @fps_max: higher FPS from all supported modes
 * @lpfr: Lines per frame for long exposure frame
 * @cur_mode: Current selected sensor mode
 * @supported_modes: Pointer to supported sensor modes
 * @num_supported_modes: Number of supported sensor modes
 * @mutex: Mutex for serializing sensor controls
 * @streaming_mode: Current selected streaming mode
 * @streaming: Flag indicating streaming state
 * @format_set: Flag indicating format is already set
 */
struct kmb_imx335 {
	struct device *dev;
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pclk_ctrl;
	struct v4l2_ctrl *hblank_ctrl;
	struct v4l2_ctrl *vblank_ctrl;
	struct v4l2_ctrl *row_time_ctrl;
	struct {
		struct v4l2_ctrl *exp_ctrl;
		struct v4l2_ctrl *again_ctrl;
		struct v4l2_ctrl *exp1_ctrl;
		struct v4l2_ctrl *again1_ctrl;
		struct v4l2_ctrl *exp2_ctrl;
		struct v4l2_ctrl *again2_ctrl;
	};
	u32 fps;
	u32 fps_min;
	u32 fps_max;
	u32 lpfr;
	const struct kmb_imx335_mode *cur_mode;
	const struct kmb_imx335_mode *supported_modes;
	u32 num_supported_modes;
	struct mutex mutex;
	enum kmb_imx335_streaming_mode streaming_mode;
	bool streaming;
	bool format_set;
};

/* Sensor mode registers */
static const struct kmb_imx335_reg mode_2592x1940_regs[] = {
	{0x3000, 0x01},
	{0x3002, 0x00},
	{0x300C, 0x3B},
	{0x300D, 0x2A},
	{0x3018, 0x04},
	{0x302C, 0x3C},
	{0x302E, 0x20},
	{0x3056, 0x94},
	{0x3074, 0xC8},
	{0x3076, 0x28},
	{0x304C, 0x00},
	{0x314C, 0xC6},
	{0x315A, 0x02},
	{0x3168, 0xA0},
	{0x316A, 0x7E},
	{0x31A1, 0x00},
	{0x3288, 0x21},
	{0x328A, 0x02},
	{0x3414, 0x05},
	{0x3416, 0x18},
	{0x3648, 0x01},
	{0x364A, 0x04},
	{0x364C, 0x04},
	{0x3678, 0x01},
	{0x367C, 0x31},
	{0x367E, 0x31},
	{0x3706, 0x10},
	{0x3708, 0x03},
	{0x3714, 0x02},
	{0x3715, 0x02},
	{0x3716, 0x01},
	{0x3717, 0x03},
	{0x371C, 0x3D},
	{0x371D, 0x3F},
	{0x372C, 0x00},
	{0x372D, 0x00},
	{0x372E, 0x46},
	{0x372F, 0x00},
	{0x3730, 0x89},
	{0x3731, 0x00},
	{0x3732, 0x08},
	{0x3733, 0x01},
	{0x3734, 0xFE},
	{0x3735, 0x05},
	{0x3740, 0x02},
	{0x375D, 0x00},
	{0x375E, 0x00},
	{0x375F, 0x11},
	{0x3760, 0x01},
	{0x3768, 0x1B},
	{0x3769, 0x1B},
	{0x376A, 0x1B},
	{0x376B, 0x1B},
	{0x376C, 0x1A},
	{0x376D, 0x17},
	{0x376E, 0x0F},
	{0x3776, 0x00},
	{0x3777, 0x00},
	{0x3778, 0x46},
	{0x3779, 0x00},
	{0x377A, 0x89},
	{0x377B, 0x00},
	{0x377C, 0x08},
	{0x377D, 0x01},
	{0x377E, 0x23},
	{0x377F, 0x02},
	{0x3780, 0xD9},
	{0x3781, 0x03},
	{0x3782, 0xF5},
	{0x3783, 0x06},
	{0x3784, 0xA5},
	{0x3788, 0x0F},
	{0x378A, 0xD9},
	{0x378B, 0x03},
	{0x378C, 0xEB},
	{0x378D, 0x05},
	{0x378E, 0x87},
	{0x378F, 0x06},
	{0x3790, 0xF5},
	{0x3792, 0x43},
	{0x3794, 0x7A},
	{0x3796, 0xA1},
	{0x37B0, 0x36},
	{0x3A00, 0x01},
};

static const struct kmb_imx335_reg mode_10bit_2592x1940_regs[] = {
	{0x3000, 0x01},
	{0x3002, 0x00},
	{0x300C, 0x3B},
	{0x300D, 0x2A},
	{0x3018, 0x04},
	{0x302C, 0x3C},
	{0x302E, 0x20},
	{0x3056, 0x94},
	{0x3074, 0xC8},
	{0x3076, 0x28},
	{0x304C, 0x00},
	{0x314C, 0xC6},
	{0x315A, 0x02},
	{0x3168, 0xA0},
	{0x316A, 0x7E},
	{0x31A1, 0x00},
	{0x3288, 0x21},
	{0x328A, 0x02},
	{0x3414, 0x05},
	{0x3416, 0x18},
	{0x3648, 0x01},
	{0x364A, 0x04},
	{0x364C, 0x04},
	{0x3678, 0x01},
	{0x367C, 0x31},
	{0x367E, 0x31},
	{0x3706, 0x10},
	{0x3708, 0x03},
	{0x3714, 0x02},
	{0x3715, 0x02},
	{0x3716, 0x01},
	{0x3717, 0x03},
	{0x371C, 0x3D},
	{0x371D, 0x3F},
	{0x372C, 0x00},
	{0x372D, 0x00},
	{0x372E, 0x46},
	{0x372F, 0x00},
	{0x3730, 0x89},
	{0x3731, 0x00},
	{0x3732, 0x08},
	{0x3733, 0x01},
	{0x3734, 0xFE},
	{0x3735, 0x05},
	{0x3740, 0x02},
	{0x375D, 0x00},
	{0x375E, 0x00},
	{0x375F, 0x11},
	{0x3760, 0x01},
	{0x3768, 0x1B},
	{0x3769, 0x1B},
	{0x376A, 0x1B},
	{0x376B, 0x1B},
	{0x376C, 0x1A},
	{0x376D, 0x17},
	{0x376E, 0x0F},
	{0x3776, 0x00},
	{0x3777, 0x00},
	{0x3778, 0x46},
	{0x3779, 0x00},
	{0x377A, 0x89},
	{0x377B, 0x00},
	{0x377C, 0x08},
	{0x377D, 0x01},
	{0x377E, 0x23},
	{0x377F, 0x02},
	{0x3780, 0xD9},
	{0x3781, 0x03},
	{0x3782, 0xF5},
	{0x3783, 0x06},
	{0x3784, 0xA5},
	{0x3788, 0x0F},
	{0x378A, 0xD9},
	{0x378B, 0x03},
	{0x378C, 0xEB},
	{0x378D, 0x05},
	{0x378E, 0x87},
	{0x378F, 0x06},
	{0x3790, 0xF5},
	{0x3792, 0x43},
	{0x3794, 0x7A},
	{0x3796, 0xA1},
	{0x37B0, 0x36},
	{0x3A00, 0x01},
	{0x319D, 0x00},
	{0x341C, 0xFF},
	{0x341D, 0x01},
	{0x3050, 0x00},
	{0x3034, 0x13},
	{0x3035, 0x01}
};

static const struct kmb_imx335_reg mode_hdr_2dol_15fps_2592x1940_regs[] = {
	{0x3000, 0x01},
	{0x3002, 0x00},
	{0x300C, 0x3B},
	{0x300D, 0x2A},
	{0x3018, 0x04},
	{0x302C, 0x3C},
	{0x302E, 0x20},
	{0x3056, 0x94},
	{0x3074, 0xC8},
	{0x3076, 0x28},
	{0x3030, 0x88},
	{0x3031, 0x13},
	{0x3034, 0xEF},
	{0x3035, 0x01},
	{0x3048, 0x01},
	{0x3049, 0x01},
	{0x304A, 0x04},
	{0x304B, 0x03},
	{0x304C, 0x13},
	{0x3058, 0xF8},
	{0x3059, 0x24},
	{0x3068, 0x92},
	{0x3069, 0x00},
	{0x306A, 0x00},
	{0x314C, 0xC6},
	{0x315A, 0x02},
	{0x3168, 0xA0},
	{0x316A, 0x7E},
	{0x319F, 0x01},
	{0x31A1, 0x00},
	{0x31D7, 0x01},
	{0x3288, 0x21},
	{0x328A, 0x02},
	{0x3414, 0x05},
	{0x3416, 0x18},
	{0x304C, 0x13},
	{0x3648, 0x01},
	{0x364A, 0x04},
	{0x364C, 0x04},
	{0x3678, 0x01},
	{0x367C, 0x31},
	{0x367E, 0x31},
	{0x3706, 0x10},
	{0x3708, 0x03},
	{0x3714, 0x02},
	{0x3715, 0x02},
	{0x3716, 0x01},
	{0x3717, 0x03},
	{0x371C, 0x3D},
	{0x371D, 0x3F},
	{0x372C, 0x00},
	{0x372D, 0x00},
	{0x372E, 0x46},
	{0x372F, 0x00},
	{0x3730, 0x89},
	{0x3731, 0x00},
	{0x3732, 0x08},
	{0x3733, 0x01},
	{0x3734, 0xFE},
	{0x3735, 0x05},
	{0x3740, 0x02},
	{0x375D, 0x00},
	{0x375E, 0x00},
	{0x375F, 0x11},
	{0x3760, 0x01},
	{0x3768, 0x1B},
	{0x3769, 0x1B},
	{0x376A, 0x1B},
	{0x376B, 0x1B},
	{0x376C, 0x1A},
	{0x376D, 0x17},
	{0x376E, 0x0F},
	{0x3776, 0x00},
	{0x3777, 0x00},
	{0x3778, 0x46},
	{0x3779, 0x00},
	{0x377A, 0x89},
	{0x377B, 0x00},
	{0x377C, 0x08},
	{0x377D, 0x01},
	{0x377E, 0x23},
	{0x377F, 0x02},
	{0x3780, 0xD9},
	{0x3781, 0x03},
	{0x3782, 0xF5},
	{0x3783, 0x06},
	{0x3784, 0xA5},
	{0x3788, 0x0F},
	{0x378A, 0xD9},
	{0x378B, 0x03},
	{0x378C, 0xEB},
	{0x378D, 0x05},
	{0x378E, 0x87},
	{0x378F, 0x06},
	{0x3790, 0xF5},
	{0x3792, 0x43},
	{0x3794, 0x7A},
	{0x3796, 0xA1},
	{0x37B0, 0x36},
	{0x3A00, 0x01},
};

static const struct kmb_imx335_reg mode_2dol_10bit_30fps_2592x1940_regs[] = {
	{0x3000, 0x01},
	{0x3002, 0x00},
	{0x300C, 0x3B},
	{0x300D, 0x2A},
	{0x3018, 0x04},
	{0x302C, 0x3C},
	{0x302E, 0x20},
	{0x3056, 0x94},
	{0x3074, 0xC8},
	{0x3076, 0x28},
	{0x3034, 0x13},
	{0x3035, 0x01},
	{0x3048, 0x01},
	{0x3049, 0x01},
	{0x304A, 0x04},
	{0x304B, 0x03},
	{0x304C, 0x13},
	{0x3058, 0x60},
	{0x3059, 0x1F},
	{0x3068, 0x92},
	{0x3069, 0x00},
	{0x306A, 0x00},
	{0x314C, 0xC6},
	{0x315A, 0x02},
	{0x3168, 0xA0},
	{0x316A, 0x7E},
	{0x319F, 0x01},
	{0x31A1, 0x00},
	{0x31D7, 0x01},
	{0x3288, 0x21},
	{0x328A, 0x02},
	{0x3414, 0x05},
	{0x3416, 0x18},
	{0x304C, 0x13},
	{0x3648, 0x01},
	{0x364A, 0x04},
	{0x364C, 0x04},
	{0x3678, 0x01},
	{0x367C, 0x31},
	{0x367E, 0x31},
	{0x3706, 0x10},
	{0x3708, 0x03},
	{0x3714, 0x02},
	{0x3715, 0x02},
	{0x3716, 0x01},
	{0x3717, 0x03},
	{0x371C, 0x3D},
	{0x371D, 0x3F},
	{0x372C, 0x00},
	{0x372D, 0x00},
	{0x372E, 0x46},
	{0x372F, 0x00},
	{0x3730, 0x89},
	{0x3731, 0x00},
	{0x3732, 0x08},
	{0x3733, 0x01},
	{0x3734, 0xFE},
	{0x3735, 0x05},
	{0x3740, 0x02},
	{0x375D, 0x00},
	{0x375E, 0x00},
	{0x375F, 0x11},
	{0x3760, 0x01},
	{0x3768, 0x1B},
	{0x3769, 0x1B},
	{0x376A, 0x1B},
	{0x376B, 0x1B},
	{0x376C, 0x1A},
	{0x376D, 0x17},
	{0x376E, 0x0F},
	{0x3776, 0x00},
	{0x3777, 0x00},
	{0x3778, 0x46},
	{0x3779, 0x00},
	{0x377A, 0x89},
	{0x377B, 0x00},
	{0x377C, 0x08},
	{0x377D, 0x01},
	{0x377E, 0x23},
	{0x377F, 0x02},
	{0x3780, 0xD9},
	{0x3781, 0x03},
	{0x3782, 0xF5},
	{0x3783, 0x06},
	{0x3784, 0xA5},
	{0x3788, 0x0F},
	{0x378A, 0xD9},
	{0x378B, 0x03},
	{0x378C, 0xEB},
	{0x378D, 0x05},
	{0x378E, 0x87},
	{0x378F, 0x06},
	{0x3790, 0xF5},
	{0x3792, 0x43},
	{0x3794, 0x7A},
	{0x3796, 0xA1},
	{0x37B0, 0x36},
	{0x3A00, 0x01},
	{0x3050, 0x00},
	{0x319D, 0x00},
	{0x341C, 0xFF},
	{0x341D, 0x01},
};

static const struct kmb_imx335_reg mode_hdr_3dol_15fps_2592x1940_regs[] = {
	{0x3000, 0x01},
	{0x3002, 0x00},
	{0x300C, 0x3B},
	{0x300D, 0x2A},
	{0x3018, 0x04},
	{0x302C, 0x3C},
	{0x302E, 0x20},
	{0x3030, 0x94},
	{0x3031, 0x11},
	{0x3034, 0x13},
	{0x3035, 0x01},
	{0x3048, 0x01},
	{0x3049, 0x02},
	{0x304A, 0x05},
	{0x304B, 0x03},
	{0x304C, 0x13},
	{0x3050, 0x00},
	{0x3056, 0x94},
	{0x3058, 0x35},
	{0x3059, 0x46},
	{0x305C, 0x3E},
	{0x305D, 0x00},
	{0x3060, 0xBE},
	{0x3068, 0x9E},
	{0x3069, 0x00},
	{0x306A, 0x00},
	{0x306C, 0xC4},
	{0x306D, 0x00},
	{0x3074, 0xC8},
	{0x3076, 0x28},
	{0x314C, 0xC6},
	{0x315A, 0x02},
	{0x3168, 0xA0},
	{0x316A, 0x7E},
	{0x319D, 0x00},
	{0x319F, 0x01},
	{0x31A1, 0x00},
	{0x31D7, 0x03},
	{0x3288, 0x21},
	{0x328A, 0x02},
	{0x3414, 0x05},
	{0x3416, 0x18},
	{0x341C, 0xFF},
	{0x341D, 0x01},
	{0x3648, 0x01},
	{0x364A, 0x04},
	{0x364C, 0x04},
	{0x3678, 0x01},
	{0x367C, 0x31},
	{0x367E, 0x31},
	{0x3706, 0x10},
	{0x3708, 0x03},
	{0x3714, 0x02},
	{0x3715, 0x02},
	{0x3716, 0x01},
	{0x3717, 0x03},
	{0x371C, 0x3D},
	{0x371D, 0x3F},
	{0x372C, 0x00},
	{0x372D, 0x00},
	{0x372E, 0x46},
	{0x372F, 0x00},
	{0x3730, 0x89},
	{0x3731, 0x00},
	{0x3732, 0x08},
	{0x3733, 0x01},
	{0x3734, 0xFE},
	{0x3735, 0x05},
	{0x3740, 0x02},
	{0x375D, 0x00},
	{0x375E, 0x00},
	{0x375F, 0x11},
	{0x3760, 0x01},
	{0x3768, 0x1B},
	{0x3769, 0x1B},
	{0x376A, 0x1B},
	{0x376B, 0x1B},
	{0x376C, 0x1A},
	{0x376D, 0x17},
	{0x376E, 0x0F},
	{0x3776, 0x00},
	{0x3777, 0x00},
	{0x3778, 0x46},
	{0x3779, 0x00},
	{0x377A, 0x89},
	{0x377B, 0x00},
	{0x377C, 0x08},
	{0x377D, 0x01},
	{0x377E, 0x23},
	{0x377F, 0x02},
	{0x3780, 0xD9},
	{0x3781, 0x03},
	{0x3782, 0xF5},
	{0x3783, 0x06},
	{0x3784, 0xA5},
	{0x3788, 0x0F},
	{0x378A, 0xD9},
	{0x378B, 0x03},
	{0x378C, 0xEB},
	{0x378D, 0x05},
	{0x378E, 0x87},
	{0x378F, 0x06},
	{0x3790, 0xF5},
	{0x3792, 0x43},
	{0x3794, 0x7A},
	{0x3796, 0xA1},
	{0x37B0, 0x36},
	{0x3A00, 0x01},
};

/* Supported sensor mode configurations */
static const struct kmb_imx335_mode supported_modes[] = {
	{
		.width = 2592,
		.height = 1940,
		.ppln = 2934,
		.lpfr[0] = 4500,
		.skip_lines = 1,
		.pclk = 396000000,
		.code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.fps = {
			.def = 30,
			.max = 30,
			.min = 1,
			.step = 5,
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_2592x1940_regs),
			.regs = mode_2592x1940_regs,
		},
	},
	{
		.width = 2592,
		.height = 1940,
		.ppln = 2934,
		.lpfr[0] = 4500,
		.skip_lines = 1,
		.pclk = 475200000,
		.row_time = 3705,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.fps = {
			.def = 60,
			.max = 60,
			.min = 1,
			.step = 5,
		},
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_10bit_2592x1940_regs),
			.regs = mode_10bit_2592x1940_regs,
		},
	},
};

static const struct kmb_imx335_mode hdr_2dol_supported_modes[] = {
	{
		.width = 2592,
		.height = 1940,
		.ppln = 3465,
		.lpfr[0] = 10000,
		.lpfr[1] = 146,
		.skip_lines = 20,
		.pclk = 396000000,
		.row_time = 6670,
		.code = MEDIA_BUS_FMT_SRGGB12_1X12,
		.fps = {
			.def = 15,
			.max = 15,
			.min = 1,
			.step = 5,
		},
		.reg_list = {
			.num_of_regs =
				ARRAY_SIZE(mode_hdr_2dol_15fps_2592x1940_regs),
			.regs = mode_hdr_2dol_15fps_2592x1940_regs,
		},
	},
	{
		.width = 2592,
		.height = 1940,
		.ppln = 3465,
		.lpfr[0] = 9000,
		.lpfr[1] = 138,
		.skip_lines = 20,
		.pclk = 457200000,
		.row_time = 3705,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.fps = {
			.def = 30,
			.max = 30,
			.min = 1,
			.step = 5,
		},
		.reg_list = {
			.num_of_regs =
				ARRAY_SIZE(
					mode_2dol_10bit_30fps_2592x1940_regs),
			.regs = mode_2dol_10bit_30fps_2592x1940_regs,
		},
	},
};

static const struct kmb_imx335_mode hdr_3dol_supported_modes[] = {
	{
		.width = 2592,
		.height = 1940,
		.ppln = 2200,
		.lpfr[0] = 18000,
		.lpfr[1] = 158,
		.lpfr[2] = 196,
		.skip_lines = 20,
		.pclk = 475200000,
		.row_time = 3750,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.fps = {
			.def = 15,
			.max = 15,
			.min = 1,
			.step = 5,
		},
		.reg_list = {
			.num_of_regs =
				ARRAY_SIZE(mode_hdr_3dol_15fps_2592x1940_regs),
			.regs = mode_hdr_3dol_15fps_2592x1940_regs,
		},
	},
};

static const u32 supported_codes[] = {
	MEDIA_BUS_FMT_SRGGB12_1X12,
	MEDIA_BUS_FMT_SRGGB10_1X10
};

/**
 * to_kmb_imx335 - imx335 V4L2 sub-device to kmb_imx335 device.
 * @subdev: pointer to imx335 V4L2 sub-device device
 *
 * Return: Pointer to kmb_imx335 device
 */
static inline struct kmb_imx335 *to_kmb_imx335(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct kmb_imx335, sd);
}

/**
 * kmb_imx335_read_reg - Read registers.
 * @kmb_imx335: pointer to imx335 device
 * @reg: Register address
 * @len: Length of bytes to read. Max supported bytes is 4
 * @val: Pointer to register value to be filled.
 *
 * Return: 0 if successful
 */
static int
kmb_imx335_read_reg(struct kmb_imx335 *kmb_imx335, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&kmb_imx335->sd);
	struct i2c_msg msgs[2] = { 0 };
	u8 addr_buf[2] = { reg >> 8, reg & 0xff };
	u8 data_buf[4] = { 0 };
	int ret;

	if (len > 4)
		return -EINVAL;

	/* Write register address */
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	/* Read data from register */
	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = get_unaligned_be32(data_buf);

	return 0;
}

/**
 * kmb_imx335_write_reg - Write register
 * @kmb_imx335: pointer to imx335 device
 * @reg: Register address
 * @len: Length of bytes. Max supported bytes is 4
 * @val: Register value
 *
 * Return: 0 if successful
 */
static int
kmb_imx335_write_reg(struct kmb_imx335 *kmb_imx335, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&kmb_imx335->sd);
	u8 buf[3];
	int i;
	int ret;

	if (len > 4) {
		dev_err_ratelimited(kmb_imx335->dev,
				    "write reg 0x%4.4x invalid len %d",
				    reg, len);
		return -EINVAL;
	}

	/* Currently we can write to sensor only one byte at a time */
	for (i = 0; i < len; i++) {
		put_unaligned_be16(reg + i, buf);
		buf[2] = (val >> (8 * i)) & 0xFF;
		ret = i2c_master_send(client, buf, ARRAY_SIZE(buf));
		if (ret != ARRAY_SIZE(buf)) {
			dev_err_ratelimited(kmb_imx335->dev,
					"write reg 0x%4.4x return err %d",
					reg, ret);
			return -EIO;
		}
	}

	return 0;
}

/**
 * kmb_imx335_write_regs - Write a list of registers
 * @kmb_imx335: pointer to imx335 device
 * @regs: List of registers to be written
 * @len: Length of registers array
 *
 * Return: 0 if successful
 */
static int kmb_imx335_write_regs(struct kmb_imx335 *kmb_imx335,
			     const struct kmb_imx335_reg *regs, u32 len)
{
	int ret;
	u32 i;

	for (i = 0; i < len; i++) {
		ret = kmb_imx335_write_reg(kmb_imx335,
					   regs[i].address,
					   1, regs[i].val);
		if (ret)
			return ret;
	}

	return 0;
}


/**
 * kmb_imx335_update_fps - Update current sensor mode to match the selected FPS
 * @kmb_imx335: pointer to imx335 device
 * @mode: pointer to kmb_imx335_mode sensor mode
 *
 * Return: none
 */
static void kmb_imx335_update_fps(struct kmb_imx335 *kmb_imx335,
				  const struct kmb_imx335_mode *mode)
{
	u32 lpfr = (mode->lpfr[0] * mode->fps.def) / kmb_imx335->fps;

	if (lpfr > KMB_IMX335_LPFR_MAX)
		lpfr = KMB_IMX335_LPFR_MAX;

	if (lpfr < mode->height + 96)
		lpfr = mode->height + 96;

	kmb_imx335->lpfr = lpfr;

	dev_dbg(kmb_imx335->dev, "Selected FPS %d lpfr %d",
		kmb_imx335->fps, lpfr);
}

/**
 * kmb_imx335_open - Open imx335 subdevice
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @fh: pointer to imx335 V4L2 sub-device file handle
 *
 * Return: 0 if successful
 */
static int kmb_imx335_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, fh->state, 0);

	mutex_lock(&kmb_imx335->mutex);

	/* Initialize try_fmt */
	try_fmt->width = kmb_imx335->cur_mode->width;
	try_fmt->height = kmb_imx335->cur_mode->height;
	try_fmt->code = kmb_imx335->cur_mode->code;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&kmb_imx335->mutex);

	return 0;
}

/**
 * kmb_imx335_set_ctrl - Set subdevice control. Supported controls:
 *                       V4L2_CID_ANALOGUE_GAIN
 *                       V4L2_CID_EXPOSURE
 *                       Both controls are in one cluster.
 *
 * @ctrl: pointer to v4l2_ctrl structure
 *
 * Return: 0 if successful
 */
static int kmb_imx335_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct kmb_imx335 *kmb_imx335 =
		container_of(ctrl->handler, struct kmb_imx335, ctrl_handler);
	u32 exposure[3] = {0};
	u32 analog_gain[3] = {0};
	u32 shutter[3] = {0};
	u32 lpfr[3] = {0};
	int ret;

	/* Set exposure and gain only if sensor is in power on state */
	if (!pm_runtime_get_if_in_use(kmb_imx335->dev))
		return 0;

	/* Handle the cluster for both controls */
	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		exposure[0] = ctrl->val;
		analog_gain[0] = kmb_imx335->again_ctrl->val;

		exposure[1] = kmb_imx335->exp1_ctrl->val;
		analog_gain[1] = kmb_imx335->again1_ctrl->val;

		exposure[2] = kmb_imx335->exp2_ctrl->val;
		analog_gain[2] = kmb_imx335->again2_ctrl->val;
		break;
	default:
		dev_err(kmb_imx335->dev, "Invalid control %d", ctrl->id);
		ret = -EINVAL;
		goto error_pm_runtime_put;
	}

	lpfr[0] = kmb_imx335->lpfr;

	switch (kmb_imx335->streaming_mode) {
	case KMB_IMX335_ULL:
		if (exposure[0] > (lpfr[0] - 9))
			exposure[0] = lpfr[0] - 9;
		else if (exposure[0] < 1)
			exposure[0] = 1;

		shutter[0] = lpfr[0] - exposure[0];

		dev_dbg(kmb_imx335->dev,
			"Set long exp %u analog gain %u sh0 %u lpfr %u",
			exposure[0], analog_gain[0], shutter[0], lpfr[0]);
		break;
	case KMB_IMX335_2DOL_HDR:
		lpfr[1] = kmb_imx335->cur_mode->lpfr[1];

		if (exposure[0] > (lpfr[0] - lpfr[1] - 18))
			exposure[0] = lpfr[0] - lpfr[1] - 18;
		else if (exposure[0] < 4)
			exposure[0] = 4;

		shutter[0] = (lpfr[0] - exposure[0]) & ~3;

		dev_dbg(kmb_imx335->dev,
			"Set long exp %u analog gain %u sh0 %u lpfr %u",
			exposure[0], analog_gain[0], shutter[0], lpfr[0]);

		lpfr[0] /= 2;

		if (exposure[1] > (lpfr[1] - 18))
			exposure[1] = lpfr[1] - 18;
		else if (exposure[1] < 4)
			exposure[1] = 4;

		shutter[1] = ((lpfr[1] - exposure[1]) & ~3) + 2;

		dev_dbg(kmb_imx335->dev,
			"Set short exp %u analog gain %u sh1 %u lpfr %u",
			exposure[1], analog_gain[1], shutter[1], lpfr[1]);
		break;
	case KMB_IMX335_3DOL_HDR:
		lpfr[1] = kmb_imx335->cur_mode->lpfr[1];
		lpfr[2] = kmb_imx335->cur_mode->lpfr[2];

		if (exposure[0] > (lpfr[0] - lpfr[2] - 26))
			exposure[0] = lpfr[0] - lpfr[2] - 26;
		else if (exposure[0] < 6)
			exposure[0] = 6;

		shutter[0] = lpfr[0] - exposure[0];
		shutter[0] -= (shutter[0] % 6);

		dev_dbg(kmb_imx335->dev,
			"Set long exp %u analog gain %u sh0 %u lpfr %u",
			exposure[0], analog_gain[0], shutter[0], lpfr[0]);

		lpfr[0] /= 4;

		if (exposure[1] > (lpfr[1] - 26))
			exposure[1] = lpfr[1] - 26;
		else if (exposure[1] < 6)
			exposure[1] = 6;

		shutter[1] = lpfr[1] - exposure[1];
		shutter[1] -= (shutter[1] % 6) + 2;

		dev_dbg(kmb_imx335->dev,
			"Set short exp %u analog gain %u sh1 %u lpfr %u",
			exposure[1], analog_gain[1], shutter[1], lpfr[1]);

		if (exposure[2] > (lpfr[2] - lpfr[1] - 26))
			exposure[2] = lpfr[2] - lpfr[1] - 26;
		else if (exposure[2] < 6)
			exposure[2] = 6;

		shutter[2] = lpfr[2] - exposure[2];
		shutter[2] -= (shutter[2] % 6) + 4;

		dev_dbg(kmb_imx335->dev,
			"Set very short exp %u analog gain %u sh2 %u lpfr %u",
			exposure[2], analog_gain[2], shutter[2], lpfr[2]);

		break;
	default:
		dev_err(kmb_imx335->dev, "Invalid sensor mode %d",
			kmb_imx335->streaming_mode);
		ret = -EINVAL;
		goto error_pm_runtime_put;
	}

	ret = kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_HOLD, 1, 1);
	if (ret)
		goto error_pm_runtime_put;

	ret = kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_LPFR,
				   3, lpfr[0]);
	if (ret)
		goto error_release_group_hold;

	ret = kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_SHUTTER,
				   3, shutter[0]);
	if (ret)
		goto error_release_group_hold;

	ret = kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_AGAIN,
				   2, analog_gain[0]);
	if (ret)
		goto error_release_group_hold;

	if (kmb_imx335->streaming_mode >= KMB_IMX335_2DOL_HDR) {
		ret = kmb_imx335_write_reg(kmb_imx335,
				   KMB_IMX335_REG_SHUTTER_S,
				   3, shutter[1]);
		if (ret)
			goto error_release_group_hold;

		ret = kmb_imx335_write_reg(kmb_imx335,
					KMB_IMX335_REG_AGAIN_S,
					2, analog_gain[1]);
		if (ret)
			goto error_release_group_hold;
	}

	if (kmb_imx335->streaming_mode == KMB_IMX335_3DOL_HDR) {
		ret = kmb_imx335_write_reg(kmb_imx335,
				   KMB_IMX335_REG_SHUTTER_VS,
				   3, shutter[2]);
		if (ret)
			goto error_release_group_hold;

		ret = kmb_imx335_write_reg(kmb_imx335,
					KMB_IMX335_REG_AGAIN_VS,
					2, analog_gain[2]);
		if (ret)
			goto error_release_group_hold;
	}

	kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_HOLD, 1, 0);

	pm_runtime_put(kmb_imx335->dev);

	return 0;

error_release_group_hold:
	kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_HOLD, 1, 0);
error_pm_runtime_put:
	pm_runtime_put(kmb_imx335->dev);
	return ret;
}

/* V4l2 subdevice control ops*/
static const struct v4l2_ctrl_ops kmb_imx335_ctrl_ops = {
	.s_ctrl = kmb_imx335_set_ctrl,
};

static const struct v4l2_ctrl_config again_short = {
	.ops = &kmb_imx335_ctrl_ops,
	.id = V4L2_CID_ANALOGUE_GAIN_SHORT,
	.name = "V4L2_CID_ANALOGUE_GAIN_SHORT",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = KMB_IMX335_AGAIN_S_MIN,
	.max = KMB_IMX335_AGAIN_S_MAX,
	.def = KMB_IMX335_AGAIN_S_DEFAULT,
	.step = KMB_IMX335_AGAIN_S_STEP,
	.menu_skip_mask = 0,
};

static const struct v4l2_ctrl_config again_very_short = {
	.ops = &kmb_imx335_ctrl_ops,
	.id = V4L2_CID_ANALOGUE_GAIN_VERY_SHORT,
	.name = "V4L2_CID_ANALOGUE_GAIN_VERY_SHORT",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = KMB_IMX335_AGAIN_VS_MIN,
	.max = KMB_IMX335_AGAIN_VS_MAX,
	.def = KMB_IMX335_AGAIN_VS_DEFAULT,
	.step = KMB_IMX335_AGAIN_VS_STEP,
	.menu_skip_mask = 0,
};

static const struct v4l2_ctrl_config exposure_short = {
	.ops = &kmb_imx335_ctrl_ops,
	.id = V4L2_CID_EXPOSURE_SHORT,
	.name = "V4L2_CID_EXPOSURE_SHORT",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = KMB_IMX335_EXPOSURE_S_MIN,
	.max = KMB_IMX335_REG_MAX,
	.def = KMB_IMX335_EXPOSURE_S_DEFAULT,
	.step = KMB_IMX335_EXPOSURE_S_STEP,
	.menu_skip_mask = 0,
};

static const struct v4l2_ctrl_config exposure_very_short = {
	.ops = &kmb_imx335_ctrl_ops,
	.id = V4L2_CID_EXPOSURE_VERY_SHORT,
	.name = "V4L2_CID_EXPOSURE_VERY_SHORT",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = KMB_IMX335_EXPOSURE_VS_MIN,
	.max = KMB_IMX335_REG_MAX,
	.def = KMB_IMX335_EXPOSURE_VS_DEFAULT,
	.step = KMB_IMX335_EXPOSURE_VS_STEP,
	.menu_skip_mask = 0,
};

static const struct v4l2_ctrl_config row_time = {
	.ops = &kmb_imx335_ctrl_ops,
	.id = V4L2_CID_ROW_TIME_NS,
	.name = "V4L2_CID_ROW_TIME_NS",
	.type = V4L2_CTRL_TYPE_INTEGER,
	.min = KMB_IMX335_REG_MIN,
	.max = KMB_IMX335_REG_MAX,
	.def = KMB_IMX335_REG_MIN,
	.step = 1,
	.menu_skip_mask = 0,
};

/**
 * kmb_imx335_get_camera_mode_by_fmt - Get the most appropriate camera
 *         mode that meets the code and resolution criteria
 * @kmb_imx335: pointer to kmb_imx335 device
 * @code: media bus format code
 * @width: frame width
 * @height: frame height
 *
 * Return: pointer to the most appropriate camera mode
 */
static const struct kmb_imx335_mode *
kmb_imx335_get_camera_mode_by_fmt(struct kmb_imx335 *kmb_imx335, u32 code,
				  u32 width, u32 height)
{
	const struct kmb_imx335_mode *mode = kmb_imx335->supported_modes;
	int i;

	for (i = 0; i < kmb_imx335->num_supported_modes; i++) {
		if (mode[i].code == code && mode[i].width == width &&
		    mode[i].height == height)
			return &mode[i];
	}

	return NULL;
}

/**
 * kmb_imx335_filter_supported_modes - Filter supported sensor modes
 * @kmb_imx335: pointer to kmb_imx335 device
 *
 * Filter supported sensor modes based on FPS
 *
 * Return: 0 if successful
 */
static int kmb_imx335_filter_supported_modes(struct kmb_imx335 *kmb_imx335)
{
	static const struct kmb_imx335_mode *modes;
	int num_modes;
	int i;

	kmb_imx335->supported_modes = NULL;

	switch (kmb_imx335->streaming_mode) {
	case KMB_IMX335_2DOL_HDR:
		modes = hdr_2dol_supported_modes;
		num_modes = ARRAY_SIZE(hdr_2dol_supported_modes);
		break;
	case KMB_IMX335_3DOL_HDR:
		modes = hdr_3dol_supported_modes;
		num_modes = ARRAY_SIZE(hdr_3dol_supported_modes);
		break;
	default:
		modes = supported_modes;
		num_modes = ARRAY_SIZE(supported_modes);
		break;
	}

	/* If fps is not set use default sensor mode */
	if (!kmb_imx335->fps) {
		kmb_imx335->fps = modes[0].fps.def;
		kmb_imx335->supported_modes = &modes[0];
		kmb_imx335->num_supported_modes = 1;
		return 0;
	}

	/*
	 * In case where fps is set before set format, first it need to be
	 * clipped to max supported by the the sensor modes.
	 */
	kmb_imx335->fps_max = modes[0].fps.max;
	kmb_imx335->fps_min = modes[0].fps.min;
	for (i = 1; i < num_modes; i++) {
		if (kmb_imx335->fps_max < modes[i].fps.max)
			kmb_imx335->fps_max = modes[i].fps.max;
		if (kmb_imx335->fps_min > modes[i].fps.min)
			kmb_imx335->fps_min = modes[i].fps.min;
	}

	if (kmb_imx335->fps > kmb_imx335->fps_max)
		kmb_imx335->fps = kmb_imx335->fps_max;
	else if (kmb_imx335->fps < kmb_imx335->fps_min)
		kmb_imx335->fps = kmb_imx335->fps_min;

	for (i = 0; i < num_modes; i++) {
		if ((kmb_imx335->fps <= modes[i].fps.max) &&
		    (kmb_imx335->fps >= modes[i].fps.min)) {
			kmb_imx335->supported_modes = &modes[i];
			break;
		}
	}

	kmb_imx335->num_supported_modes = num_modes - i;

	if (!kmb_imx335->supported_modes || !kmb_imx335->num_supported_modes)
		return -EINVAL;

	return 0;
}

/**
 * kmb_imx335_select_camera_mode - Select the most appropriate camera mode
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: pointer to the most appropriate camera mode
 */
static const struct kmb_imx335_mode *
kmb_imx335_select_camera_mode(struct kmb_imx335 *kmb_imx335,
			      struct v4l2_subdev_format *fmt)
{
	int ret;

	switch (fmt->format.reserved[0]) {
	case 2:
		kmb_imx335->streaming_mode = KMB_IMX335_2DOL_HDR;
		break;
	case 3:
		kmb_imx335->streaming_mode = KMB_IMX335_3DOL_HDR;
		break;
	default:
		kmb_imx335->streaming_mode = KMB_IMX335_ULL;
		break;
	}

	ret = kmb_imx335_filter_supported_modes(kmb_imx335);
	if (ret < 0)
		return NULL;

	return v4l2_find_nearest_size(kmb_imx335->supported_modes,
		kmb_imx335->num_supported_modes,
		width, height, fmt->format.width,
		fmt->format.height);
}

/**
 * kmb_imx335_enum_mbus_code - Enumerate V4L2 sub-device mbus codes
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @cfg: V4L2 sub-device pad configuration
 * @code: V4L2 sub-device code enumeration need to be filled
 *
 * Return: 0 if successful
 */
static int kmb_imx335_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *cfg,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	if (code->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	mutex_lock(&kmb_imx335->mutex);
	code->code = supported_modes[code->index].code;
	mutex_unlock(&kmb_imx335->mutex);

	return 0;
}

/**
 * kmb_imx335_enum_frame_size - Enumerate V4L2 sub-device frame sizes
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @cfg: V4L2 sub-device pad configuration
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * Return: 0 if successful
 */
static int kmb_imx335_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *cfg,
				  struct v4l2_subdev_frame_size_enum *fsize)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	if (fsize->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	mutex_lock(&kmb_imx335->mutex);
	if (fsize->code != supported_modes[fsize->index].code) {
		mutex_unlock(&kmb_imx335->mutex);
		return -EINVAL;
	}

	fsize->min_width = supported_modes[fsize->index].width;
	fsize->max_width = fsize->min_width;
	fsize->min_height = supported_modes[fsize->index].height;
	fsize->max_height = fsize->min_height;
	mutex_unlock(&kmb_imx335->mutex);

	return 0;
}

/**
 * kmb_imx335_enum_frame_interval - Enumerate V4L2 sub-device frame intervals
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @cfg: V4L2 sub-device pad configuration
 * @fsize: V4L2 sub-device size enumeration need to be filled
 *
 * callback for VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL()
 *
 * Return: 0 if successful
 */
static int
kmb_imx335_enum_frame_interval(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *cfg,
			       struct v4l2_subdev_frame_interval_enum *fie)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);
	const struct kmb_imx335_mode *mode;
	int fps;
	int ret = 0;

	if (fie->pad)
		return -EINVAL;

	mutex_lock(&kmb_imx335->mutex);

	if (fie->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		if (fie->code != kmb_imx335->cur_mode->code ||
		    fie->width != kmb_imx335->cur_mode->width ||
		    fie->height != kmb_imx335->cur_mode->height) {
			ret = -EINVAL;
			goto exit_unlock;
		}

		mode = kmb_imx335->cur_mode;
	} else {
		mode = kmb_imx335_get_camera_mode_by_fmt(kmb_imx335,
				fie->code, fie->width, fie->height);
		if (!mode) {
			ret = -EINVAL;
			goto exit_unlock;
		}
	}

	fps = mode->fps.step * fie->index;
	fie->interval.numerator = 1;
	fie->interval.denominator = fps;

	if (fps < mode->fps.min) {
		fie->interval.denominator = mode->fps.min;
	} else if (fps > mode->fps.max) {
		ret = -EINVAL;
		goto exit_unlock;
	}

	dev_dbg(kmb_imx335->dev, "Enum FPS %d %d/%d", fps,
		fie->interval.numerator, fie->interval.denominator);

exit_unlock:
	mutex_unlock(&kmb_imx335->mutex);
	return ret;
}

/**
 * kmb_imx335_fill_pad_format - Fill subdevice pad format
 *                              from selected sensor mode
 * @kmb_imx335: pointer to kmb_imx335 device
 * @mode: Pointer to kmb_imx335_mode sensor mode
 * @fmt: V4L2 sub-device format need to be filled
 *
 * Return: none
 */
static void kmb_imx335_fill_pad_format(struct kmb_imx335 *kmb_imx335,
				       const struct kmb_imx335_mode *mode,
				       struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = mode->code;
	fmt->format.field = V4L2_FIELD_NONE;

	if (kmb_imx335->streaming_mode == KMB_IMX335_2DOL_HDR)
		fmt->format.reserved[0] = 2;
	else if (kmb_imx335->streaming_mode == KMB_IMX335_3DOL_HDR)
		fmt->format.reserved[0] = 3;
}

/**
 * kmb_imx335_skip_top_lines - Skip top lines containing metadata
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @lines: number of lines to be skipped
 *
 * Return: 0 if successful
 */
static int kmb_imx335_skip_top_lines(struct v4l2_subdev *sd, u32 *lines)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	*lines = kmb_imx335->cur_mode->skip_lines;

	return 0;
}

/**
 * kmb_imx335_update_controls - Update control ranges based on streaming mode
 * @kmb_imx335: pointer to kmb_imx335 device
 * @mode: pointer to kmb_imx335_mode sensor mode
 *
 * Return: none
 */
static void kmb_imx335_update_controls(struct kmb_imx335 *kmb_imx335,
					const struct kmb_imx335_mode *mode)
{
	switch (kmb_imx335->streaming_mode) {
	case KMB_IMX335_ULL:
		__v4l2_ctrl_s_ctrl(kmb_imx335->vblank_ctrl,
				   kmb_imx335->lpfr - mode->height);
		__v4l2_ctrl_s_ctrl(kmb_imx335->hblank_ctrl,
				   mode->ppln - mode->width);
		__v4l2_ctrl_s_ctrl(kmb_imx335->row_time_ctrl,
				   mode->row_time);
		__v4l2_ctrl_modify_range(kmb_imx335->pclk_ctrl,
					mode->pclk, mode->pclk,
					1, mode->pclk);
		__v4l2_ctrl_modify_range(kmb_imx335->exp_ctrl,
					KMB_IMX335_EXPOSURE_MIN,
					kmb_imx335->lpfr - 9,
					1, KMB_IMX335_EXPOSURE_DEFAULT);
		break;
	case KMB_IMX335_2DOL_HDR:
		__v4l2_ctrl_s_ctrl(kmb_imx335->vblank_ctrl,
				   kmb_imx335->lpfr - mode->height);
		__v4l2_ctrl_s_ctrl(kmb_imx335->hblank_ctrl,
				   mode->ppln - mode->width);
		__v4l2_ctrl_s_ctrl(kmb_imx335->row_time_ctrl,
				   mode->row_time);
		__v4l2_ctrl_modify_range(kmb_imx335->pclk_ctrl,
					mode->pclk, mode->pclk,
					1, mode->pclk);
		__v4l2_ctrl_modify_range(kmb_imx335->exp_ctrl,
					KMB_IMX335_EXPOSURE_S_MIN,
					kmb_imx335->lpfr - mode->lpfr[1] - 18,
					1, KMB_IMX335_EXPOSURE_DEFAULT);
		__v4l2_ctrl_modify_range(kmb_imx335->exp1_ctrl,
					KMB_IMX335_EXPOSURE_S_MIN,
					mode->lpfr[1] - 18,
					1, KMB_IMX335_EXPOSURE_S_DEFAULT);
		break;
	case KMB_IMX335_3DOL_HDR:
		__v4l2_ctrl_s_ctrl(kmb_imx335->vblank_ctrl,
				   kmb_imx335->lpfr - mode->height);
		__v4l2_ctrl_s_ctrl(kmb_imx335->hblank_ctrl,
				   mode->ppln - mode->width);
		__v4l2_ctrl_s_ctrl(kmb_imx335->row_time_ctrl,
				   mode->row_time);
		__v4l2_ctrl_modify_range(kmb_imx335->pclk_ctrl,
					mode->pclk, mode->pclk,
					1, mode->pclk);
		__v4l2_ctrl_modify_range(kmb_imx335->exp_ctrl,
					KMB_IMX335_EXPOSURE_VS_STEP,
					kmb_imx335->lpfr - mode->lpfr[2] - 26,
					1, KMB_IMX335_EXPOSURE_DEFAULT);
		__v4l2_ctrl_modify_range(kmb_imx335->exp2_ctrl,
					KMB_IMX335_EXPOSURE_VS_STEP,
					mode->lpfr[1] - 26,
					1, KMB_IMX335_EXPOSURE_S_DEFAULT);
		__v4l2_ctrl_modify_range(kmb_imx335->exp2_ctrl,
					KMB_IMX335_EXPOSURE_VS_MIN,
					mode->lpfr[2] - mode->lpfr[1] - 26,
					1, KMB_IMX335_EXPOSURE_VS_DEFAULT);
		break;
	default:
		pr_err("No such streaming mode");
	}
}

/**
 * kmb_imx335_get_pad_format - Get subdevice pad format
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @cfg: V4L2 sub-device pad configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful
 */
static int kmb_imx335_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *cfg,
				 struct v4l2_subdev_format *fmt)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	mutex_lock(&kmb_imx335->mutex);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *framefmt;

		framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
		fmt->format = *framefmt;
	} else {
		kmb_imx335_fill_pad_format(kmb_imx335,
					   kmb_imx335->cur_mode,
					   fmt);
	}

	mutex_unlock(&kmb_imx335->mutex);

	return 0;
}

/**
 * kmb_imx335_set_pad_format - Set subdevice pad format
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @cfg: V4L2 sub-device pad configuration
 * @fmt: V4L2 sub-device format need to be set
 *
 * Return: 0 if successful
 */
static int
kmb_imx335_set_pad_format(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *cfg,
			  struct v4l2_subdev_format *fmt)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);
	const struct kmb_imx335_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;

	mutex_lock(&kmb_imx335->mutex);

	mode = kmb_imx335_select_camera_mode(kmb_imx335, fmt);
	if (!mode) {
		dev_err(sd->dev, "No camera mode was selected!");
		mutex_unlock(&kmb_imx335->mutex);
		return -EINVAL;
	}

	kmb_imx335->format_set = TRUE;

	kmb_imx335_update_fps(kmb_imx335, mode);

	kmb_imx335_fill_pad_format(kmb_imx335, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		framefmt = v4l2_subdev_get_try_format(sd, cfg, fmt->pad);
		*framefmt = fmt->format;
	} else {
		kmb_imx335_update_controls(kmb_imx335, mode);
		kmb_imx335->cur_mode = mode;
	}

	mutex_unlock(&kmb_imx335->mutex);

	return 0;
}

/**
 * kmb_imx335_start_streaming - Start sensor stream
 * @kmb_imx335: pointer to kmb_imx335 device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_start_streaming(struct kmb_imx335 *kmb_imx335)
{
	const struct kmb_imx335_reg_list *reg_list;
	int ret;

	/* Write sensor mode registers */
	reg_list = &kmb_imx335->cur_mode->reg_list;
	ret = kmb_imx335_write_regs(kmb_imx335, reg_list->regs,
				    reg_list->num_of_regs);
	if (ret) {
		dev_err(kmb_imx335->dev, "fail to write initial registers");
		return ret;
	}

	/* Setup handler will write actual exposure and gain */
	ret =  __v4l2_ctrl_handler_setup(kmb_imx335->sd.ctrl_handler);
	if (ret) {
		dev_err(kmb_imx335->dev, "fail to setup handler");
		return ret;
	}

	/* Start streaming */
	ret = kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_MODE_SELECT,
				   1, KMB_IMX335_MODE_STREAMING);
	if (ret) {
		dev_err(kmb_imx335->dev, "fail to start streaming");
		return ret;
	}

	return 0;
}

/**
 * kmb_imx335_stop_streaming - Stop sensor stream
 * @kmb_imx335: pointer to kmb_imx335 device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_stop_streaming(struct kmb_imx335 *kmb_imx335)
{
	return kmb_imx335_write_reg(kmb_imx335, KMB_IMX335_REG_MODE_SELECT,
				1, KMB_IMX335_MODE_STANDBY);
}

/**
 * kmb_imx335_set_stream - Enable sensor streaming
 * @sd: pointer to imx335 subdevice
 * @enable: Set to enable sensor streaming
 *
 * Return: 0 if successful
 */
static int kmb_imx335_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);
	int ret;

	mutex_lock(&kmb_imx335->mutex);

	if (kmb_imx335->streaming == enable) {
		mutex_unlock(&kmb_imx335->mutex);
		return 0;
	}

	if (enable) {
		ret = pm_runtime_get_sync(kmb_imx335->dev);
		if (ret)
			goto error_unlock;

		ret = kmb_imx335_start_streaming(kmb_imx335);
		if (ret)
			goto error_power_off;
	} else {
		kmb_imx335_stop_streaming(kmb_imx335);
		pm_runtime_put(kmb_imx335->dev);
	}

	kmb_imx335->streaming = enable;

	mutex_unlock(&kmb_imx335->mutex);
	return 0;

error_power_off:
	pm_runtime_put(kmb_imx335->dev);
error_unlock:
	mutex_unlock(&kmb_imx335->mutex);
	return ret;
}

/**
 * kmb_imx335_get_frame_interval - Get subdevice frame interval
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @interval: V4L2 sub-device current farme interval
 *
 * Return: 0 if successful
 */
static int kmb_imx335_get_frame_interval(struct v4l2_subdev *sd,
				struct v4l2_subdev_frame_interval *interval)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	if (interval->pad)
		return -EINVAL;

	mutex_lock(&kmb_imx335->mutex);
	interval->interval.numerator = 1;
	interval->interval.denominator = kmb_imx335->fps;
	mutex_unlock(&kmb_imx335->mutex);

	dev_dbg(kmb_imx335->dev, "Get frame interval %d/%d",
		interval->interval.numerator,
		interval->interval.denominator);

	return 0;
}

/**
 * kmb_imx335_set_frame_interval - Set subdevice frame interval
 * @sd: pointer to imx335 V4L2 sub-device structure
 * @interval: V4L2 sub-device farme interval to be set
 *
 * Return: 0 if successful
 */
static int kmb_imx335_set_frame_interval(struct v4l2_subdev *sd,
				struct v4l2_subdev_frame_interval *interval)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);
	u32 fps;
	u32 fps_max;
	u32 fps_min;

	if (interval->pad)
		return -EINVAL;

	mutex_lock(&kmb_imx335->mutex);

	/*
	 * We don't know the format here. Just store requested fps, it will be
	 * clipped on set pad format.
	 */
	if (!kmb_imx335->format_set) {
		kmb_imx335->fps = interval->interval.denominator /
				  interval->interval.numerator;
		mutex_unlock(&kmb_imx335->mutex);
		return 0;
	}

	fps_max = kmb_imx335->cur_mode->fps.max;
	fps_min = kmb_imx335->cur_mode->fps.min;

	fps = interval->interval.denominator /
	      interval->interval.numerator;
	if (fps < fps_min) {
		interval->interval.numerator = 1;
		interval->interval.denominator = fps_min;
	} else if (fps > fps_max) {
		interval->interval.numerator = 1;
		interval->interval.denominator = fps_max;
	}

	kmb_imx335->fps = interval->interval.denominator /
			  interval->interval.numerator;

	kmb_imx335_update_fps(kmb_imx335, kmb_imx335->cur_mode);

	kmb_imx335_update_controls(kmb_imx335, kmb_imx335->cur_mode);

	mutex_unlock(&kmb_imx335->mutex);

	dev_dbg(kmb_imx335->dev, "Set frame interval %d/%d",
		interval->interval.numerator,
		interval->interval.denominator);

	return 0;
}

/**
 * kmb_imx335_power_on - Sensor power on sequence
 * @kmb_imx335: imb_imx335 device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_power_on(struct kmb_imx335 *kmb_imx335)
{
	int ret;

	/* request optional reset pin */
	kmb_imx335->reset_gpio =
		gpiod_get_optional(kmb_imx335->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(kmb_imx335->reset_gpio)) {
		ret = PTR_ERR(kmb_imx335->reset_gpio);
		dev_err(kmb_imx335->dev, "failed to get reset gpio %d", ret);
		return ret;
	}

	/* Ignore the call if reset gpio is not present */
	if (kmb_imx335->reset_gpio)
		gpiod_set_value_cansleep(kmb_imx335->reset_gpio, 1);

	ret = clk_prepare_enable(kmb_imx335->inclk);
	if (ret) {
		dev_err(kmb_imx335->dev, "fail to enable inclk\n");
		goto error_reset;
	}

	usleep_range(20000, 22000);

	return 0;

error_reset:
	if (kmb_imx335->reset_gpio) {
		gpiod_set_value_cansleep(kmb_imx335->reset_gpio, 0);
		gpiod_put(kmb_imx335->reset_gpio);
		kmb_imx335->reset_gpio = NULL;
	}

	return ret;
}

/**
 * kmb_imx335_power_off - Sensor power off sequence
 * @kmb_imx335: imb_imx335 device
 */
static void kmb_imx335_power_off(struct kmb_imx335 *kmb_imx335)
{
	/* Ignore the call if reset gpio is not present */
	if (kmb_imx335->reset_gpio)
		gpiod_set_value_cansleep(kmb_imx335->reset_gpio, 0);

	clk_disable_unprepare(kmb_imx335->inclk);

	if (kmb_imx335->reset_gpio) {
		gpiod_put(kmb_imx335->reset_gpio);
		kmb_imx335->reset_gpio = NULL;
	}
}

/**
 * kmb_imx335_detect - Detect imx335 sensor
 * @kmb_imx335: pointer to kmb_imx335 device
 *
 * Return: 0 if successful, -EIO if sensor id does not match
 */
static int kmb_imx335_detect(struct kmb_imx335 *kmb_imx335)
{
	int ret;
	u32 val;

	ret = kmb_imx335_read_reg(kmb_imx335, KMB_IMX335_REG_ID, 2, &val);
	if (ret)
		return ret;

	if (val != KMB_IMX335_ID) {
		dev_err(kmb_imx335->dev, "chip id mismatch: %x!=%x",
			KMB_IMX335_ID, val);
		return -EIO;
	}

	ret = kmb_imx335_read_reg(kmb_imx335, KMB_IMX335_Y_SIZE_ID, 2, &val);
	if (ret)
		return ret;

	if (val != KMB_IMX335_Y_SIZE) {
		dev_err(kmb_imx335->dev, "y_size mismatch: %x!=%x",
			KMB_IMX335_Y_SIZE, val);
		return -EIO;
	}

	return 0;
}


/**
 * kmb_imx335_s_power - Set power core operation. Actual power is enabled on
 *                      set stream, this callback is used only for reset formats
 *                      and fps to default values.
 * @sd: pointer to kmb_imx334 sub-device.
 * @on: power on/off flag.
 *
 * Return: 0 if successful
 */
static int kmb_imx335_s_power(struct v4l2_subdev *sd, int on)
{
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	if (!on)
		return 0;

	mutex_lock(&kmb_imx335->mutex);

	kmb_imx335->format_set = FALSE;

	/* Set default sensor mode */
	kmb_imx335->fps = 0;
	kmb_imx335->cur_mode = &supported_modes[0];
	kmb_imx335->lpfr = kmb_imx335->cur_mode->lpfr[0];

	mutex_unlock(&kmb_imx335->mutex);

	return 0;

}

/* V4l2 subdevice ops */
static struct v4l2_subdev_core_ops kmb_imx335_subdev_core_ops = {
	.s_power = kmb_imx335_s_power,
};

static const struct v4l2_subdev_video_ops kmb_imx335_video_ops = {
	.s_stream = kmb_imx335_set_stream,
	.g_frame_interval = kmb_imx335_get_frame_interval,
	.s_frame_interval = kmb_imx335_set_frame_interval,
};

static const struct v4l2_subdev_pad_ops kmb_imx335_pad_ops = {
	.enum_mbus_code = kmb_imx335_enum_mbus_code,
	.get_fmt = kmb_imx335_get_pad_format,
	.set_fmt = kmb_imx335_set_pad_format,
	.enum_frame_size = kmb_imx335_enum_frame_size,
	.enum_frame_interval = kmb_imx335_enum_frame_interval,
};

static const struct v4l2_subdev_sensor_ops kmb_imx335_sensor_ops = {
	.g_skip_top_lines = kmb_imx335_skip_top_lines,
};

static const struct v4l2_subdev_ops kmb_imx335_subdev_ops = {
	.core = &kmb_imx335_subdev_core_ops,
	.video = &kmb_imx335_video_ops,
	.pad = &kmb_imx335_pad_ops,
	.sensor = &kmb_imx335_sensor_ops,
};

static const struct media_entity_operations kmb_imx335_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_internal_ops kmb_imx335_internal_ops = {
	.open = kmb_imx335_open,
};

/**
 * kmb_imx335_init_controls - Initialize sensor subdevice controls
 * @kmb_imx335: pointer to kmb_imx335 device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_init_controls(struct kmb_imx335 *kmb_imx335)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &kmb_imx335->ctrl_handler;
	const struct kmb_imx335_mode *mode = kmb_imx335->cur_mode;
	u32 hblank;
	u32 vblank;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 10);
	if (ret)
		return ret;

	/* Serialize controls with sensor device */
	ctrl_hdlr->lock = &kmb_imx335->mutex;

	/* Initialize exposure and gain */
	kmb_imx335->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						 &kmb_imx335_ctrl_ops,
						 V4L2_CID_EXPOSURE,
						 KMB_IMX335_EXPOSURE_MIN,
						 mode->lpfr[0],
						 KMB_IMX335_EXPOSURE_STEP,
						 KMB_IMX335_EXPOSURE_DEFAULT);

	kmb_imx335->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						   &kmb_imx335_ctrl_ops,
						   V4L2_CID_ANALOGUE_GAIN,
						   KMB_IMX335_AGAIN_MIN,
						   KMB_IMX335_AGAIN_MAX,
						   KMB_IMX335_AGAIN_STEP,
						   KMB_IMX335_AGAIN_DEFAULT);

	kmb_imx335->exp1_ctrl = v4l2_ctrl_new_custom(ctrl_hdlr,
						     &exposure_short,
						     NULL);

	kmb_imx335->again1_ctrl = v4l2_ctrl_new_custom(ctrl_hdlr,
						       &again_short,
						       NULL);

	kmb_imx335->exp2_ctrl = v4l2_ctrl_new_custom(ctrl_hdlr,
						     &exposure_very_short,
						     NULL);

	kmb_imx335->again2_ctrl = v4l2_ctrl_new_custom(ctrl_hdlr,
						       &again_very_short,
						       NULL);

	v4l2_ctrl_cluster(6, &kmb_imx335->exp_ctrl);

	/* Read only controls */
	kmb_imx335->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						  &kmb_imx335_ctrl_ops,
						  V4L2_CID_PIXEL_RATE,
						  mode->pclk, mode->pclk,
						  1, mode->pclk);
	if (kmb_imx335->pclk_ctrl)
		kmb_imx335->pclk_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->lpfr[0] - mode->height;
	kmb_imx335->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						    &kmb_imx335_ctrl_ops,
						    V4L2_CID_VBLANK,
						    KMB_IMX335_REG_MIN,
						    KMB_IMX335_REG_MAX,
						    1, vblank);
	if (kmb_imx335->vblank_ctrl)
		kmb_imx335->vblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	hblank = mode->ppln - mode->width;
	kmb_imx335->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						    &kmb_imx335_ctrl_ops,
						    V4L2_CID_HBLANK,
						    KMB_IMX335_REG_MIN,
						    KMB_IMX335_REG_MAX,
						    1, hblank);
	if (kmb_imx335->hblank_ctrl)
		kmb_imx335->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	kmb_imx335->row_time_ctrl = v4l2_ctrl_new_custom(ctrl_hdlr,
							 &row_time,
							 NULL);
	if (kmb_imx335->row_time_ctrl)
		kmb_imx335->row_time_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(kmb_imx335->dev, "control init failed: %d", ret);
		goto error;
	}

	kmb_imx335->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

/* --------------- probe as i2c device -------------------- */

/**
 * kmb_imx335_i2c_resume - PM resume callback
 * @dev: i2c device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	return kmb_imx335_power_on(kmb_imx335);
}

/**
 * kmb_imx335_i2c_suspend - PM suspend callback
 * @dev: i2c device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	kmb_imx335_power_off(kmb_imx335);

	return 0;
}

/**
 * kmb_imx335_i2c_probe - I2C client device binding
 * @client: pointer to i2c client device
 * @id: pointer to i2c device id
 *
 * Return: 0 if successful
 */
static int kmb_imx335_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct kmb_imx335 *kmb_imx335;
	int ret;

	kmb_imx335 = devm_kzalloc(&client->dev, sizeof(*kmb_imx335),
				  GFP_KERNEL);
	if (!kmb_imx335)
		return -ENOMEM;

	mutex_init(&kmb_imx335->mutex);

	kmb_imx335->dev = &client->dev;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&kmb_imx335->sd, client, &kmb_imx335_subdev_ops);

	/* Get sensor input clock */
	kmb_imx335->inclk = devm_clk_get(&client->dev, "inclk");
	if (IS_ERR(kmb_imx335->inclk)) {
		ret = PTR_ERR(kmb_imx335->inclk);
		dev_err(&client->dev, "could not get inclk");
		goto error_mutex_destroy;
	}

	ret = clk_set_rate(kmb_imx335->inclk, KMB_IMX335_INCLK_RATE);
	if (ret) {
		dev_err(&client->dev, "could not set inclk frequency\n");
		goto error_mutex_destroy;
	}

	ret = kmb_imx335_power_on(kmb_imx335);
	if (ret) {
		dev_err(&client->dev, "failed to power-on the sensor\n");
		goto error_mutex_destroy;
	}

	/* Check module identity */
	ret = kmb_imx335_detect(kmb_imx335);
	if (ret) {
		dev_err(&client->dev, "failed to find sensor: %d", ret);
		goto error_sensor_power_off;
	}

	/* Set default mode to max resolution */
	kmb_imx335->cur_mode = &supported_modes[0];
	kmb_imx335->fps = kmb_imx335->cur_mode->fps.def;
	kmb_imx335->lpfr = kmb_imx335->cur_mode->lpfr[0];

	ret = kmb_imx335_init_controls(kmb_imx335);
	if (ret) {
		dev_err(&client->dev, "failed to init controls: %d", ret);
		goto error_sensor_power_off;
	}

	/* Initialize subdev */
	kmb_imx335->sd.internal_ops = &kmb_imx335_internal_ops;
	kmb_imx335->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	kmb_imx335->sd.entity.ops = &kmb_imx335_subdev_entity_ops;
	kmb_imx335->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	kmb_imx335->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&kmb_imx335->sd.entity, 1,
				     &kmb_imx335->pad);
	if (ret) {
		dev_err(&client->dev, "failed to init entity pads: %d", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&kmb_imx335->sd);
	if (ret < 0) {
		dev_err(&client->dev,
				"failed to register async subdev: %d", ret);
		goto error_media_entity;
	}

	ret = kmb_imx335_filter_supported_modes(kmb_imx335);
	if (ret < 0) {
		dev_err(kmb_imx335->dev,
				"failed to find supported mode: %d", ret);
		goto error_media_entity;
	}

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

error_media_entity:
	media_entity_cleanup(&kmb_imx335->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(kmb_imx335->sd.ctrl_handler);
error_sensor_power_off:
	kmb_imx335_power_off(kmb_imx335);
error_mutex_destroy:
	mutex_destroy(&kmb_imx335->mutex);

	return ret;
}

/**
 * kmb_imx335_i2c_remove - I2C client device unbinding
 * @client: pointer to I2C client device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_i2c_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct kmb_imx335 *kmb_imx335 = to_kmb_imx335(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	pm_runtime_suspended(&client->dev);

	mutex_destroy(&kmb_imx335->mutex);

	return 0;
}

static const struct dev_pm_ops kmb_imx335_i2c_pm_ops = {
	SET_RUNTIME_PM_OPS(kmb_imx335_i2c_suspend, kmb_imx335_i2c_resume, NULL)
};

static const struct i2c_device_id kmb_imx335_i2c_id_table[] = {
	{KMB_IMX335_DRV_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, kmb_imx335_i2c_id_table);

static struct i2c_driver kmb_imx335_i2c_driver = {
	.probe = kmb_imx335_i2c_probe,
	.remove = kmb_imx335_i2c_remove,
	.id_table = kmb_imx335_i2c_id_table,
	.driver = {
		.owner = THIS_MODULE,
		.pm = &kmb_imx335_i2c_pm_ops,
		.name = KMB_IMX335_DRV_NAME,
	},
};

/* --------------- probe as platform device ----------------- */

/**
 * kmb_imx335_platform_resume - PM resume callback
 * @dev: platform device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_platform_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct kmb_imx335 *kmb_imx335 = platform_get_drvdata(pdev);

	return kmb_imx335_power_on(kmb_imx335);
}

/**
 * kmb_imx335_platform_suspend - PM suspend callback
 * @dev: platform device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_platform_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct kmb_imx335 *kmb_imx335 = platform_get_drvdata(pdev);

	kmb_imx335_power_off(kmb_imx335);

	return 0;
}

/**
 * kmb_imx335_get_i2c_client - Get I2C client
 * @kmb_imx335: pointer to kmb_imx335 device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_get_i2c_client(struct kmb_imx335 *kmb_imx335)
{
	struct i2c_board_info info = {
		I2C_BOARD_INFO("kmb-imx335-sensor-p", 0x1A)};
	const unsigned short addr_list[] = {0x1A, 0x10,
		0x36, 0x37, I2C_CLIENT_END};
	struct i2c_adapter *i2c_adp;
	struct device_node *phandle;

	phandle = of_parse_phandle(kmb_imx335->dev->of_node, "i2c-bus", 0);
	if (!phandle)
		return -ENODEV;

	i2c_adp = of_get_i2c_adapter_by_node(phandle);
	of_node_put(phandle);
	if (!i2c_adp)
		return -EPROBE_DEFER;

	kmb_imx335->client = i2c_new_scanned_device(i2c_adp, &info, addr_list,
						    NULL);
	i2c_put_adapter(i2c_adp);
	if (IS_ERR(kmb_imx335->client))
		return PTR_ERR(kmb_imx335->client);

	dev_dbg(kmb_imx335->dev, "Detected on i2c address %x", info.addr);
	return 0;
}

/**
 * kmb_imx335_pdev_probe - Platform device binding
 * @pdev: pointer to platform device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_pdev_probe(struct platform_device *pdev)
{
	struct kmb_imx335 *kmb_imx335;
	struct gpio_descs *detect_gpios;
	int ret;

	kmb_imx335 = devm_kzalloc(&pdev->dev, sizeof(*kmb_imx335), GFP_KERNEL);
	if (!kmb_imx335)
		return -ENOMEM;

	platform_set_drvdata(pdev, kmb_imx335);

	mutex_init(&kmb_imx335->mutex);

	kmb_imx335->dev = &pdev->dev;

	/* Initialize subdev */
	v4l2_subdev_init(&kmb_imx335->sd, &kmb_imx335_subdev_ops);
	kmb_imx335->sd.owner = pdev->dev.driver->owner;
	kmb_imx335->sd.dev = &pdev->dev;

	/* request optional detect pins */
	detect_gpios =
		gpiod_get_array_optional(&pdev->dev, "detect", GPIOD_OUT_LOW);
	if (IS_ERR(detect_gpios)) {
		ret = PTR_ERR(detect_gpios);
		dev_info(&pdev->dev, "failed to get detect gpios %d", ret);
		/* Defer the probe if detect gpios are busy */
		ret = (ret == -EBUSY) ? -EPROBE_DEFER : ret;
		goto error_mutex_destroy;
	}

	/* Get sensor input clock */
	kmb_imx335->inclk = devm_clk_get(&pdev->dev, "inclk");
	if (IS_ERR(kmb_imx335->inclk)) {
		ret = PTR_ERR(kmb_imx335->inclk);
		dev_err(&pdev->dev, "could not get inclk");
		goto error_put_detect_gpios;
	}

	ret = clk_set_rate(kmb_imx335->inclk, KMB_IMX335_INCLK_RATE);
	if (ret) {
		dev_err(&pdev->dev, "could not set inclk frequency\n");
		goto error_put_detect_gpios;
	}

	ret = kmb_imx335_power_on(kmb_imx335);
	if (ret) {
		dev_dbg(&pdev->dev, "failed to power-on the sensor %d", ret);
		/* Defer the probe if resourcess are busy */
		ret = (ret == -EBUSY) ? -EPROBE_DEFER : ret;
		goto error_put_detect_gpios;
	}

	ret = kmb_imx335_get_i2c_client(kmb_imx335);
	if (ret) {
		dev_dbg(&pdev->dev, "failed to get i2c %d\n", ret);
		goto error_sensor_power_off;
	}

	v4l2_set_subdevdata(&kmb_imx335->sd, kmb_imx335->client);
	i2c_set_clientdata(kmb_imx335->client, &kmb_imx335->sd);
	v4l2_i2c_subdev_set_name(&kmb_imx335->sd, kmb_imx335->client,
				 KMB_IMX335_DRV_NAME, pdev->name);

	/* Check module identity */
	ret = kmb_imx335_detect(kmb_imx335);
	if (ret) {
		dev_err(&pdev->dev, "failed to find sensor: %d", ret);
		goto error_unregister_i2c_dev;
	}

	/* Set default mode to max resolution */
	kmb_imx335->cur_mode = &supported_modes[0];
	kmb_imx335->fps = kmb_imx335->cur_mode->fps.def;
	kmb_imx335->lpfr = kmb_imx335->cur_mode->lpfr[0];

	ret = kmb_imx335_init_controls(kmb_imx335);
	if (ret) {
		dev_err(&pdev->dev, "failed to init controls: %d", ret);
		goto error_unregister_i2c_dev;
	}

	/* Initialize subdev */
	kmb_imx335->sd.internal_ops = &kmb_imx335_internal_ops;
	kmb_imx335->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	kmb_imx335->sd.entity.ops = &kmb_imx335_subdev_entity_ops;
	kmb_imx335->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	/* Initialize source pad */
	kmb_imx335->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&kmb_imx335->sd.entity, 1,
				     &kmb_imx335->pad);
	if (ret) {
		dev_err(&pdev->dev, "failed to init entity pads: %d", ret);
		goto error_handler_free;
	}

	ret = v4l2_async_register_subdev_sensor(&kmb_imx335->sd);
	if (ret < 0) {
		dev_err(&pdev->dev,
				"failed to register async subdev: %d", ret);
		goto error_media_entity;
	}

	ret = kmb_imx335_filter_supported_modes(kmb_imx335);
	if (ret < 0) {
		dev_err(&pdev->dev,
				"failed to find supported mode: %d", ret);
		goto error_media_entity;
	}

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	pm_runtime_idle(&pdev->dev);

	if (detect_gpios)
		gpiod_put_array(detect_gpios);

	dev_info(&pdev->dev, "Probe success!");
	return 0;

error_media_entity:
	media_entity_cleanup(&kmb_imx335->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(kmb_imx335->sd.ctrl_handler);
error_unregister_i2c_dev:
	if (kmb_imx335->client)
		i2c_unregister_device(kmb_imx335->client);
error_sensor_power_off:
	kmb_imx335_power_off(kmb_imx335);
error_put_detect_gpios:
	if (detect_gpios)
		gpiod_put_array(detect_gpios);
error_mutex_destroy:
	mutex_destroy(&kmb_imx335->mutex);

	return ret;
}

/**
 * kmb_imx335_pdev_remove - Platform device unbinding
 * @pdev: pointer to platform device
 *
 * Return: 0 if successful
 */
static int kmb_imx335_pdev_remove(struct platform_device *pdev)
{
	struct kmb_imx335 *kmb_imx335 = platform_get_drvdata(pdev);
	struct v4l2_subdev *sd = &kmb_imx335->sd;

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&pdev->dev);
	pm_runtime_suspended(&pdev->dev);

	if (kmb_imx335->client)
		i2c_unregister_device(kmb_imx335->client);

	mutex_destroy(&kmb_imx335->mutex);

	return 0;
}

static const struct dev_pm_ops kmb_imx335_platform_pm_ops = {
	SET_RUNTIME_PM_OPS(kmb_imx335_platform_suspend,
			   kmb_imx335_platform_resume, NULL)
};

static const struct of_device_id kmb_imx335_id_table[] = {
	{.compatible = "intel,kmb-imx335-sensor-p"},
	{}
};
MODULE_DEVICE_TABLE(of, kmb_imx335_id_table);

static struct platform_driver kmb_imx335_platform_driver = {
	.probe	= kmb_imx335_pdev_probe,
	.remove = kmb_imx335_pdev_remove,
	.driver = {
		.name = KMB_IMX335_DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &kmb_imx335_platform_pm_ops,
		.of_match_table = kmb_imx335_id_table,
	}
};

static int __init kmb_imx335_init(void)
{
	int ret;

	ret = i2c_add_driver(&kmb_imx335_i2c_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&kmb_imx335_platform_driver);
	if (ret)
		i2c_del_driver(&kmb_imx335_i2c_driver);

	return ret;
}

static void __exit kmb_imx335_exit(void)
{
	i2c_del_driver(&kmb_imx335_i2c_driver);
	platform_driver_unregister(&kmb_imx335_platform_driver);
}

module_init(kmb_imx335_init);
module_exit(kmb_imx335_exit);

MODULE_DESCRIPTION("KeemBay imx335 Sensor driver");
MODULE_LICENSE("GPL v2");
