// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2012
 * Altera Corporation <www.altera.com>
 */

#include <clk.h>
#include <log.h>
#include <asm-generic/io.h>
#include <asm/io.h>
#include <dma.h>
#include <dm.h>
#include <fdtdec.h>
#include <malloc.h>
#include <reset.h>
#include <spi.h>
#include <spi-mem.h>
#include <dm/device_compat.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/sizes.h>
#include <linux/time.h>
#include <zynqmp_firmware.h>
#include "cadence_qspi.h"
#include <dt-bindings/power/xlnx-versal-power.h>

#define CQSPI_STIG_READ			0
#define CQSPI_STIG_WRITE		1
#define CQSPI_READ			2
#define CQSPI_WRITE			3

static bool is_calibrated(struct cadence_spi_priv *priv,
			  struct spi_slave *slave)
{
	return (priv->qspi_calibrated_hz == priv->req_hz) &&
	       (priv->qspi_calibrated_cs == spi_chip_select(slave->dev));
}

static void set_calibrated(struct cadence_spi_priv *priv,
			   struct spi_slave *slave)
{
	priv->qspi_calibrated_hz = priv->req_hz;
	priv->qspi_calibrated_cs = spi_chip_select(slave->dev);
}

__weak int cadence_qspi_apb_dma_read(struct cadence_spi_priv *priv,
				     const struct spi_mem_op *op)
{
	return 0;
}

__weak int cadence_qspi_versal_flash_reset(struct udevice *dev)
{
	return 0;
}

__weak ofnode cadence_qspi_get_subnode(struct udevice *dev)
{
	return dev_read_first_subnode(dev);
}

static int cadence_spi_write_speed(struct udevice *bus, uint hz)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *plat = dev_get_plat(bus);

	cadence_qspi_apb_config_baudrate_div(priv->regbase,
					     priv->ref_clk_hz, hz);

	/* Reconfigure delay timing if speed is changed. */
	cadence_qspi_apb_delay(priv->regbase, priv->ref_clk_hz, hz,
			       plat->tshsl_ns, plat->tsd2d_ns,
			       plat->tchsh_ns, plat->tslch_ns);
}

void cadence_spi_update_speed(struct udevice *bus, bool calibrated)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	void *regb = priv->regbase;
	uint hz = priv->req_hz;
	u32 rdc = 0;

	if (!calibrated)
		hz = plat->calib_hz;

	/* Disable QSPI */
	cadence_qspi_apb_controller_disable(regb);

	if (calibrated) {
		rdc = priv->read_delay;
		if (plat->phy_support && plat->use_phy) {
			cadence_qspi_apb_enable_phy(regb, true);
			if (plat->slow_phy_tx)
				hz /= 4;
		}
	} else if (plat->phy_support) {
		cadence_qspi_apb_enable_phy(regb, false);
	}

	cadence_spi_write_speed(bus, hz);
	cadence_qspi_apb_readdata_capture(priv, 1, rdc);

	if (plat->phy_support)
		cadence_qspi_apb_set_phy_cfg(regb,
					     priv->phyrxdly, priv->phytxdly);

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(regb);
}

/* Calibration sequence to determine the read data capture delay register
 * Returns 0 on success, negative error code.
 */
static int spi_non_phy_calibrate(struct spi_slave *slave,
				 int (*test_read_fn)(struct spi_slave *))
{
	struct udevice *bus = slave->dev->parent;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	void *base = priv->regbase;
	int err = 0, i, range_lo = -1, range_hi = -1;

	/* use back the intended clock and find low range */
	cadence_spi_update_speed(bus, true);

	for (i = 0; i < plat->max_read_delay; i++) {
		/* Disable QSPI */
		cadence_qspi_apb_controller_disable(base);

		/* reconfigure the read data capture delay register */
		cadence_qspi_apb_readdata_capture(priv, 1, i);

		/* Enable back QSPI */
		cadence_qspi_apb_controller_enable(base);

		err = test_read_fn(slave);
		if (err < 0) {
			puts("SF: Calibration failed (read)\n");
			goto err;
		}

		/* search for range lo */
		if (range_lo == -1 && err == 0) {
			range_lo = i;
			continue;
		}

		/* search for range hi */
		if (range_lo != -1 && err) {
			range_hi = i - 1;
			break;
		}
		range_hi = i;
	}

	if (range_lo == -1) {
		puts("SF: Calibration failed (low range)\n");
		err = -EIO;
		goto err;
	}

	/* Disable QSPI for subsequent initialization */
	cadence_qspi_apb_controller_disable(base);

	priv->read_delay = (range_hi + range_lo) / 2;

	/* configure the final value for read data capture delay register */
	cadence_qspi_apb_readdata_capture(priv, 1, priv->read_delay);
	debug("SF: Calibration: read-delay=%u (%i - %i)\n",
	      priv->read_delay, range_lo, range_hi);

	return 0;

err:
	cadence_spi_update_speed(bus, false);
	return err;
}

#if CONFIG_IS_ENABLED(SPI_FLASH_HS_CALIB)
static void clr_calibrated(struct cadence_spi_priv *priv,
			   struct spi_slave *slave)
{
	priv->qspi_calibrated_hz = 0;
}

/* Returns 0 on success, negative error code.
 */
int cadence_spi_calibrate(struct spi_slave *slave,
			  int (*test_read_fn)(struct spi_slave *))
{
	struct udevice *bus = slave->dev->parent;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	int err = 0;

	if (!test_read_fn) {
		if (priv->qspi_calibrated_hz &&
		    priv->qspi_calibrated_cs != spi_chip_select(slave->dev)) {
			debug("%s: multiple chips on the bus not yet implemented\n",
			      __func__);
			return -ENOSYS;
		}
		clr_calibrated(priv, slave);
		cadence_spi_update_speed(bus, false);
		return 0;
	}

	/* todo: Allow recalibrations. This could be useful in the event
	 * of a communication CRC failure. A recalibration could improve
	 * signal quality. This however, requires implementing communication
	 * CRC features in spi-nor.
	 */
	if (is_calibrated(priv, slave))
		return 0;

	if (plat->calib_cfg) {
		set_calibrated(priv, slave);
		cadence_spi_update_speed(bus, true);
		return 0;
	}

	if (plat->use_phy)
		err = cadence_qspi_apb_phy_calibrate(slave, test_read_fn);
	else
		err = spi_non_phy_calibrate(slave, test_read_fn);
	if (err)
		return err;

	set_calibrated(priv, slave);

	return 0;
}
#else
/* NOTE: This will not work as expected if spi-nor has put the chip into a
 * multi-io or DDR mode. Use CONFIG_SPI_FLASH_HS_CALIB.
 * Calibrating from only 3 bytes is also not enough to get a reliable
 * calibration range.
 */
static int cadence_spi_read_id(struct cadence_spi_priv *priv, u8 len,
			       u8 *idcode)
{
	int err;

	struct spi_mem_op op = SPI_MEM_OP(SPI_MEM_OP_CMD(0x9F, 1),
					  SPI_MEM_OP_NO_ADDR,
					  SPI_MEM_OP_NO_DUMMY,
					  SPI_MEM_OP_DATA_IN(len, idcode, 1));

	err = cadence_qspi_apb_command_read_setup(priv, &op);
	if (!err)
		err = cadence_qspi_apb_command_read(priv, &op);
	return err;
}

/* Returns 0 on success, negative error code.
 */
static int cadence_spi_legacy_non_phy_calib_chk(struct spi_slave *slave)
{
	struct udevice *bus = slave->dev->parent;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	u32 temp = 0;

	int err = cadence_spi_read_id(priv, 3, (u8 *)&temp);

	if (err)
		return err;

	return temp != priv->chipid;
}

/* Calibration sequence to determine the read data capture delay register
 * Returns 0 on success, negative error code.
 */
static int legacy_spi_calibration(struct spi_slave *slave)
{
	struct udevice *bus = slave->dev->parent;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	int err = 0;

	if (is_calibrated(priv, slave)) {
		return 0;
	} else if (plat->calib_cfg) {
		set_calibrated(priv, slave);
		cadence_spi_update_speed(bus, true);
		return 0;
	}

	cadence_spi_update_speed(bus, false);

	/* read the ID which will be our golden value */
	err = cadence_spi_read_id(priv, 3, (u8 *)&priv->chipid);
	if (err) {
		puts("SF: Calibration failed (read)\n");
		return err;
	}

	err = spi_non_phy_calibrate(slave, cadence_spi_legacy_non_phy_calib_chk);
	if (err)
		return err;

	set_calibrated(priv, slave);

	return 0;
}
#endif

static int cadence_spi_set_speed(struct udevice *bus, uint hz)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	/*
	 * In the high speed calib case, clibration clearing will then apply
	 * the max non-calibrated speed.
	 * When calibration occurs later, it will then apply the full requested
	 * speed.
	 *
	 * In the legacy calibration case, exec_op calls check if the new
	 * speed needs to be applied.
	 */
	priv->req_hz = hz;

	debug("%s: speed=%d\n", __func__, hz);

	return 0;
}

#if CONFIG_IS_ENABLED(DMA_CHANNELS)
static int cadence_spi_probe_dma(struct udevice *bus)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct dma_dev_priv *dma_uc;
	int hasdma;
	int ret;

	hasdma = (ofnode_read_u32(dev_ofnode(bus), "dmas", NULL) == 0) &&
		 (ofnode_read_u32(dev_ofnode(bus), "dma-names", NULL) == 0);
	if (!hasdma)
		return 0;

	ret = dma_get_by_name(bus, "dst", &priv->dstdma);
	if (ret != 0)
		return 0;

	dma_uc = dev_get_uclass_priv(priv->dstdma.dev);

	if (dma_uc->supported == DMA_SUPPORTS_MEM_TO_MEM) {
		/* We were given a specific DMA channel that only
		 * supports mem-to-mem transactions.
		 */
		priv->hasdma = hasdma;
		priv->ops.direct_read_copy = cadence_qspi_apb_read_copy_mdma;
		priv->ops.direct_write_copy = cadence_qspi_apb_write_copy_mdma;
		return 0;
	}

	/* Todo: Implement device DMA channel modes when needed
	 * (DMA_SUPPORTS_MEM_TO_DEV, DMA_SUPPORTS_DEV_TO_MEM).
	 */
	return -ENOSYS;
}
#endif

static int cadence_spi_probe(struct udevice *bus)
{
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct clk clk;
	int ret;

	priv->plat		= plat;
	priv->regbase		= plat->regbase;
	priv->ahbbase		= plat->ahbbase;
	priv->is_dma		= plat->is_dma;
	priv->is_decoded_cs	= plat->is_decoded_cs;
	priv->fifo_depth	= plat->fifo_depth;
	priv->fifo_width	= plat->fifo_width;
	priv->trigger_address	= plat->trigger_address;
	priv->ahbsize		= plat->ahbsize;
	priv->read_delay	= plat->read_delay;
	priv->phyrxdly		= plat->phyrxdly;
	priv->phytxdly		= plat->phytxdly;

	priv->ops.direct_read_copy = cadence_qspi_apb_direct_read_copy;
	priv->ops.direct_write_copy = cadence_qspi_apb_direct_write_copy;

	if (IS_ENABLED(CONFIG_ZYNQMP_FIRMWARE))
		xilinx_pm_request(PM_REQUEST_NODE, PM_DEV_OSPI,
				  ZYNQMP_PM_CAPABILITY_ACCESS, ZYNQMP_PM_MAX_QOS,
				  ZYNQMP_PM_REQUEST_ACK_NO, NULL);

	if (priv->ref_clk_hz == 0) {
		ret = clk_get_by_index(bus, 0, &clk);
		if (ret) {
#ifdef CONFIG_HAS_CQSPI_REF_CLK
			priv->ref_clk_hz = CONFIG_CQSPI_REF_CLK;
#elif defined(CONFIG_ARCH_SOCFPGA)
			priv->ref_clk_hz = cm_get_qspi_controller_clk_hz();
#else
			return ret;
#endif
		} else {
			priv->ref_clk_hz = clk_get_rate(&clk);
			if (IS_ERR_VALUE(priv->ref_clk_hz))
				return priv->ref_clk_hz;
		}
	}

	priv->resets = devm_reset_bulk_get_optional(bus);
	if (priv->resets)
		reset_deassert_bulk(priv->resets);

	if (!priv->qspi_is_init) {
		cadence_qspi_apb_controller_init(priv);
		priv->qspi_is_init = 1;
	}

	priv->wr_delay = 50 * DIV_ROUND_UP(NSEC_PER_SEC, priv->ref_clk_hz);

	if (CONFIG_IS_ENABLED(DMA_CHANNELS)) {
		ret = cadence_spi_probe_dma(bus);
		if (ret)
			return ret;
	}

	/* Versal and Versal-NET use spi calibration to set read delay */
	if (CONFIG_IS_ENABLED(ARCH_VERSAL) ||
	    CONFIG_IS_ENABLED(ARCH_VERSAL_NET))
		if (priv->read_delay >= 0)
			priv->read_delay = -1;

	/* Reset ospi flash device */
	return cadence_qspi_versal_flash_reset(bus);
}

static int cadence_spi_remove(struct udevice *dev)
{
	struct cadence_spi_priv *priv = dev_get_priv(dev);
	int ret = 0;

	if (priv->resets)
		ret = reset_release_bulk(priv->resets);

	return ret;
}

static int cadence_spi_set_mode(struct udevice *bus, uint mode)
{
	struct cadence_spi_priv *priv = dev_get_priv(bus);

	/* Disable QSPI */
	cadence_qspi_apb_controller_disable(priv->regbase);

	/* Set SPI mode */
	cadence_qspi_apb_set_clk_mode(priv->regbase, mode);

	/* Enable Direct Access Controller */
	if (priv->use_dac_mode)
		cadence_qspi_apb_dac_mode_enable(priv->regbase);

	/* Enable QSPI */
	cadence_qspi_apb_controller_enable(priv->regbase);

	return 0;
}

static int cadence_spi_mem_exec_op(struct spi_slave *spi,
				   const struct spi_mem_op *op)
{
	struct udevice *bus = spi->dev->parent;
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	void *base = priv->regbase;
	int err = 0;
	u32 mode;

	/* Set Chip select */
	cadence_qspi_apb_chipselect(base, spi_chip_select(spi->dev),
				    priv->is_decoded_cs);

	/* todo: Due to there only being 1 declaration of per-flash parameters,
	 * this driver only ever correctly supported 1 chip on the bus.
	 * Per-flash data must be moved into a data structure that can lookup
	 * by chip select.
	 */
#if CONFIG_IS_ENABLED(SPI_FLASH_HS_CALIB)
	if (priv->qspi_calibrated_hz &&
	    priv->qspi_calibrated_cs != spi_chip_select(spi->dev)) {
		debug("%s: multiple chips on the bus not yet implemented\n",
		      __func__);
		return -ENOSYS;
	} else if (is_calibrated(priv, spi) &&
		   priv->qspi_calibrated_hz != priv->req_hz) {
		debug("%s: speed change after calibration not yet supported\n",
		      __func__);
		return -ENOSYS;
	}
#else
	/* Regardless of the above, attempt a recalib anyway. */
	if (!is_calibrated(priv, spi) ||
	    priv->qspi_calibrated_hz != priv->req_hz) {
		err = legacy_spi_calibration(spi);
		if (err)
			return err;
	}
#endif

	if (op->data.dir == SPI_MEM_DATA_IN && op->data.buf.in) {
		/*
		 * Performing reads in DAC mode forces to read minimum 4 bytes
		 * which is unsupported on some flash devices during register
		 * reads, prefer STIG mode for such small reads.
		 */
		if (op->data.nbytes <= CQSPI_STIG_DATA_LEN_MAX)
			mode = CQSPI_STIG_READ;
		else
			mode = CQSPI_READ;
	} else {
		if (op->data.nbytes <= CQSPI_STIG_DATA_LEN_MAX)
			mode = CQSPI_STIG_WRITE;
		else
			mode = CQSPI_WRITE;
	}

	switch (mode) {
	case CQSPI_STIG_READ:
		err = cadence_qspi_apb_command_read_setup(priv, op);
		if (!err)
			err = cadence_qspi_apb_command_read(priv, op);
		break;
	case CQSPI_STIG_WRITE:
		err = cadence_qspi_apb_command_write_setup(priv, op);
		if (!err)
			err = cadence_qspi_apb_command_write(priv, op);
		break;
	case CQSPI_READ:
		err = cadence_qspi_apb_read_setup(priv, op);
		if (!err) {
			if (priv->is_dma)
				err = cadence_qspi_apb_dma_read(priv, op);
			else
				err = cadence_qspi_apb_read_execute(priv, op);
		}
		break;
	case CQSPI_WRITE:
		err = cadence_qspi_apb_write_setup(priv, op);
		if (!err)
			err = cadence_qspi_apb_write_execute(priv, op);
		break;
	default:
		err = -1;
		break;
	}

	return err;
}

static bool cadence_spi_mem_supports_op(struct spi_slave *slave,
					const struct spi_mem_op *op)
{
	bool all_true, all_false;

	/*
	 * For an op to be DTR, cmd phase along with every other non-empty
	 * phase should have dtr field set to 1. If an op phase has zero
	 * nbytes, ignore its dtr field; otherwise, check its dtr field.
	 * Also, dummy checks not performed here Since supports_op()
	 * already checks that all or none of the fields are DTR.
	 *
	 * op->dummy.dtr is required for converting nbytes into ncycles.
	 * Also, don't check the dtr field of the op phase having zero nbytes.
	 */
	all_true = op->cmd.dtr &&
		   (!op->addr.nbytes || op->addr.dtr) &&
		   (!op->dummy.nbytes || op->dummy.dtr) &&
		   (!op->data.nbytes || op->data.dtr);

	all_false = !op->cmd.dtr && !op->addr.dtr && !op->dummy.dtr &&
		    !op->data.dtr;

	/* Mixed DTR modes not supported. */
	if (!(all_true || all_false))
		return false;

	if (all_true)
		return spi_mem_dtr_supports_op(slave, op);
	else
		return spi_mem_default_supports_op(slave, op);
}

static int cadence_spi_of_to_plat(struct udevice *bus)
{
	struct cadence_spi_plat *plat = dev_get_plat(bus);
	struct cadence_spi_priv *priv = dev_get_priv(bus);
	struct cadence_spi_plat *chip = plat;
	ofnode subnode;

	plat->regbase = devfdt_get_addr_index_ptr(bus, 0);
	plat->ahbbase = devfdt_get_addr_size_index_ptr(bus, 1, &plat->ahbsize);
	plat->is_decoded_cs = dev_read_bool(bus, "cdns,is-decoded-cs");
	plat->fifo_depth = dev_read_u32_default(bus, "cdns,fifo-depth", 128);
	plat->fifo_width = dev_read_u32_default(bus, "cdns,fifo-width", 4);
	plat->trigger_address = dev_read_u32_default(bus,
						     "cdns,trigger-address",
						     0);
	/* Use DAC mode only when MMIO window is at least 8M wide */
	if (plat->ahbsize >= SZ_8M)
		priv->use_dac_mode = true;

	plat->is_dma = dev_read_bool(bus, "cdns,is-dma");

	if (CONFIG_IS_ENABLED(SPI_FLASH_HS_CALIB))
		plat->phy_support = bus->driver_data & CQSPI_HW_SUPPORTS_PHY;

	plat->slow_phy_tx = plat->phy_support && (bus->driver_data
		& CQSPI_QUIRK_SLOW_PHY_TX_DMA);

	plat->max_read_delay = dev_read_u32_default(bus,
						    "cdns,max-read-delay",
						    CQSPI_READ_CAPTURE_MAX_DELAY);

	/* All other parameters are embedded in the child node */
	subnode = cadence_qspi_get_subnode(bus);
	if (!ofnode_valid(subnode)) {
		debug("Error: subnode with SPI flash config missing!\n");
		return -ENODEV;
	}

	/* Read other parameters from DT */
	chip->page_size = ofnode_read_u32_default(subnode, "page-size", 256);
	chip->block_size = ofnode_read_u32_default(subnode, "block-size", 16);
	chip->tshsl_ns = ofnode_read_u32_default(subnode, "cdns,tshsl-ns",
						 200);
	chip->tsd2d_ns = ofnode_read_u32_default(subnode, "cdns,tsd2d-ns",
						 255);
	chip->tchsh_ns = ofnode_read_u32_default(subnode, "cdns,tchsh-ns", 20);
	chip->tslch_ns = ofnode_read_u32_default(subnode, "cdns,tslch-ns", 20);

	chip->calib_hz = ofnode_read_u32_default(subnode,
						 "cdns,spi-calib-frequency",
						 1000000);

	chip->use_dqs = ofnode_read_bool(subnode, "cdns,dqs");
	chip->use_phy = ofnode_read_bool(subnode, "cdns,phy") && plat->phy_support;

	if (!ofnode_read_u32(subnode, "cdns,read-delay", &chip->read_delay))
		chip->calib_cfg |= true;
	if (chip->read_delay > plat->max_read_delay)
		chip->read_delay = plat->max_read_delay;

	if (!ofnode_read_u32(subnode, "cdns,phyrxdly", &chip->phyrxdly) ||
	    !ofnode_read_u32(subnode, "cdns,phytxdly", &chip->phytxdly)) {
		chip->calib_cfg |= true;
		if (!chip->use_phy)
			debug("PHY delays configured but PHY mode is not enabled!\n");
	}

	debug("%s: regbase=%p ahbbase=%p page-size=%d\n",
	      __func__, plat->regbase, plat->ahbbase, plat->page_size);

	return 0;
}

static const struct spi_controller_mem_ops cadence_spi_mem_ops = {
	.exec_op = cadence_spi_mem_exec_op,
	.supports_op = cadence_spi_mem_supports_op,
#if CONFIG_IS_ENABLED(SPI_FLASH_HS_CALIB)
	.calibrate = cadence_spi_calibrate,
#endif
};

static const struct dm_spi_ops cadence_spi_ops = {
	.set_speed	= cadence_spi_set_speed,
	.set_mode	= cadence_spi_set_mode,
	.mem_ops	= &cadence_spi_mem_ops,
	/*
	 * cs_info is not needed, since we require all chip selects to be
	 * in the device tree explicitly
	 */
};

static const struct udevice_id cadence_spi_ids[] = {
	{ .compatible = "cdns,qspi-nor" },
	{ .compatible = "ti,am654-ospi" },
	{ .compatible = "adi,sc59x-ospi", .data =
		CQSPI_HW_SUPPORTS_PHY | CQSPI_QUIRK_SLOW_PHY_TX_DMA },
	{ }
};

U_BOOT_DRIVER(cadence_spi) = {
	.name = "cadence_spi",
	.id = UCLASS_SPI,
	.of_match = cadence_spi_ids,
	.ops = &cadence_spi_ops,
	.of_to_plat = cadence_spi_of_to_plat,
	.plat_auto	= sizeof(struct cadence_spi_plat),
	.priv_auto	= sizeof(struct cadence_spi_priv),
	.probe = cadence_spi_probe,
	.remove = cadence_spi_remove,
	.flags = DM_FLAG_OS_PREPARE,
};
