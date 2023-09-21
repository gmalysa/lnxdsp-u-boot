/*
 * Copyright (C) 2012 Altera Corporation <www.altera.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  - Neither the name of the Altera Corporation nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL ALTERA CORPORATION BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <log.h>
#include <asm/io.h>
#include <dma.h>
#include <dma-uclass.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <wait_bit.h>
#include <spi.h>
#include <spi-mem.h>
#include <malloc.h>
#include "cadence_qspi.h"

__weak void cadence_qspi_apb_enable_linear_mode(bool enable)
{
	return;
}

void cadence_qspi_apb_controller_enable(void *reg_base)
{
	unsigned int reg;
	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_ENABLE;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

void cadence_qspi_apb_controller_disable(void *reg_base)
{
	unsigned int reg;
	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~CQSPI_REG_CONFIG_ENABLE;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

void cadence_qspi_apb_dac_mode_enable(void *reg_base)
{
	unsigned int reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_DIRECT;
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

void cadence_qspi_apb_enable_phy(void *reg_base, bool enbl)
{
	u32 reg;

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	if (enbl)
		reg |= CQSPI_REG_CONFIG_PHY_ENABLE_MASK
			| CQSPI_REG_CONFIG_PIPELINE_PHY_EN_MASK;
	else
		reg &= ~(CQSPI_REG_CONFIG_PHY_ENABLE_MASK
			| CQSPI_REG_CONFIG_PIPELINE_PHY_EN_MASK);
	writel(reg, reg_base + CQSPI_REG_CONFIG);
}

void cadence_qspi_apb_set_phy_cfg(void *reg_base,
				  u32 rxdly, u32 txdly)
{
	u32 reg;

	reg = readl(reg_base + CQSPI_REG_PHY_CONFIG);
	reg &= ~(CQSPI_REG_PHY_CONFIG_RESYNC
		| (CQSPI_REG_PHY_CONFIG_RXDLY_MSK
			<< CQSPI_REG_PHY_CONFIG_RXDLY_LSB)
		| (CQSPI_REG_PHY_CONFIG_TXDLY_MSK
			<< CQSPI_REG_PHY_CONFIG_TXDLY_LSB));
	reg |= ((rxdly & CQSPI_REG_PHY_CONFIG_RXDLY_MSK)
			<< CQSPI_REG_PHY_CONFIG_RXDLY_LSB)
		| ((txdly & CQSPI_REG_PHY_CONFIG_TXDLY_MSK)
			<< CQSPI_REG_PHY_CONFIG_TXDLY_LSB)
		| CQSPI_REG_PHY_CONFIG_RXBYP;
	writel(reg, reg_base + CQSPI_REG_PHY_CONFIG);

	reg = readl(reg_base + CQSPI_REG_PHY_CONFIG);
	reg |= CQSPI_REG_PHY_CONFIG_RESYNC;
	writel(reg, reg_base + CQSPI_REG_PHY_CONFIG);
}

void cadence_qspi_apb_readdata_capture(const struct cadence_spi_priv *priv,
				       unsigned int bypass, unsigned int delay)
{
	void *reg_base = priv->regbase;
	unsigned int reg;

	cadence_qspi_apb_controller_disable(reg_base);
	reg = readl(reg_base + CQSPI_REG_RD_DATA_CAPTURE);

	if (bypass)
		reg |= CQSPI_REG_RD_DATA_CAPTURE_BYPASS;
	else
		reg &= ~CQSPI_REG_RD_DATA_CAPTURE_BYPASS;

	reg &= ~(CQSPI_REG_RD_DATA_CAPTURE_DELAY_MASK
		<< CQSPI_REG_RD_DATA_CAPTURE_DELAY_LSB);

	reg |= (delay & CQSPI_REG_RD_DATA_CAPTURE_DELAY_MASK)
		<< CQSPI_REG_RD_DATA_CAPTURE_DELAY_LSB;

	if (priv->plat->phy_support) {
		if (priv->plat->use_dqs)
			reg |= CQSPI_REG_READCAPTURE_DQS_ENABLE;
		else
			reg &= ~CQSPI_REG_READCAPTURE_DQS_ENABLE;
	}

	writel(reg, reg_base + CQSPI_REG_RD_DATA_CAPTURE);

	cadence_qspi_apb_controller_enable(reg_base);
}

#if CONFIG_IS_ENABLED(SPI_FLASH_HS_CALIB)
/**
 * This algorithm was implemented based on the Analog Devices application note
 * EE-437: "OSPI PHY Configuration and Training".
 *
 * Algorithm breif:
 * * Set Read Delay Capture, TX DLL delay, and RX DLL delay to 0.
 * * Iterate over RDC, TXdly and RXdly until the first valid configuration is
 *   found.
 * * Keep this RDC value. Scan all TXdly values for the range of valid values
 *   and pick the middle value.
 * * If data strobe signal (DQS) is used, use the first valid RXdly value, +1.
 * * If DQS is not used, scan all RXdly for the range of valid values and pick
 *   the middle value.
 * * Configurations are considered valid if they pass for at least 2
 *   consecutive iterations.
 *
 * The caller is responsible for providing the function to test
 * if a configuration is valid.
 *
 * Returns 0 on success, negative error code.
 */
int cadence_qspi_apb_phy_calibrate(struct spi_slave *slave,
				   int (*test_read_fn)(struct spi_slave *))
{
	struct udevice *bus = slave->dev->parent;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	void * const reg_base = priv->regbase;
	int err = 0;
	const bool dqs = plat->use_dqs;
	int fast = 1;

	int rdcd;
	int txvalid_count;
	int rxvalid_count;

	int first_txdly_valid;
	int last_txdly_valid;
	int first_rxdly_valid;
	int last_rxdly_valid;

	int txdly;
	int rxdly;

	int txdly_step;
	int rxdly_step;
	int txpass_limit;
	int rxpass_limit;

	if (priv->req_hz != priv->ref_clk_hz) {
		debug("%s: phy mode must operate at ref_clk speed.", __func__);
		err = -EINVAL;
		goto out;
	}

	cadence_spi_update_speed(bus, true);

try_again_slow:
	if (fast) {
		txdly_step = 4;
		rxdly_step = 4;
		txpass_limit = 16;
		rxpass_limit = 16;
	} else {
		txdly_step = 1;
		rxdly_step = 1;
		txpass_limit = 128;
		rxpass_limit = 128;
	}

	first_txdly_valid = -1;
	last_txdly_valid = -1;

	for (rdcd = 0; rdcd < plat->max_read_delay; ++rdcd) {
		cadence_qspi_apb_readdata_capture(priv, 1, rdcd);

		txvalid_count = 0;
		for (txdly = 0; txdly < CQSPI_PHY_DLL_MAX_DELAY;
				txdly += txdly_step) {
			rxvalid_count = 0;
			for (rxdly = 0; rxdly < CQSPI_PHY_DLL_MAX_DELAY;
					rxdly += rxdly_step) {
				cadence_qspi_apb_set_phy_cfg(reg_base,
							     rxdly, txdly);
				err = test_read_fn(slave);
				if (err < 0) {
					goto out;
				} else if (!err) {
					++rxvalid_count;

					if (rxvalid_count == 2)
						++txvalid_count;

					/* Check for enough passing cfgs
					 * With DQS, only 2 need to pass.
					 */
					if (dqs && rxvalid_count >= 2)
						break;
					else if (!dqs && (rxvalid_count >=
							  rxpass_limit))
						break;
				} else if (rxvalid_count == 1) {
					//must be consecutive to be valid
					rxvalid_count = 0;
				} else if (rxvalid_count >= 2) {
					break; //end of valid range
				}
			}

			if (rxvalid_count >= 2) {
				if (first_txdly_valid < 0)
					first_txdly_valid = txdly;
				last_txdly_valid = txdly;
			}

			if (txvalid_count >= txpass_limit)
				break;
			else if (txvalid_count && !rxvalid_count)
				break;
		}
		if (first_txdly_valid >= 0)
			break;
	}

	if (first_txdly_valid < 0 || last_txdly_valid < 0) {
		if (fast) {
			fast = 0;
			goto try_again_slow;
		} else {
			goto out;
		}
	}

	txdly = (first_txdly_valid + last_txdly_valid) / 2;

	rxvalid_count = 0;
	first_rxdly_valid = -1;
	last_rxdly_valid = -1;
	for (rxdly = 0; rxdly < CQSPI_PHY_DLL_MAX_DELAY;
			rxdly += rxdly_step) {
		cadence_qspi_apb_set_phy_cfg(reg_base, rxdly, txdly);
		err = test_read_fn(slave);
		if (err < 0) {
			goto out;
		} else if (!err) {
			++rxvalid_count;

			if (first_rxdly_valid < 0)
				first_rxdly_valid = rxdly;
			last_rxdly_valid = rxdly;

			//check if we have enough passing configurations
			if (dqs && rxvalid_count >= 2)
				break;
			else if (!dqs && (rxvalid_count >= rxpass_limit))
				break;
		} else if (rxvalid_count == 1) {
			//must be consecutive to be valid
			rxvalid_count = 0;
			first_rxdly_valid = -1;
			last_rxdly_valid = -1;
		} else if (rxvalid_count >= 2) {
			break; //end of valid range
		}
	}

	if (first_rxdly_valid < 0 || last_rxdly_valid < 0) {
		if (fast) {
			fast = 0;
			goto try_again_slow;
		} else {
			goto out;
		}
	}

	if (dqs)
		rxdly = first_rxdly_valid + 1;
	else
		rxdly = (first_rxdly_valid + last_rxdly_valid) / 2;

	cadence_qspi_apb_set_phy_cfg(reg_base, rxdly, txdly);
	err = test_read_fn(slave);
	if (err < 0) {
		goto out;
	} else if (err) {
		if (fast) {
			fast = 0;
			goto try_again_slow;
		} else {
			goto out;
		}
	}

	priv->phyrxdly = rxdly;
	priv->phytxdly = txdly;
	priv->read_delay = rdcd;

	debug("%s: read-delay=%u phyrxdly=%u phytxdly=%u\n",
	      __func__, rdcd, rxdly, txdly);

	return 0;

out:
	cadence_spi_update_speed(bus, false);
	return err;
}
#endif

static unsigned int cadence_qspi_calc_dummy(const struct spi_mem_op *op,
					    bool dtr)
{
	unsigned int dummy_clk;

	if (!op->dummy.nbytes || !op->dummy.buswidth)
		return 0;

	dummy_clk = op->dummy.nbytes * (8 / op->dummy.buswidth);
	if (dtr)
		dummy_clk /= 2;

	return dummy_clk;
}

static u32 cadence_qspi_calc_rdreg(struct cadence_spi_priv *priv)
{
	u32 rdreg = 0;

	rdreg |= priv->inst_width << CQSPI_REG_RD_INSTR_TYPE_INSTR_LSB;
	rdreg |= priv->addr_width << CQSPI_REG_RD_INSTR_TYPE_ADDR_LSB;
	rdreg |= priv->data_width << CQSPI_REG_RD_INSTR_TYPE_DATA_LSB;

	return rdreg;
}

static int cadence_qspi_buswidth_to_inst_type(u8 buswidth)
{
	switch (buswidth) {
	case 0:
	case 1:
		return CQSPI_INST_TYPE_SINGLE;

	case 2:
		return CQSPI_INST_TYPE_DUAL;

	case 4:
		return CQSPI_INST_TYPE_QUAD;

	case 8:
		return CQSPI_INST_TYPE_OCTAL;

	default:
		return -ENOTSUPP;
	}
}

static int cadence_qspi_set_protocol(struct cadence_spi_priv *priv,
				     const struct spi_mem_op *op)
{
	int ret;

	ret = cadence_qspi_buswidth_to_inst_type(op->cmd.buswidth);
	if (ret < 0)
		return ret;
	priv->inst_width = ret;

	ret = cadence_qspi_buswidth_to_inst_type(op->addr.buswidth);
	if (ret < 0)
		return ret;
	priv->addr_width = ret;

	ret = cadence_qspi_buswidth_to_inst_type(op->data.buswidth);
	if (ret < 0)
		return ret;
	priv->data_width = ret;

	return 0;
}

/* Return 1 if idle, otherwise return 0 (busy). */
static unsigned int cadence_qspi_wait_idle(void *reg_base)
{
	unsigned int start, count = 0;
	/* timeout in unit of ms */
	unsigned int timeout = 5000;

	start = get_timer(0);
	for ( ; get_timer(start) < timeout ; ) {
		if (CQSPI_REG_IS_IDLE(reg_base))
			count++;
		else
			count = 0;
		/*
		 * Ensure the QSPI controller is in true idle state after
		 * reading back the same idle status consecutively
		 */
		if (count >= CQSPI_POLL_IDLE_RETRY)
			return 1;
	}

	/* Timeout, still in busy mode. */
	printf("QSPI: QSPI is still busy after poll for %d ms.\n", timeout);
	return 0;
}

void cadence_qspi_apb_config_baudrate_div(void *reg_base,
	unsigned int ref_clk_hz, unsigned int sclk_hz)
{
	unsigned int reg;
	unsigned int div;

	cadence_qspi_apb_controller_disable(reg_base);
	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_BAUD_MASK << CQSPI_REG_CONFIG_BAUD_LSB);

	/*
	 * The baud_div field in the config reg is 4 bits, and the ref clock is
	 * divided by 2 * (baud_div + 1). Round up the divider to ensure the
	 * SPI clock rate is less than or equal to the requested clock rate.
	 */
	div = DIV_ROUND_UP(ref_clk_hz, sclk_hz * 2) - 1;

	/* ensure the baud rate doesn't exceed the max value */
	if (div > CQSPI_REG_CONFIG_BAUD_MASK)
		div = CQSPI_REG_CONFIG_BAUD_MASK;

	debug("%s: ref_clk %dHz sclk %dHz Div 0x%x, %s %dHz\n", __func__,
	      ref_clk_hz, sclk_hz, div,
	      (readl(reg_base + CQSPI_REG_CONFIG) &
	       CQSPI_REG_CONFIG_PHY_ENABLE_MASK) ? "PHY" : "actual",
	      (readl(reg_base + CQSPI_REG_CONFIG) &
	       CQSPI_REG_CONFIG_PHY_ENABLE_MASK) ?
	       ref_clk_hz : ref_clk_hz / (2 * (div + 1)));

	reg |= (div << CQSPI_REG_CONFIG_BAUD_LSB);
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	cadence_qspi_apb_controller_enable(reg_base);
}

void cadence_qspi_apb_set_clk_mode(void *reg_base, uint mode)
{
	unsigned int reg;

	cadence_qspi_apb_controller_disable(reg_base);
	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg &= ~(CQSPI_REG_CONFIG_CLK_POL | CQSPI_REG_CONFIG_CLK_PHA);

	if (mode & SPI_CPOL)
		reg |= CQSPI_REG_CONFIG_CLK_POL;
	if (mode & SPI_CPHA)
		reg |= CQSPI_REG_CONFIG_CLK_PHA;

	writel(reg, reg_base + CQSPI_REG_CONFIG);

	cadence_qspi_apb_controller_enable(reg_base);
}

void cadence_qspi_apb_chipselect(void *reg_base,
	unsigned int chip_select, unsigned int decoder_enable)
{
	unsigned int reg;

	cadence_qspi_apb_controller_disable(reg_base);

	debug("%s : chipselect %d decode %d\n", __func__, chip_select,
	      decoder_enable);

	reg = readl(reg_base + CQSPI_REG_CONFIG);
	/* docoder */
	if (decoder_enable) {
		reg |= CQSPI_REG_CONFIG_DECODE;
	} else {
		reg &= ~CQSPI_REG_CONFIG_DECODE;
		/* Convert CS if without decoder.
		 * CS0 to 4b'1110
		 * CS1 to 4b'1101
		 * CS2 to 4b'1011
		 * CS3 to 4b'0111
		 */
		chip_select = 0xF & ~(1 << chip_select);
	}

	reg &= ~(CQSPI_REG_CONFIG_CHIPSELECT_MASK
			<< CQSPI_REG_CONFIG_CHIPSELECT_LSB);
	reg |= (chip_select & CQSPI_REG_CONFIG_CHIPSELECT_MASK)
			<< CQSPI_REG_CONFIG_CHIPSELECT_LSB;
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	cadence_qspi_apb_controller_enable(reg_base);
}

void cadence_qspi_apb_delay(void *reg_base,
	unsigned int ref_clk, unsigned int sclk_hz,
	unsigned int tshsl_ns, unsigned int tsd2d_ns,
	unsigned int tchsh_ns, unsigned int tslch_ns)
{
	unsigned int ref_clk_ns;
	unsigned int sclk_ns;
	unsigned int tshsl, tchsh, tslch, tsd2d;
	unsigned int reg;

	cadence_qspi_apb_controller_disable(reg_base);

	/* Convert to ns. */
	ref_clk_ns = DIV_ROUND_UP(1000000000, ref_clk);

	/* Convert to ns. */
	sclk_ns = DIV_ROUND_UP(1000000000, sclk_hz);

	/* The controller adds additional delay to that programmed in the reg */
	if (tshsl_ns >= sclk_ns + ref_clk_ns)
		tshsl_ns -= sclk_ns + ref_clk_ns;
	if (tchsh_ns >= sclk_ns + 3 * ref_clk_ns)
		tchsh_ns -= sclk_ns + 3 * ref_clk_ns;
	tshsl = DIV_ROUND_UP(tshsl_ns, ref_clk_ns);
	tchsh = DIV_ROUND_UP(tchsh_ns, ref_clk_ns);
	tslch = DIV_ROUND_UP(tslch_ns, ref_clk_ns);
	tsd2d = DIV_ROUND_UP(tsd2d_ns, ref_clk_ns);

	reg = ((tshsl & CQSPI_REG_DELAY_TSHSL_MASK)
			<< CQSPI_REG_DELAY_TSHSL_LSB);
	reg |= ((tchsh & CQSPI_REG_DELAY_TCHSH_MASK)
			<< CQSPI_REG_DELAY_TCHSH_LSB);
	reg |= ((tslch & CQSPI_REG_DELAY_TSLCH_MASK)
			<< CQSPI_REG_DELAY_TSLCH_LSB);
	reg |= ((tsd2d & CQSPI_REG_DELAY_TSD2D_MASK)
			<< CQSPI_REG_DELAY_TSD2D_LSB);
	writel(reg, reg_base + CQSPI_REG_DELAY);

	cadence_qspi_apb_controller_enable(reg_base);
}

void cadence_qspi_apb_controller_init(struct cadence_spi_priv *priv)
{
	unsigned reg;

	cadence_qspi_apb_controller_disable(priv->regbase);

	/* Configure the device size and address bytes */
	reg = readl(priv->regbase + CQSPI_REG_SIZE);
	/* Clear the previous value */
	reg &= ~(CQSPI_REG_SIZE_PAGE_MASK << CQSPI_REG_SIZE_PAGE_LSB);
	reg &= ~(CQSPI_REG_SIZE_BLOCK_MASK << CQSPI_REG_SIZE_BLOCK_LSB);
	reg |= (priv->plat->page_size << CQSPI_REG_SIZE_PAGE_LSB);
	reg |= (priv->plat->block_size << CQSPI_REG_SIZE_BLOCK_LSB);
	writel(reg, priv->regbase + CQSPI_REG_SIZE);

	/* Configure the remap address register, no remap */
	writel(0, priv->regbase + CQSPI_REG_REMAP);

	/* Indirect mode configurations */
	writel(priv->fifo_depth / 2, priv->regbase + CQSPI_REG_SRAMPARTITION);

	/* Disable all interrupts */
	writel(0, priv->regbase + CQSPI_REG_IRQMASK);

	cadence_qspi_apb_controller_enable(priv->regbase);
}

int cadence_qspi_apb_exec_flash_cmd(void *reg_base, unsigned int reg)
{
	unsigned int retry = CQSPI_REG_RETRY;

	/* Write the CMDCTRL without start execution. */
	writel(reg, reg_base + CQSPI_REG_CMDCTRL);
	/* Start execute */
	reg |= CQSPI_REG_CMDCTRL_EXECUTE;
	writel(reg, reg_base + CQSPI_REG_CMDCTRL);

	while (retry--) {
		reg = readl(reg_base + CQSPI_REG_CMDCTRL);
		if ((reg & CQSPI_REG_CMDCTRL_INPROGRESS) == 0)
			break;
		udelay(1);
	}

	if (!retry) {
		printf("QSPI: flash command execution timeout\n");
		return -EIO;
	}

	/* Polling QSPI idle status. */
	if (!cadence_qspi_wait_idle(reg_base))
		return -EIO;

	/* Flush the CMDCTRL reg after the execution */
	writel(0, reg_base + CQSPI_REG_CMDCTRL);

	return 0;
}

static int cadence_qspi_setup_opcode_ext(struct cadence_spi_priv *priv,
					 const struct spi_mem_op *op,
					 unsigned int shift)
{
	unsigned int reg;
	u8 ext;

	if (op->cmd.nbytes != 2)
		return -EINVAL;

	/* Opcode extension is the LSB. */
	ext = op->cmd.opcode & 0xff;

	reg = readl(priv->regbase + CQSPI_REG_OP_EXT_LOWER);
	reg &= ~(0xff << shift);
	reg |= ext << shift;
	writel(reg, priv->regbase + CQSPI_REG_OP_EXT_LOWER);

	return 0;
}

static int cadence_qspi_enable_dtr(struct cadence_spi_priv *priv,
				   const struct spi_mem_op *op,
				   unsigned int shift,
				   bool enable)
{
	unsigned int reg;
	int ret;

	reg = readl(priv->regbase + CQSPI_REG_CONFIG);

	switch (op->cmd.nbytes) {
	case 1:
		reg &= ~CQSPI_REG_CONFIG_DUAL_OPCODE;
		break;
	case 2:
		reg |= CQSPI_REG_CONFIG_DUAL_OPCODE;

		/* Set up command opcode extension. */
		ret = cadence_qspi_setup_opcode_ext(priv, op, shift);
		if (ret)
			return ret;
		break;
	default:
		return log_msg_ret("QSPI: Invalid command length", -EINVAL);
	}

	if (enable)
		reg |= CQSPI_REG_CONFIG_DTR_PROTO;
	else
		reg &= ~CQSPI_REG_CONFIG_DTR_PROTO;

	writel(reg, priv->regbase + CQSPI_REG_CONFIG);

	return 0;
}

int cadence_qspi_apb_command_read_setup(struct cadence_spi_priv *priv,
					const struct spi_mem_op *op)
{
	int ret;
	unsigned int reg;

	ret = cadence_qspi_set_protocol(priv, op);
	if (ret)
		return ret;

	ret = cadence_qspi_enable_dtr(priv, op, CQSPI_REG_OP_EXT_STIG_LSB,
				      op->cmd.dtr);
	if (ret)
		return ret;

	reg = cadence_qspi_calc_rdreg(priv);
	reg |= op->cmd.dtr ? CQSPI_REG_RD_INSTR_DDR_EN_MASK : 0;
	writel(reg, priv->regbase + CQSPI_REG_RD_INSTR);

	return 0;
}

/* For command RDID, RDSR. */
int cadence_qspi_apb_command_read(struct cadence_spi_priv *priv,
				  const struct spi_mem_op *op)
{
	void *reg_base = priv->regbase;
	unsigned int reg;
	unsigned int read_len;
	int status;
	unsigned int rxlen = op->data.nbytes;
	void *rxbuf = op->data.buf.in;
	unsigned int dummy_clk;
	u8 opcode;

	switch (op->cmd.nbytes) {
	case 1:
		opcode = op->cmd.opcode;
		break;
	case 2:
		opcode = op->cmd.opcode >> 8;
		break;
	default:
		return log_msg_ret("QSPI: Invalid command length", -EINVAL);
	}

	if (opcode == CMD_4BYTE_OCTAL_READ && !op->cmd.dtr)
		opcode = CMD_4BYTE_FAST_READ;

	reg = opcode << CQSPI_REG_CMDCTRL_OPCODE_LSB;

	/* Set up dummy cycles. */
	dummy_clk = cadence_qspi_calc_dummy(op, op->cmd.dtr);
	if (dummy_clk > CQSPI_DUMMY_CLKS_MAX)
		return -ENOTSUPP;

	if (dummy_clk)
		reg |= (dummy_clk & CQSPI_REG_CMDCTRL_DUMMY_MASK)
		     << CQSPI_REG_CMDCTRL_DUMMY_LSB;

	reg |= (0x1 << CQSPI_REG_CMDCTRL_RD_EN_LSB);

	/* 0 means 1 byte. */
	reg |= (((rxlen - 1) & CQSPI_REG_CMDCTRL_RD_BYTES_MASK)
		<< CQSPI_REG_CMDCTRL_RD_BYTES_LSB);

	/* setup ADDR BIT field */
	if (op->addr.nbytes) {
		writel(op->addr.val, priv->regbase + CQSPI_REG_CMDADDRESS);
		/*
		 * address bytes are zero indexed
		 */
		reg |= (((op->addr.nbytes - 1) &
			  CQSPI_REG_CMDCTRL_ADD_BYTES_MASK) <<
			  CQSPI_REG_CMDCTRL_ADD_BYTES_LSB);
		reg |= (0x1 << CQSPI_REG_CMDCTRL_ADDR_EN_LSB);
	}

	status = cadence_qspi_apb_exec_flash_cmd(reg_base, reg);
	if (status != 0)
		return status;

	reg = readl(reg_base + CQSPI_REG_CMDREADDATALOWER);

	/* Put the read value into rx_buf */
	read_len = (rxlen > 4) ? 4 : rxlen;
	memcpy(rxbuf, &reg, read_len);
	rxbuf += read_len;

	if (rxlen > 4) {
		reg = readl(reg_base + CQSPI_REG_CMDREADDATAUPPER);

		read_len = rxlen - read_len;
		memcpy(rxbuf, &reg, read_len);
	}
	return 0;
}

int cadence_qspi_apb_command_write_setup(struct cadence_spi_priv *priv,
					 const struct spi_mem_op *op)
{
	int ret;
	unsigned int reg;

	ret = cadence_qspi_set_protocol(priv, op);
	if (ret)
		return ret;

	ret = cadence_qspi_enable_dtr(priv, op, CQSPI_REG_OP_EXT_STIG_LSB,
				      op->cmd.dtr);
	if (ret)
		return ret;

	reg = cadence_qspi_calc_rdreg(priv);
	reg |= op->cmd.dtr ? CQSPI_REG_RD_INSTR_DDR_EN_MASK : 0;
	writel(reg, priv->regbase + CQSPI_REG_RD_INSTR);

	return 0;
}

/* For commands: WRSR, WREN, WRDI, CHIP_ERASE, BE, etc. */
int cadence_qspi_apb_command_write(struct cadence_spi_priv *priv,
				   const struct spi_mem_op *op)
{
	unsigned int reg = 0;
	unsigned int wr_data;
	unsigned int wr_len;
	unsigned int dummy_clk;
	unsigned int txlen = op->data.nbytes;
	const void *txbuf = op->data.buf.out;
	void *reg_base = priv->regbase;
	u8 opcode;

	switch (op->cmd.nbytes) {
	case 1:
		opcode = op->cmd.opcode;
		break;
	case 2:
		opcode = op->cmd.opcode >> 8;
		break;
	default:
		return log_msg_ret("QSPI: Invalid command length", -EINVAL);
	}

	reg |= opcode << CQSPI_REG_CMDCTRL_OPCODE_LSB;

	/* setup ADDR BIT field */
	if (op->addr.nbytes) {
		writel(op->addr.val, priv->regbase + CQSPI_REG_CMDADDRESS);
		/*
		 * address bytes are zero indexed
		 */
		reg |= (((op->addr.nbytes - 1) &
			  CQSPI_REG_CMDCTRL_ADD_BYTES_MASK) <<
			  CQSPI_REG_CMDCTRL_ADD_BYTES_LSB);
		reg |= (0x1 << CQSPI_REG_CMDCTRL_ADDR_EN_LSB);
	}

	/* Set up dummy cycles. */
	dummy_clk = cadence_qspi_calc_dummy(op, op->cmd.dtr);
	if (dummy_clk > CQSPI_DUMMY_CLKS_MAX)
		return -EOPNOTSUPP;

	if (dummy_clk)
		reg |= (dummy_clk & CQSPI_REG_CMDCTRL_DUMMY_MASK)
		     << CQSPI_REG_CMDCTRL_DUMMY_LSB;

	if (txlen) {
		/* writing data = yes */
		reg |= (0x1 << CQSPI_REG_CMDCTRL_WR_EN_LSB);
		reg |= ((txlen - 1) & CQSPI_REG_CMDCTRL_WR_BYTES_MASK)
			<< CQSPI_REG_CMDCTRL_WR_BYTES_LSB;

		wr_len = txlen > 4 ? 4 : txlen;
		memcpy(&wr_data, txbuf, wr_len);
		writel(wr_data, reg_base +
			CQSPI_REG_CMDWRITEDATALOWER);

		if (txlen > 4) {
			txbuf += wr_len;
			wr_len = txlen - wr_len;
			memcpy(&wr_data, txbuf, wr_len);
			writel(wr_data, reg_base +
				CQSPI_REG_CMDWRITEDATAUPPER);
		}
	}

	/* Execute the command */
	return cadence_qspi_apb_exec_flash_cmd(reg_base, reg);
}

/* Opcode + Address (3/4 bytes) + dummy bytes (0-4 bytes) */
int cadence_qspi_apb_read_setup(struct cadence_spi_priv *priv,
				const struct spi_mem_op *op)
{
	unsigned int reg;
	unsigned int rd_reg;
	unsigned int dummy_clk;
	unsigned int dummy_bytes = op->dummy.nbytes;
	int ret;
	u8 opcode;

	ret = cadence_qspi_set_protocol(priv, op);
	if (ret)
		return ret;

	ret = cadence_qspi_enable_dtr(priv, op, CQSPI_REG_OP_EXT_READ_LSB,
				      op->cmd.dtr);
	if (ret)
		return ret;

	/* Setup the indirect trigger address */
	writel(priv->trigger_address,
	       priv->regbase + CQSPI_REG_INDIRECTTRIGGER);

	/* Configure the opcode */
	switch (op->cmd.nbytes) {
	case 1:
		opcode = op->cmd.opcode;
		break;
	case 2:
		opcode = op->cmd.opcode >> 8;
		break;
	default:
		return log_msg_ret("QSPI: Invalid command length", -EINVAL);
	}

	rd_reg = opcode << CQSPI_REG_RD_INSTR_OPCODE_LSB;
	rd_reg |= op->cmd.dtr ? CQSPI_REG_RD_INSTR_DDR_EN_MASK : 0;
	rd_reg |= cadence_qspi_calc_rdreg(priv);

	writel(op->addr.val, priv->regbase + CQSPI_REG_INDIRECTRDSTARTADDR);

	if (dummy_bytes) {
		/* Convert to clock cycles. */
		dummy_clk = cadence_qspi_calc_dummy(op, op->cmd.dtr);

		if (dummy_clk > CQSPI_DUMMY_CLKS_MAX)
			return -ENOTSUPP;

		if (dummy_clk)
			rd_reg |= (dummy_clk & CQSPI_REG_RD_INSTR_DUMMY_MASK)
				<< CQSPI_REG_RD_INSTR_DUMMY_LSB;
	}

	writel(rd_reg, priv->regbase + CQSPI_REG_RD_INSTR);

	/* set device size */
	reg = readl(priv->regbase + CQSPI_REG_SIZE);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (op->addr.nbytes - 1);
	writel(reg, priv->regbase + CQSPI_REG_SIZE);
	return 0;
}

static u32 cadence_qspi_get_rd_sram_level(struct cadence_spi_priv *priv)
{
	u32 reg = readl(priv->regbase + CQSPI_REG_SDRAMLEVEL);
	reg >>= CQSPI_REG_SDRAMLEVEL_RD_LSB;
	return reg & CQSPI_REG_SDRAMLEVEL_RD_MASK;
}

static int cadence_qspi_wait_for_data(struct cadence_spi_priv *priv)
{
	unsigned int timeout = 10000;
	u32 reg;

	while (timeout--) {
		reg = cadence_qspi_get_rd_sram_level(priv);
		if (reg)
			return reg;
		udelay(1);
	}

	return -ETIMEDOUT;
}

static int
cadence_qspi_apb_indirect_read_execute(struct cadence_spi_priv *priv,
				       unsigned int n_rx, u8 *rxbuf)
{
	unsigned int remaining = n_rx;
	unsigned int bytes_to_read = 0;
	int ret;

	writel(n_rx, priv->regbase + CQSPI_REG_INDIRECTRDBYTES);

	/* Start the indirect read transfer */
	writel(CQSPI_REG_INDIRECTRD_START,
	       priv->regbase + CQSPI_REG_INDIRECTRD);

	while (remaining > 0) {
		ret = cadence_qspi_wait_for_data(priv);
		if (ret < 0) {
			printf("Indirect write timed out (%i)\n", ret);
			goto failrd;
		}

		bytes_to_read = ret;

		while (bytes_to_read != 0) {
			bytes_to_read *= priv->fifo_width;
			bytes_to_read = bytes_to_read > remaining ?
					remaining : bytes_to_read;
			/*
			 * Handle non-4-byte aligned access to avoid
			 * data abort.
			 */
			if (((uintptr_t)rxbuf % 4) || (bytes_to_read % 4))
				readsb(priv->ahbbase, rxbuf, bytes_to_read);
			else
				readsl(priv->ahbbase, rxbuf,
				       bytes_to_read >> 2);
			rxbuf += bytes_to_read;
			remaining -= bytes_to_read;
			bytes_to_read = cadence_qspi_get_rd_sram_level(priv);
		}
	}

	/* Check indirect done status */
	ret = wait_for_bit_le32(priv->regbase + CQSPI_REG_INDIRECTRD,
				CQSPI_REG_INDIRECTRD_DONE, 1, 10, 0);
	if (ret) {
		printf("Indirect read completion error (%i)\n", ret);
		goto failrd;
	}

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTRD_DONE,
	       priv->regbase + CQSPI_REG_INDIRECTRD);

	/* Check indirect done status */
	ret = wait_for_bit_le32(priv->regbase + CQSPI_REG_INDIRECTRD,
				CQSPI_REG_INDIRECTRD_DONE, 0, 10, 0);
	if (ret) {
		printf("Indirect read clear completion error (%i)\n", ret);
		goto failrd;
	}

	return 0;

failrd:
	/* Cancel the indirect read */
	writel(CQSPI_REG_INDIRECTRD_CANCEL,
	       priv->regbase + CQSPI_REG_INDIRECTRD);
	return ret;
}

int cadence_qspi_apb_read_execute(struct cadence_spi_priv *priv,
				  const struct spi_mem_op *op)
{
	u64 from = op->addr.val;
	void *buf = op->data.buf.in;
	size_t len = op->data.nbytes;
	int retval = 0;

	cadence_qspi_apb_enable_linear_mode(true);

	if (op->addr.nbytes && priv->use_dac_mode && (from + len < priv->ahbsize)) {
		retval = priv->ops.direct_read_copy(priv, buf, from, len);
		if (!cadence_qspi_wait_idle(priv->regbase))
			retval = -EIO;
		return retval;
	}

	return cadence_qspi_apb_indirect_read_execute(priv, len, buf);
}

/* Opcode + Address (3/4 bytes) */
int cadence_qspi_apb_write_setup(struct cadence_spi_priv *priv,
				 const struct spi_mem_op *op)
{
	unsigned int reg;
	int ret;
	u8 opcode;

	ret = cadence_qspi_set_protocol(priv, op);
	if (ret)
		return ret;

	ret = cadence_qspi_enable_dtr(priv, op, CQSPI_REG_OP_EXT_WRITE_LSB,
				      op->cmd.dtr);
	if (ret)
		return ret;

	/* Setup the indirect trigger address */
	writel(priv->trigger_address,
	       priv->regbase + CQSPI_REG_INDIRECTTRIGGER);

	/* Configure the opcode */
	switch (op->cmd.nbytes) {
	case 1:
		opcode = op->cmd.opcode;
		break;
	case 2:
		opcode = op->cmd.opcode >> 8;
		break;
	default:
		return log_msg_ret("QSPI: Invalid command length", -EINVAL);
	}

	reg = opcode << CQSPI_REG_WR_INSTR_OPCODE_LSB;
	reg |= CQSPI_REG_WR_INSTR_WELDIS_MASK;
	reg |= priv->data_width << CQSPI_REG_WR_INSTR_TYPE_DATA_LSB;
	reg |= priv->addr_width << CQSPI_REG_WR_INSTR_TYPE_ADDR_LSB;
	writel(reg, priv->regbase + CQSPI_REG_WR_INSTR);

	reg = cadence_qspi_calc_rdreg(priv);
	reg |= op->cmd.dtr ? CQSPI_REG_RD_INSTR_DDR_EN_MASK : 0;
	writel(reg, priv->regbase + CQSPI_REG_RD_INSTR);

	writel(op->addr.val, priv->regbase + CQSPI_REG_INDIRECTWRSTARTADDR);

	/*
	 * Some flashes like the cypress Semper flash expect a 4-byte
	 * dummy address with the Read SR command in DTR mode, but this
	 * controller does not support sending address with the Read SR
	 * command. So, disable write completion polling on the
	 * controller's side. spi-nor will take care of polling the
	 * status register.
	 *
	 * Theoretically, some flashes have their WIP bit in different
	 * bit positions or have a different bit polarity. spi-nor
	 * currently does not have an interface in place to dictate
	 * this information to this driver for proper configuration.
	 *
	 * The default of the controller hardware has this status register
	 * auto polling without expiration. This means that if there is any
	 * controller misconfiguration or communication failure, it will
	 * completely lock up the controller.
	 *
	 * Thus, unconditionally disable this feature for now.
	 */
	reg = readl(priv->regbase + CQSPI_REG_WR_COMPLETION_CTRL);
	reg |= CQSPI_REG_WR_DISABLE_AUTO_POLL;
	writel(reg, priv->regbase + CQSPI_REG_WR_COMPLETION_CTRL);

	reg = readl(priv->regbase + CQSPI_REG_SIZE);
	reg &= ~CQSPI_REG_SIZE_ADDRESS_MASK;
	reg |= (op->addr.nbytes - 1);
	writel(reg, priv->regbase + CQSPI_REG_SIZE);
	return 0;
}

static int
cadence_qspi_apb_indirect_write_execute(struct cadence_spi_priv *priv,
					unsigned int n_tx, const u8 *txbuf)
{
	unsigned int page_size = priv->plat->page_size;
	unsigned int remaining = n_tx;
	const u8 *bb_txbuf = txbuf;
	void *bounce_buf = NULL;
	unsigned int write_bytes;
	int ret;

	/*
	 * Use bounce buffer for non 32 bit aligned txbuf to avoid data
	 * aborts
	 */
	if ((uintptr_t)txbuf % 4) {
		bounce_buf = malloc(n_tx);
		if (!bounce_buf)
			return -ENOMEM;
		memcpy(bounce_buf, txbuf, n_tx);
		bb_txbuf = bounce_buf;
	}

	/* Configure the indirect read transfer bytes */
	writel(n_tx, priv->regbase + CQSPI_REG_INDIRECTWRBYTES);

	/* Start the indirect write transfer */
	writel(CQSPI_REG_INDIRECTWR_START,
	       priv->regbase + CQSPI_REG_INDIRECTWR);

	/*
	 * Some delay is required for the above bit to be internally
	 * synchronized by the QSPI module.
	 */
	ndelay(priv->wr_delay);

	while (remaining > 0) {
		write_bytes = remaining > page_size ? page_size : remaining;
		writesl(priv->ahbbase, bb_txbuf, write_bytes >> 2);
		if (write_bytes % 4)
			writesb(priv->ahbbase,
				bb_txbuf + rounddown(write_bytes, 4),
				write_bytes % 4);

		ret = wait_for_bit_le32(priv->regbase + CQSPI_REG_SDRAMLEVEL,
					CQSPI_REG_SDRAMLEVEL_WR_MASK <<
					CQSPI_REG_SDRAMLEVEL_WR_LSB, 0, 10, 0);
		if (ret) {
			printf("Indirect write timed out (%i)\n", ret);
			goto failwr;
		}

		bb_txbuf += write_bytes;
		remaining -= write_bytes;
	}

	/* Check indirect done status */
	ret = wait_for_bit_le32(priv->regbase + CQSPI_REG_INDIRECTWR,
				CQSPI_REG_INDIRECTWR_DONE, 1, 10, 0);
	if (ret) {
		printf("Indirect write completion error (%i)\n", ret);
		goto failwr;
	}

	/* Clear indirect completion status */
	writel(CQSPI_REG_INDIRECTWR_DONE,
	       priv->regbase + CQSPI_REG_INDIRECTWR);

	/* Check indirect done status */
	ret = wait_for_bit_le32(priv->regbase + CQSPI_REG_INDIRECTWR,
				CQSPI_REG_INDIRECTWR_DONE, 0, 10, 0);
	if (ret) {
		printf("Indirect write clear completion error (%i)\n", ret);
		goto failwr;
	}

	if (bounce_buf)
		free(bounce_buf);
	return 0;

failwr:
	/* Cancel the indirect write */
	writel(CQSPI_REG_INDIRECTWR_CANCEL,
	       priv->regbase + CQSPI_REG_INDIRECTWR);
	if (bounce_buf)
		free(bounce_buf);
	return ret;
}

int cadence_qspi_apb_write_execute(struct cadence_spi_priv *priv,
				   const struct spi_mem_op *op)
{
	u32 to = op->addr.val;
	const void *buf = op->data.buf.out;
	size_t len = op->data.nbytes;
	u32 cfg;
	int retval = 0;

	cadence_qspi_apb_enable_linear_mode(true);
	if (op->addr.nbytes && priv->use_dac_mode && (to + len < priv->ahbsize)) {
		cfg = readl(priv->regbase + CQSPI_REG_CONFIG);
		if (priv->plat->slow_phy_tx && (cfg & CQSPI_REG_CONFIG_PHY_ENABLE_MASK))
			writel(cfg & ~(CQSPI_REG_CONFIG_PHY_ENABLE_MASK),
			       priv->regbase + CQSPI_REG_CONFIG);

		retval = priv->ops.direct_write_copy(priv, buf, to, len);

		if (!cadence_qspi_wait_idle(priv->regbase))
			retval = -EIO;

		writel(cfg, priv->regbase + CQSPI_REG_CONFIG);

		return retval;
	}

	return cadence_qspi_apb_indirect_write_execute(priv, len, buf);
}

void cadence_qspi_apb_enter_xip(void *reg_base, char xip_dummy)
{
	unsigned int reg;

	/* enter XiP mode immediately and enable direct mode */
	reg = readl(reg_base + CQSPI_REG_CONFIG);
	reg |= CQSPI_REG_CONFIG_ENABLE;
	reg |= CQSPI_REG_CONFIG_DIRECT;
	reg |= CQSPI_REG_CONFIG_XIP_IMM;
	writel(reg, reg_base + CQSPI_REG_CONFIG);

	/* keep the XiP mode */
	writel(xip_dummy, reg_base + CQSPI_REG_MODE_BIT);

	/* Enable mode bit at devrd */
	reg = readl(reg_base + CQSPI_REG_RD_INSTR);
	reg |= (1 << CQSPI_REG_RD_INSTR_MODE_EN_LSB);
	writel(reg, reg_base + CQSPI_REG_RD_INSTR);
}

#if CONFIG_IS_ENABLED(DMA_CHANNELS)
static int cadence_qspi_apb_copy_mdma(struct udevice *dmadev,
				      void *dst, void *src, size_t len)
{
	struct dma_ops *ops = (struct dma_ops *)dmadev->driver->ops;

	/* Some transfers might not be aligned to cache boundaries. Align them
	 * for the cache operation while preserving the original transfer
	 * address.
	 */
	uintptr_t algn_dst_l = ((uintptr_t)dst / ARCH_DMA_MINALIGN) *
				ARCH_DMA_MINALIGN;
	uintptr_t algn_dst_h = ALIGN((uintptr_t)dst + len, ARCH_DMA_MINALIGN);
	uintptr_t algn_src_l = ((uintptr_t)src / ARCH_DMA_MINALIGN) *
				ARCH_DMA_MINALIGN;
	uintptr_t algn_src_h = ALIGN((uintptr_t)src + len, ARCH_DMA_MINALIGN);
	uintptr_t algn_len = max(algn_dst_h - algn_dst_l,
				 algn_src_h - algn_src_l);

	dma_addr_t dst_map = dma_map_single((void *)algn_dst_l, algn_len,
					    DMA_FROM_DEVICE);
	dma_addr_t src_map = dma_map_single((void *)algn_src_l, algn_len,
					    DMA_TO_DEVICE);

	uintptr_t dma_dst = dst_map + ((uintptr_t)dst - algn_dst_l);
	uintptr_t dma_src = src_map + ((uintptr_t)src - algn_src_l);

	int ret = ops->transfer(dmadev, DMA_MEM_TO_MEM, dma_dst, dma_src, len);

	dma_unmap_single(dst_map,  algn_len, DMA_FROM_DEVICE);
	dma_unmap_single(src_map, algn_len, DMA_TO_DEVICE);

	return ret;
}

int cadence_qspi_apb_read_copy_mdma(struct cadence_spi_priv *priv,
				    void *dst, u64 src, size_t len)
{
	return cadence_qspi_apb_copy_mdma(priv->dstdma.dev, dst,
					  priv->ahbbase + src, len);
}

int cadence_qspi_apb_write_copy_mdma(struct cadence_spi_priv *priv,
				     const void *src, u64 dst, size_t len)
{
	return cadence_qspi_apb_copy_mdma(priv->dstdma.dev,
					  priv->ahbbase + dst,
					  (void *)src, len);
}
#else
int cadence_qspi_apb_read_copy_mdma(struct cadence_spi_priv *priv,
				    void *dst, u64 src, size_t len)
{
	return -ENOSYS;
}

int cadence_qspi_apb_write_copy_mdma(struct cadence_spi_priv *priv,
				     const void *src, u64 dst, size_t len)
{
	return -ENOSYS;
}
#endif

int cadence_qspi_apb_direct_read_copy(struct cadence_spi_priv *priv,
				      void *dst, u64 src, size_t len)
{
	if (len < 256 ||
	    dma_memcpy(dst, priv->ahbbase + src, len) < 0) {
		memcpy_fromio(dst, priv->ahbbase + src, len);
	}
	return 0;
}

int cadence_qspi_apb_direct_write_copy(struct cadence_spi_priv *priv,
				       const void *src, u64 dst, size_t len)
{
	memcpy_toio(priv->ahbbase + dst, src, len);
	return 0;
}
