/**************************************************************************

 Copyright 2006 Dave Airlie <airlied@linux.ie>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
THE COPYRIGHT HOLDERS AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/** @file
 * SDVO support for i915 and newer chipsets.
 *
 * The SDVO outputs send digital display data out over the PCIE bus to display
 * cards implementing a defined interface.  These cards may have DVI, TV, CRT,
 * or other outputs on them.
 *
 * The system has two SDVO channels, which may be used for SDVO chips on the
 * motherboard, or in the external cards.  The two channels may also be used
 * in a ganged mode to provide higher bandwidth to a single output.  Currently,
 * this code doesn't deal with either ganged mode or more than one SDVO output.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "compiler.h"
#include "i830.h"
#include "i830_display.h"
#include "i810_reg.h"
#include "i830_sdvo_regs.h"

/** SDVO driver private structure. */
struct i830_sdvo_priv {
    /** SDVO device on SDVO I2C bus. */
    I2CDevRec d;

    /** Register for the SDVO device: SDVOB or SDVOC */
    int output_device;

    /** Active outputs controlled by this SDVO output */
    struct i830_sdvo_output_flags active_outputs;

    /**
     * Capabilities of the SDVO device returned by i830_sdvo_get_capabilities()
     */
    struct i830_sdvo_caps caps;

    /** Pixel clock limitations reported by the SDVO device, in kHz */
    int pixel_clock_min, pixel_clock_max;

    /** State for save/restore */
    /** @{ */
    int save_sdvo_mult;
    struct i830_sdvo_output_flags save_active_outputs;
    struct i830_sdvo_dtd save_input_dtd_1, save_input_dtd_2;
    struct i830_sdvo_dtd save_output_dtd;
    CARD32 save_SDVOX;
    /** @} */
};

/** Read a single byte from the given address on the SDVO device. */
static Bool i830_sdvo_read_byte(I830OutputPtr output, int addr,
				unsigned char *ch)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    if (!xf86I2CReadByte(&dev_priv->d, addr, ch)) {
	xf86DrvMsg(output->pI2CBus->scrnIndex, X_ERROR,
		   "Unable to read from %s slave %d.\n",
		   output->pI2CBus->BusName, dev_priv->d.SlaveAddr);
	return FALSE;
    }
    return TRUE;
}

/** Write a single byte to the given address on the SDVO device. */
static Bool i830_sdvo_write_byte(I830OutputPtr output,
				 int addr, unsigned char ch)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    if (!xf86I2CWriteByte(&dev_priv->d, addr, ch)) {
	xf86DrvMsg(output->pI2CBus->scrnIndex, X_ERROR,
		   "Unable to write to %s Slave %d.\n",
		   output->pI2CBus->BusName, dev_priv->d.SlaveAddr);
	return FALSE;
    }
    return TRUE;
}


#define SDVO_CMD_NAME_ENTRY(cmd) {cmd, #cmd}
/** Mapping of command numbers to names, for debug output */
const struct _sdvo_cmd_name {
    CARD8 cmd;
    char *name;
} sdvo_cmd_names[] = {
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_RESET),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_DEVICE_CAPS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_FIRMWARE_REV),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_TRAINED_INPUTS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_ACTIVE_OUTPUTS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_ACTIVE_OUTPUTS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_IN_OUT_MAP),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_IN_OUT_MAP),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_ATTACHED_DISPLAYS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_HOT_PLUG_SUPPORT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_ACTIVE_HOT_PLUG),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_ACTIVE_HOT_PLUG),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_INTERRUPT_EVENT_SOURCE),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_TARGET_INPUT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_TARGET_OUTPUT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_INPUT_TIMINGS_PART1),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_INPUT_TIMINGS_PART2),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_INPUT_TIMINGS_PART1),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_INPUT_TIMINGS_PART2),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_INPUT_TIMINGS_PART1),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_OUTPUT_TIMINGS_PART1),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_OUTPUT_TIMINGS_PART2),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_OUTPUT_TIMINGS_PART1),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_OUTPUT_TIMINGS_PART2),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_CREATE_PREFERRED_INPUT_TIMING),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART1),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART2),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_INPUT_PIXEL_CLOCK_RANGE),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_OUTPUT_PIXEL_CLOCK_RANGE),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_SUPPORTED_CLOCK_RATE_MULTS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_CLOCK_RATE_MULT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_CLOCK_RATE_MULT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_SUPPORTED_TV_FORMATS),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_GET_TV_FORMAT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_TV_FORMAT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_TV_RESOLUTION_SUPPORT),
    SDVO_CMD_NAME_ENTRY(SDVO_CMD_SET_CONTROL_BUS_SWITCH),
};

/**
 * Writes out the data given in args (up to 8 bytes), followed by the opcode.
 */
static void
i830_sdvo_write_cmd(I830OutputPtr output, CARD8 cmd, void *args, int args_len)
{
    int i;

    /* Write the SDVO command logging */
    xf86DrvMsg(output->pI2CBus->scrnIndex, X_INFO, "SDVO: W: %02X ", cmd);
    for (i = 0; i < args_len; i++)
	LogWrite(1, "%02X ", ((CARD8 *)args)[i]);
    for (; i < 8; i++)
	LogWrite(1, "   ");
    for (i = 0; i < sizeof(sdvo_cmd_names) / sizeof(sdvo_cmd_names[0]); i++) {
	if (cmd == sdvo_cmd_names[i].cmd) {
	    LogWrite(1, "(%s)", sdvo_cmd_names[i].name);
	    break;
	}
    }
    if (i == sizeof(sdvo_cmd_names) / sizeof(sdvo_cmd_names[0]))
	LogWrite(1, "(%02X)", cmd);
    LogWrite(1, "\n");

    /* send the output regs */
    for (i = 0; i < args_len; i++) {
	i830_sdvo_write_byte(output, SDVO_I2C_ARG_0 - i, ((CARD8 *)args)[i]);
    }
    /* blast the command reg */
    i830_sdvo_write_byte(output, SDVO_I2C_OPCODE, cmd);
}

static const char *cmd_status_names[] = {
	"Power on",
	"Success",
	"Not supported",
	"Invalid arg",
	"Pending",
	"Target not specified",
	"Scaling not supported"
};

/**
 * Reads back response_len bytes from the SDVO device, and returns the status.
 */
static CARD8
i830_sdvo_read_response(I830OutputPtr output, void *response, int response_len)
{
    int i;
    CARD8 status;

    /* Read the command response */
    for (i = 0; i < response_len; i++) {
	i830_sdvo_read_byte(output, SDVO_I2C_RETURN_0 + i,
			    &((CARD8 *)response)[i]);
    }

    /* Read the return status */
    i830_sdvo_read_byte(output, SDVO_I2C_CMD_STATUS, &status);

    /* Write the SDVO command logging */
    xf86DrvMsg(output->pI2CBus->scrnIndex, X_INFO,
	       "SDVO: R: ");
    for (i = 0; i < response_len; i++)
	LogWrite(1, "%02X ", ((CARD8 *)response)[i]);
    for (; i < 8; i++)
	LogWrite(1, "   ");
    if (status <= SDVO_CMD_STATUS_SCALING_NOT_SUPP)
    {
	LogWrite(1, "(%s)", cmd_status_names[status]);
    } else {
	LogWrite(1, "(??? %d)", status);
    }
    LogWrite(1, "\n");

    return status;
}

int
i830_sdvo_get_pixel_multiplier(DisplayModePtr pMode)
{
    if (pMode->Clock >= 100000)
	return 1;
    else if (pMode->Clock >= 50000)
	return 2;
    else
	return 4;
}

/* Sets the control bus switch to either point at one of the DDC buses or the
 * PROM.  It resets from the DDC bus back to internal registers at the next I2C
 * STOP.  PROM access is terminated by accessing an internal register.
 */
static void
i830_sdvo_set_control_bus_switch(I830OutputPtr output, CARD8 target)
{
    i830_sdvo_write_cmd(output, SDVO_CMD_SET_CONTROL_BUS_SWITCH, &target, 1);
}

static Bool
i830_sdvo_set_target_input(I830OutputPtr output, Bool target_0, Bool target_1)
{
    struct i830_sdvo_set_target_input_args targets = {0};
    CARD8 status;

    if (target_0 && target_1)
	return SDVO_CMD_STATUS_NOTSUPP;

    if (target_1)
	targets.target_1 = 1;

    i830_sdvo_write_cmd(output, SDVO_CMD_SET_TARGET_INPUT, &targets,
			sizeof(targets));

    status = i830_sdvo_read_response(output, NULL, 0);

    return (status == SDVO_CMD_STATUS_SUCCESS);
}

/**
 * Return whether each input is trained.
 *
 * This function is making an assumption about the layout of the response,
 * which should be checked against the docs.
 */
static Bool
i830_sdvo_get_trained_inputs(I830OutputPtr output, Bool *input_1, Bool *input_2)
{
    struct i830_sdvo_get_trained_inputs_response response;
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_TRAINED_INPUTS, NULL, 0);

    status = i830_sdvo_read_response(output, &response, sizeof(response));
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    *input_1 = response.input0_trained;
    *input_2 = response.input1_trained;

    return TRUE;
}

static Bool
i830_sdvo_get_active_outputs(I830OutputPtr output,
			     struct i830_sdvo_output_flags *outputs)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_ACTIVE_OUTPUTS, NULL, 0);
    status = i830_sdvo_read_response(output, outputs, sizeof(*outputs));

    return (status == SDVO_CMD_STATUS_SUCCESS);
}

static Bool
i830_sdvo_set_active_outputs(I830OutputPtr output,
			     struct i830_sdvo_output_flags *outputs)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_SET_ACTIVE_OUTPUTS, outputs,
			sizeof(*outputs));
    status = i830_sdvo_read_response(output, NULL, 0);

    return (status == SDVO_CMD_STATUS_SUCCESS);
}

/**
 * Returns the pixel clock range limits of the current target input in kHz.
 */
static Bool
i830_sdvo_get_input_pixel_clock_range(I830OutputPtr output, int *clock_min,
				      int *clock_max)
{
    struct i830_sdvo_pixel_clock_range clocks;
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_INPUT_PIXEL_CLOCK_RANGE, NULL, 0);

    status = i830_sdvo_read_response(output, &clocks, sizeof(clocks));

    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    /* Convert the values from units of 10 kHz to kHz. */
    *clock_min = clocks.min * 10;
    *clock_max = clocks.max * 10;

    return TRUE;
}

static Bool
i830_sdvo_set_target_output(I830OutputPtr output,
			    struct i830_sdvo_output_flags *outputs)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_SET_TARGET_OUTPUT, outputs,
			sizeof(*outputs));

    status = i830_sdvo_read_response(output, NULL, 0);

    return (status == SDVO_CMD_STATUS_SUCCESS);
}

/** Fetches either input or output timings to *dtd, depending on cmd. */
static Bool
i830_sdvo_get_timing(I830OutputPtr output, CARD8 cmd, struct i830_sdvo_dtd *dtd)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, cmd, NULL, 0);

    status = i830_sdvo_read_response(output, &dtd->part1, sizeof(dtd->part1));
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    i830_sdvo_write_cmd(output, cmd + 1, NULL, 0);

    status = i830_sdvo_read_response(output, &dtd->part2, sizeof(dtd->part2));
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    return TRUE;
}

static Bool
i830_sdvo_get_input_timing(I830OutputPtr output, struct i830_sdvo_dtd *dtd)
{
    return i830_sdvo_get_timing(output, SDVO_CMD_GET_INPUT_TIMINGS_PART1, dtd);
}

static Bool
i830_sdvo_get_output_timing(I830OutputPtr output, struct i830_sdvo_dtd *dtd)
{
    return i830_sdvo_get_timing(output, SDVO_CMD_GET_OUTPUT_TIMINGS_PART1, dtd);
}

/** Sets either input or output timings from *dtd, depending on cmd. */
static Bool
i830_sdvo_set_timing(I830OutputPtr output, CARD8 cmd, struct i830_sdvo_dtd *dtd)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, cmd, &dtd->part1, sizeof(dtd->part1));
    status = i830_sdvo_read_response(output, NULL, 0);
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    i830_sdvo_write_cmd(output, cmd + 1, &dtd->part2, sizeof(dtd->part2));
    status = i830_sdvo_read_response(output, NULL, 0);
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    return TRUE;
}

static Bool
i830_sdvo_set_input_timing(I830OutputPtr output, struct i830_sdvo_dtd *dtd)
{
    return i830_sdvo_set_timing(output, SDVO_CMD_SET_INPUT_TIMINGS_PART1, dtd);
}

static Bool
i830_sdvo_set_output_timing(I830OutputPtr output, struct i830_sdvo_dtd *dtd)
{
    return i830_sdvo_set_timing(output, SDVO_CMD_SET_OUTPUT_TIMINGS_PART1, dtd);
}

#if 0
static Bool
i830_sdvo_create_preferred_input_timing(I830OutputPtr output, CARD16 clock,
					CARD16 width, CARD16 height)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;
    struct i830_sdvo_preferred_input_timing_args args;

    args.clock = clock;
    args.width = width;
    args.height = height;
    i830_sdvo_write_cmd(output, SDVO_CMD_CREATE_PREFERRED_INPUT_TIMING,
			&args, sizeof(args));
    status = i830_sdvo_read_response(output, NULL, 0);
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    return TRUE;
}

static Bool
i830_sdvo_get_preferred_input_timing(I830OutputPtr output,
				     struct i830_sdvo_dtd *dtd)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART1,
			NULL, 0);

    status = i830_sdvo_read_response(output, &dtd->part1, sizeof(dtd->part1));
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART2,
			NULL, 0);

    status = i830_sdvo_read_response(output, &dtd->part2, sizeof(dtd->part2));
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    return TRUE;
}
#endif

/** Returns the SDVO_CLOCK_RATE_MULT_* for the current clock multiplier */
static int
i830_sdvo_get_clock_rate_mult(I830OutputPtr output)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;
    CARD8 response;
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_CLOCK_RATE_MULT, NULL, 0);
    status = i830_sdvo_read_response(output, &response, 1);

    if (status != SDVO_CMD_STATUS_SUCCESS) {
	xf86DrvMsg(dev_priv->d.pI2CBus->scrnIndex, X_ERROR,
		   "Couldn't get SDVO clock rate multiplier\n");
	return SDVO_CLOCK_RATE_MULT_1X;
    } else {
	xf86DrvMsg(dev_priv->d.pI2CBus->scrnIndex, X_INFO,
		   "Current clock rate multiplier: %d\n", response);
    }

    return response;
}

/**
 * Sets the current clock multiplier.
 *
 * This has to match with the settings in the DPLL/SDVO reg when the output
 * is actually turned on.
 */
static Bool
i830_sdvo_set_clock_rate_mult(I830OutputPtr output, CARD8 val)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_SET_CLOCK_RATE_MULT, &val, 1);
    status = i830_sdvo_read_response(output, NULL, 0);
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    return TRUE;
}

static void
i830_sdvo_pre_set_mode(ScrnInfoPtr pScrn, I830OutputPtr output,
		       DisplayModePtr mode)
{
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_sdvo_priv *dev_priv = output->dev_priv;
    CARD16 width = mode->CrtcHDisplay;
    CARD16 height = mode->CrtcVDisplay;
    CARD16 h_blank_len, h_sync_len, v_blank_len, v_sync_len;
    CARD16 h_sync_offset, v_sync_offset;
    struct i830_sdvo_dtd output_dtd;
    struct i830_sdvo_output_flags no_outputs;

    memset(&no_outputs, 0, sizeof(no_outputs));

    /* do some mode translations */
    h_blank_len = mode->CrtcHBlankEnd - mode->CrtcHBlankStart;
    h_sync_len = mode->CrtcHSyncEnd - mode->CrtcHSyncStart;

    v_blank_len = mode->CrtcVBlankEnd - mode->CrtcVBlankStart;
    v_sync_len = mode->CrtcVSyncEnd - mode->CrtcVSyncStart;

    h_sync_offset = mode->CrtcHSyncStart - mode->CrtcHBlankStart;
    v_sync_offset = mode->CrtcVSyncStart - mode->CrtcVBlankStart;

    output_dtd.part1.clock = mode->Clock / 10;
    output_dtd.part1.h_active = width & 0xff;
    output_dtd.part1.h_blank = h_blank_len & 0xff;
    output_dtd.part1.h_high = (((width >> 8) & 0xf) << 4) |
	((h_blank_len >> 8) & 0xf);
    output_dtd.part1.v_active = height & 0xff;
    output_dtd.part1.v_blank = v_blank_len & 0xff;
    output_dtd.part1.v_high = (((height >> 8) & 0xf) << 4) |
	((v_blank_len >> 8) & 0xf);

    output_dtd.part2.h_sync_off = h_sync_offset;
    output_dtd.part2.h_sync_width = h_sync_len & 0xff;
    output_dtd.part2.v_sync_off_width = (v_sync_offset & 0xf) << 4 |
	(v_sync_len & 0xf);
    output_dtd.part2.sync_off_width_high = 0;
    output_dtd.part2.dtd_flags = 0x18;
    output_dtd.part2.sdvo_flags = 0;
    output_dtd.part2.v_sync_off_width = 0;
    output_dtd.part2.reserved = 0;
    if (mode->Flags & V_PHSYNC)
	output_dtd.part2.dtd_flags |= 0x2;
    if (mode->Flags & V_PVSYNC)
	output_dtd.part2.dtd_flags |= 0x4;

    /* Turn off the screens before adjusting timings */
    i830_sdvo_set_active_outputs(output, &no_outputs);

    /* Set the output timing to the screen */
    i830_sdvo_set_target_output(output, &dev_priv->active_outputs);
    i830_sdvo_set_output_timing(output, &output_dtd);

    /* Set the input timing to the screen. Assume always input 0. */
    i830_sdvo_set_target_input(output, TRUE, FALSE);

    /* We would like to use i830_sdvo_create_preferred_input_timing() to
     * provide the device with a timing it can support, if it supports that
     * feature.  However, presumably we would need to adjust the CRTC to output
     * the preferred timing, and we don't support that currently.
     */
#if 0
    success = i830_sdvo_create_preferred_input_timing(output, clock,
						      width, height);
    if (success) {
	struct i830_sdvo_dtd *input_dtd;

	i830_sdvo_get_preferred_input_timing(output, &input_dtd);
	i830_sdvo_set_input_timing(output, &input_dtd);
    }
#else
    i830_sdvo_set_input_timing(output, &output_dtd);
#endif

    switch (i830_sdvo_get_pixel_multiplier(mode)) {
    case 1:
	i830_sdvo_set_clock_rate_mult(output, SDVO_CLOCK_RATE_MULT_1X);
	break;
    case 2:
	i830_sdvo_set_clock_rate_mult(output, SDVO_CLOCK_RATE_MULT_2X);
	break;
    case 4:
	i830_sdvo_set_clock_rate_mult(output, SDVO_CLOCK_RATE_MULT_4X);
	break;
    }

    OUTREG(SDVOC, INREG(SDVOC) & ~SDVO_ENABLE);
    OUTREG(SDVOB, INREG(SDVOB) & ~SDVO_ENABLE);
}

static void
i830_sdvo_post_set_mode(ScrnInfoPtr pScrn, I830OutputPtr output,
			DisplayModePtr mode)
{
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_sdvo_priv *dev_priv = output->dev_priv;
    Bool input1, input2;
    CARD32 dpll, sdvob, sdvoc;
    int dpll_reg = (output->pipe == 0) ? DPLL_A : DPLL_B;
    int dpll_md_reg = (output->pipe == 0) ? DPLL_A_MD : DPLL_B_MD;
    int sdvo_pixel_multiply;
    int i;
    CARD8 status;

    /* Set the SDVO control regs. */
    sdvob = INREG(SDVOB) & SDVOB_PRESERVE_MASK;
    sdvoc = INREG(SDVOC) & SDVOC_PRESERVE_MASK;
    sdvob |= SDVO_ENABLE | (9 << 19) | SDVO_BORDER_ENABLE;
    sdvoc |= 9 << 19;
    if (output->pipe == 1)
	sdvob |= SDVO_PIPE_B_SELECT;

    dpll = INREG(dpll_reg);

    sdvo_pixel_multiply = i830_sdvo_get_pixel_multiplier(mode);
    if (IS_I965G(pI830)) {
	OUTREG(dpll_md_reg, (0 << DPLL_MD_UDI_DIVIDER_SHIFT) |
	       ((sdvo_pixel_multiply - 1) << DPLL_MD_UDI_MULTIPLIER_SHIFT));
    } else if (IS_I945G(pI830) || IS_I945GM(pI830)) {
	dpll |= (sdvo_pixel_multiply - 1) << SDVO_MULTIPLIER_SHIFT_HIRES;
    } else {
	sdvob |= (sdvo_pixel_multiply - 1) << SDVO_PORT_MULTIPLY_SHIFT;
    }

    OUTREG(dpll_reg, dpll | DPLL_DVO_HIGH_SPEED);

    OUTREG(SDVOB, sdvob);
    OUTREG(SDVOC, sdvoc);

    for (i = 0; i < 2; i++)
	i830WaitForVblank(pScrn);

    status = i830_sdvo_get_trained_inputs(output, &input1, &input2);

    /* Warn if the device reported failure to sync. */
    if (status == SDVO_CMD_STATUS_SUCCESS && !input1) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "First SDVO output reported failure to sync\n");
    }

    i830_sdvo_set_active_outputs(output, &dev_priv->active_outputs);
    i830_sdvo_set_target_input(output, TRUE, FALSE);
}

static void
i830_sdvo_dpms(ScrnInfoPtr pScrn, I830OutputPtr output, int mode)
{
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    if (mode != DPMSModeOn) {
	struct i830_sdvo_output_flags no_outputs;

	memset(&no_outputs, 0, sizeof(no_outputs));

	i830_sdvo_set_active_outputs(output, &no_outputs);
	OUTREG(SDVOB, INREG(SDVOB) & ~SDVO_ENABLE);
    } else {
	i830_sdvo_set_active_outputs(output, &dev_priv->active_outputs);
	OUTREG(SDVOB, INREG(SDVOB) | SDVO_ENABLE);
    }
}

static void
i830_sdvo_save(ScrnInfoPtr pScrn, I830OutputPtr output)
{
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    /* XXX: We should save the in/out mapping. */

    dev_priv->save_sdvo_mult = i830_sdvo_get_clock_rate_mult(output);
    i830_sdvo_get_active_outputs(output, &dev_priv->save_active_outputs);

    if (dev_priv->caps.sdvo_inputs_mask & 0x1) {
       i830_sdvo_set_target_input(output, TRUE, FALSE);
       i830_sdvo_get_input_timing(output, &dev_priv->save_input_dtd_1);
    }

    if (dev_priv->caps.sdvo_inputs_mask & 0x2) {
       i830_sdvo_set_target_input(output, FALSE, TRUE);
       i830_sdvo_get_input_timing(output, &dev_priv->save_input_dtd_2);
    }

    /* XXX: We should really iterate over the enabled outputs and save each
     * one's state.
     */
    i830_sdvo_set_target_output(output, &dev_priv->save_active_outputs);
    i830_sdvo_get_output_timing(output, &dev_priv->save_output_dtd);

    dev_priv->save_SDVOX = INREG(dev_priv->output_device);
}

static void
i830_sdvo_restore(ScrnInfoPtr pScrn, I830OutputPtr output)
{
    I830Ptr pI830 = I830PTR(pScrn);
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    if (dev_priv->caps.sdvo_inputs_mask & 0x1) {
       i830_sdvo_set_target_input(output, TRUE, FALSE);
       i830_sdvo_set_input_timing(output, &dev_priv->save_input_dtd_1);
    }

    if (dev_priv->caps.sdvo_inputs_mask & 0x2) {
       i830_sdvo_set_target_input(output, FALSE, TRUE);
       i830_sdvo_set_input_timing(output, &dev_priv->save_input_dtd_2);
    }

    i830_sdvo_set_target_output(output, &dev_priv->save_active_outputs);
    i830_sdvo_set_output_timing(output, &dev_priv->save_output_dtd);

    i830_sdvo_set_clock_rate_mult(output, dev_priv->save_sdvo_mult);

    OUTREG(dev_priv->output_device, dev_priv->save_SDVOX);

    i830_sdvo_set_active_outputs(output, &dev_priv->save_active_outputs);
}

static int
i830_sdvo_mode_valid(ScrnInfoPtr pScrn, I830OutputPtr output,
		     DisplayModePtr pMode)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    if (pMode->Flags & V_DBLSCAN)
	return MODE_NO_DBLESCAN;

    if (dev_priv->pixel_clock_min > pMode->Clock)
	return MODE_CLOCK_HIGH;

    if (dev_priv->pixel_clock_max < pMode->Clock)
	return MODE_CLOCK_LOW;

    return MODE_OK;
}

static Bool
i830_sdvo_get_capabilities(I830OutputPtr output, struct i830_sdvo_caps *caps)
{
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_DEVICE_CAPS, NULL, 0);
    status = i830_sdvo_read_response(output, caps, sizeof(*caps));
    if (status != SDVO_CMD_STATUS_SUCCESS)
	return FALSE;

    return TRUE;
}

/** Forces the device over to the real I2C bus and uses its GetByte */
static Bool
i830_sdvo_ddc_i2c_get_byte(I2CDevPtr d, I2CByte *data, Bool last)
{
    I830OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
    I2CBusPtr i2cbus = output->pI2CBus, savebus;
    Bool ret;

    savebus = d->pI2CBus;
    d->pI2CBus = i2cbus;
    ret = i2cbus->I2CGetByte(d, data, last);
    d->pI2CBus = savebus;

    return ret;
}

/** Forces the device over to the real I2C bus and uses its PutByte */
static Bool
i830_sdvo_ddc_i2c_put_byte(I2CDevPtr d, I2CByte c)
{
    I830OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
    I2CBusPtr i2cbus = output->pI2CBus, savebus;
    Bool ret;

    savebus = d->pI2CBus;
    d->pI2CBus = i2cbus;
    ret = i2cbus->I2CPutByte(d, c);
    d->pI2CBus = savebus;

    return ret;
}

/**
 * Sets the control bus over to DDC before sending the start on the real I2C
 * bus.
 *
 * The control bus will flip back at the stop following the start executed
 * here.
 */
static Bool
i830_sdvo_ddc_i2c_start(I2CBusPtr b, int timeout)
{
    I830OutputPtr output = b->DriverPrivate.ptr;
    I2CBusPtr i2cbus = output->pI2CBus;

    i830_sdvo_set_control_bus_switch(output, SDVO_CONTROL_BUS_DDC2);
    return i2cbus->I2CStart(i2cbus, timeout);
}

/** Forces the device over to the real SDVO bus and sends a stop to it. */
static void
i830_sdvo_ddc_i2c_stop(I2CDevPtr d)
{
    I830OutputPtr output = d->pI2CBus->DriverPrivate.ptr;
    I2CBusPtr i2cbus = output->pI2CBus, savebus;

    savebus = d->pI2CBus;
    d->pI2CBus = i2cbus;
    i2cbus->I2CStop(d);
    d->pI2CBus = savebus;
}

/**
 * Mirrors xf86i2c I2CAddress, using the bus's (wrapped) methods rather than
 * the default methods.
 *
 * This ensures that our start commands always get wrapped with control bus
 * switches.  xf86i2c should probably be fixed to do this.
 */
static Bool
i830_sdvo_ddc_i2c_address(I2CDevPtr d, I2CSlaveAddr addr)
{
    if (d->pI2CBus->I2CStart(d->pI2CBus, d->StartTimeout)) {
	if (d->pI2CBus->I2CPutByte(d, addr & 0xFF)) {
	    if ((addr & 0xF8) != 0xF0 &&
		(addr & 0xFE) != 0x00)
		return TRUE;

	    if (d->pI2CBus->I2CPutByte(d, (addr >> 8) & 0xFF))
		return TRUE;
	}

	d->pI2CBus->I2CStop(d);
    }

    return FALSE;
}

static void
i830_sdvo_dump_cmd(I830OutputPtr output, int opcode)
{
    CARD8 response[8];

    i830_sdvo_write_cmd(output, opcode, NULL, 0);
    i830_sdvo_read_response(output, response, 8);
}

static void
i830_sdvo_dump_device(I830OutputPtr output)
{
    struct i830_sdvo_priv *dev_priv = output->dev_priv;

    ErrorF("Dump %s\n", dev_priv->d.DevName);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_DEVICE_CAPS);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_FIRMWARE_REV);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_TRAINED_INPUTS);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_ACTIVE_OUTPUTS);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_IN_OUT_MAP);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_ATTACHED_DISPLAYS);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_HOT_PLUG_SUPPORT);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_ACTIVE_HOT_PLUG);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_INTERRUPT_EVENT_SOURCE);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_INPUT_TIMINGS_PART1);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_INPUT_TIMINGS_PART2);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_OUTPUT_TIMINGS_PART1);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_OUTPUT_TIMINGS_PART2);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART1);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_PREFERRED_INPUT_TIMING_PART2);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_INPUT_PIXEL_CLOCK_RANGE);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_OUTPUT_PIXEL_CLOCK_RANGE);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_SUPPORTED_CLOCK_RATE_MULTS);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_CLOCK_RATE_MULT);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_SUPPORTED_TV_FORMATS);
    i830_sdvo_dump_cmd(output, SDVO_CMD_GET_TV_FORMAT);
}

void
i830_sdvo_dump(ScrnInfoPtr pScrn)
{
    I830Ptr pI830 = I830PTR(pScrn);
    int	i;

    for (i = 0; i < pI830->num_outputs; i++) {
	if (pI830->output[i].type == I830_OUTPUT_SDVO)
	    i830_sdvo_dump_device(&pI830->output[i]);
    }
}

/**
 * Asks the SDVO device if any displays are currently connected.
 *
 * This interface will need to be augmented, since we could potentially have
 * multiple displays connected, and the caller will also probably want to know
 * what type of display is connected.  But this is enough for the moment.
 *
 * Takes 14ms on average on my i945G.
 */
static enum detect_status
i830_sdvo_detect(ScrnInfoPtr pScrn, I830OutputPtr output)
{
    CARD8 response[2];
    CARD8 status;

    i830_sdvo_write_cmd(output, SDVO_CMD_GET_ATTACHED_DISPLAYS, NULL, 0);
    status = i830_sdvo_read_response(output, &response, 2);

    if (status != SDVO_CMD_STATUS_SUCCESS)
	return OUTPUT_STATUS_UNKNOWN;

    if (response[0] != 0 || response[1] != 0)
	return OUTPUT_STATUS_CONNECTED;
    else
	return OUTPUT_STATUS_DISCONNECTED;
}

void
i830_sdvo_init(ScrnInfoPtr pScrn, int output_device)
{
    I830Ptr pI830 = I830PTR(pScrn);
    I830OutputPtr output = &pI830->output[pI830->num_outputs];
    struct i830_sdvo_priv *dev_priv;
    int i;
    unsigned char ch[0x40];
    I2CBusPtr i2cbus = NULL, ddcbus;

    output->type = I830_OUTPUT_SDVO;
    output->dpms = i830_sdvo_dpms;
    output->save = i830_sdvo_save;
    output->restore = i830_sdvo_restore;
    output->mode_valid = i830_sdvo_mode_valid;
    output->pre_set_mode = i830_sdvo_pre_set_mode;
    output->post_set_mode = i830_sdvo_post_set_mode;
    output->detect = i830_sdvo_detect;
    output->get_modes = i830_ddc_get_modes;

    /* While it's the same bus, we just initialize a new copy to avoid trouble
     * with tracking refcounting ourselves, since the XFree86 DDX bits don't.
     */
    if (output_device == SDVOB)
	I830I2CInit(pScrn, &i2cbus, GPIOE, "SDVOCTRL_E for SDVOB");
    else
	I830I2CInit(pScrn, &i2cbus, GPIOE, "SDVOCTRL_E for SDVOC");

    if (i2cbus == NULL)
	return;

    /* Allocate the SDVO output private data */
    dev_priv = xcalloc(1, sizeof(struct i830_sdvo_priv));
    if (dev_priv == NULL) {
	xf86DestroyI2CBusRec(i2cbus, TRUE, TRUE);
	return;
    }

    if (output_device == SDVOB) {
	dev_priv->d.DevName = "SDVO Controller B";
	dev_priv->d.SlaveAddr = 0x70;
    } else {
	dev_priv->d.DevName = "SDVO Controller C";
	dev_priv->d.SlaveAddr = 0x72;
    }
    dev_priv->d.pI2CBus = i2cbus;
    dev_priv->d.DriverPrivate.ptr = output;
    dev_priv->output_device = output_device;

    if (!xf86I2CDevInit(&dev_priv->d)) {
	xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		   "Failed to initialize SDVO I2C device %s\n",
		   output_device == SDVOB ? "SDVOB" : "SDVOC");
	xf86DestroyI2CBusRec(i2cbus, TRUE, TRUE);
	xfree(dev_priv);
	return;
    }

    /* Set up our wrapper I2C bus for DDC.  It acts just like the regular I2C
     * bus, except that it does the control bus switch to DDC mode before every
     * Start.  While we only need to do it at Start after every Stop after a
     * Start, extra attempts should be harmless.
     */
    ddcbus = xf86CreateI2CBusRec();
    if (ddcbus == NULL) {
	xf86DestroyI2CDevRec(&dev_priv->d, FALSE);
	xf86DestroyI2CBusRec(i2cbus, TRUE, TRUE);
	xfree(dev_priv);
	return;
    }
    if (output_device == SDVOB)
        ddcbus->BusName = "SDVOB DDC Bus";
    else
        ddcbus->BusName = "SDVOC DDC Bus";
    ddcbus->scrnIndex = i2cbus->scrnIndex;
    ddcbus->I2CGetByte = i830_sdvo_ddc_i2c_get_byte;
    ddcbus->I2CPutByte = i830_sdvo_ddc_i2c_put_byte;
    ddcbus->I2CStart = i830_sdvo_ddc_i2c_start;
    ddcbus->I2CStop = i830_sdvo_ddc_i2c_stop;
    ddcbus->I2CAddress = i830_sdvo_ddc_i2c_address;
    ddcbus->DriverPrivate.ptr = &pI830->output[pI830->num_outputs];
    if (!xf86I2CBusInit(ddcbus)) {
	xf86DestroyI2CDevRec(&dev_priv->d, FALSE);
	xf86DestroyI2CBusRec(i2cbus, TRUE, TRUE);
	xfree(dev_priv);
	return;
    }

    output->pI2CBus = i2cbus;
    output->pDDCBus = ddcbus;
    output->dev_priv = dev_priv;

    /* Read the regs to test if we can talk to the device */
    for (i = 0; i < 0x40; i++) {
	if (!i830_sdvo_read_byte(output, i, &ch[i])) {
	    xf86DestroyI2CBusRec(output->pDDCBus, FALSE, FALSE);
	    xf86DestroyI2CDevRec(&dev_priv->d, FALSE);
	    xf86DestroyI2CBusRec(i2cbus, TRUE, TRUE);
	    xfree(dev_priv);
	    return;
	}
    }

    i830_sdvo_get_capabilities(output, &dev_priv->caps);

    i830_sdvo_get_input_pixel_clock_range(output, &dev_priv->pixel_clock_min,
					  &dev_priv->pixel_clock_max);

    memset(&dev_priv->active_outputs, 0, sizeof(dev_priv->active_outputs));
    dev_priv->active_outputs.tmds0 = 1;

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	       "SDVO device VID/DID: %02X:%02X.%02X, "
	       "clock range %.1fMHz - %.1fMHz, "
	       "input 1: %c, input 2: %c, "
	       "output 1: %c, output 2: %c\n",
	       dev_priv->caps.vendor_id, dev_priv->caps.device_id,
	       dev_priv->caps.device_rev_id,
	       dev_priv->pixel_clock_min / 1000.0,
	       dev_priv->pixel_clock_max / 1000.0,
	       (dev_priv->caps.sdvo_inputs_mask & 0x1) ? 'Y' : 'N',
	       (dev_priv->caps.sdvo_inputs_mask & 0x2) ? 'Y' : 'N',
	       dev_priv->caps.output_flags.tmds0 ? 'Y' : 'N',
	       dev_priv->caps.output_flags.tmds1 ? 'Y' : 'N');

    pI830->num_outputs++;
}
