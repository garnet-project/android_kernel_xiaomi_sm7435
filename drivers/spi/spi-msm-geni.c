// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/ipc_logging.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/msm-geni-se.h>
#include <linux/msm_gpi.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-msm-geni.h>
#include <linux/pinctrl/consumer.h>

#define SPI_NUM_CHIPSELECT	(4)
#define SPI_XFER_TIMEOUT_MS (1500)
#define SPI_AUTO_SUSPEND_DELAY	(250)
#define SPI_XFER_TIMEOUT_OFFSET	(250)
/* SPI SE specific registers */
#define SE_SPI_CPHA		(0x224)
#define SE_SPI_LOOPBACK		(0x22C)
#define SE_SPI_CPOL		(0x230)
#define SE_SPI_DEMUX_OUTPUT_INV	(0x24C)
#define SE_SPI_DEMUX_SEL	(0x250)
#define SE_SPI_TRANS_CFG	(0x25C)
#define SE_SPI_WORD_LEN		(0x268)
#define SE_SPI_TX_TRANS_LEN	(0x26C)
#define SE_SPI_RX_TRANS_LEN	(0x270)
#define SE_SPI_PRE_POST_CMD_DLY	(0x274)
#define SE_SPI_DELAY_COUNTERS	(0x278)

#define SE_SPI_SLAVE_EN           (0x2BC)
#define SPI_SLAVE_EN              BIT(0)

/* SE_SPI_CPHA register fields */
#define CPHA			(BIT(0))

/* SE_SPI_LOOPBACK register fields */
#define LOOPBACK_ENABLE		(0x1)
#define NORMAL_MODE		(0x0)
#define LOOPBACK_MSK		(GENMASK(1, 0))

/* SE_SPI_CPOL register fields */
#define CPOL			(BIT(2))

/* SE_SPI_DEMUX_OUTPUT_INV register fields */
#define CS_DEMUX_OUTPUT_INV_MSK	(GENMASK(3, 0))

/* SE_SPI_DEMUX_SEL register fields */
#define CS_DEMUX_OUTPUT_SEL	(GENMASK(3, 0))

/* SE_SPI_TX_TRANS_CFG register fields */
#define CS_TOGGLE		(BIT(1))

/* SE_SPI_WORD_LEN register fields */
#define WORD_LEN_MSK		(GENMASK(9, 0))
#define MIN_WORD_LEN		(4)

/* SPI_TX/SPI_RX_TRANS_LEN fields */
#define TRANS_LEN_MSK		(GENMASK(23, 0))

/* SE_SPI_DELAY_COUNTERS */
#define SPI_INTER_WORDS_DELAY_MSK	(GENMASK(9, 0))
#define SPI_CS_CLK_DELAY_MSK		(GENMASK(19, 10))
#define SPI_CS_CLK_DELAY_SHFT		(10)

/* M_CMD OP codes for SPI */
#define SPI_TX_ONLY		(1)
#define SPI_RX_ONLY		(2)
#define SPI_FULL_DUPLEX		(3)
#define SPI_TX_RX		(7)
#define SPI_CS_ASSERT		(8)
#define SPI_CS_DEASSERT		(9)
#define SPI_SCK_ONLY		(10)
/* M_CMD params for SPI */
#define SPI_PRE_CMD_DELAY	BIT(0)
#define TIMESTAMP_BEFORE	BIT(1)
#define FRAGMENTATION		BIT(2)
#define TIMESTAMP_AFTER		BIT(3)
#define POST_CMD_DELAY		BIT(4)

/* GSI CONFIG0 TRE Params */
/* Flags bit fields */
#define GSI_LOOPBACK_EN		(BIT(0))
#define GSI_CS_TOGGLE		(BIT(3))
#define GSI_CPHA		(BIT(4))
#define GSI_CPOL		(BIT(5))

#define MAX_TX_SG		(3)
#define NUM_SPI_XFER		(8)

/* SPI sampling registers */
#define SE_GENI_CGC_CTRL	(0x28)
#define SE_GENI_CFG_SEQ_START	(0x84)
#define SE_GENI_CFG_REG108	(0x2B0)
#define SE_GENI_CFG_REG109	(0x2B4)
#define CPOL_CTRL_SHFT	1
#define RX_IO_POS_FF_EN_SEL_SHFT	4
#define RX_IO_EN2CORE_EN_DELAY_SHFT	8
#define RX_SI_EN2IO_DELAY_SHFT 12

#define SPI_LOG_DBG(log_ctx, print, dev, x...) do { \
GENI_SE_DBG(log_ctx, print, dev, x); \
if (dev) \
	spi_trace_log(dev, x); \
} while (0)

#define SPI_LOG_ERR(log_ctx, print, dev, x...) do { \
GENI_SE_ERR(log_ctx, print, dev, x); \
if (dev) \
	spi_trace_log(dev, x); \
} while (0)

#define CREATE_TRACE_POINTS
#include "spi-qup-trace.h"

/* FTRACE Logging */
void spi_trace_log(struct device *dev, const char *fmt, ...)
{
	struct va_format vaf = {
		.fmt = fmt,
	};

	va_list args;

	va_start(args, fmt);
	vaf.va = &args;
	trace_spi_log_info(dev_name(dev), &vaf);
	va_end(args);
}

struct gsi_desc_cb {
	struct spi_master *spi;
	struct spi_transfer *xfer;
};

struct spi_geni_gsi {
	struct msm_gpi_tre lock_t;
	struct msm_gpi_tre unlock_t;
	struct msm_gpi_tre config0_tre;
	struct msm_gpi_tre go_tre;
	struct msm_gpi_tre tx_dma_tre;
	struct msm_gpi_tre rx_dma_tre;
	struct scatterlist tx_sg[MAX_TX_SG];
	struct scatterlist rx_sg;
	dma_cookie_t tx_cookie;
	dma_cookie_t rx_cookie;
	struct msm_gpi_dma_async_tx_cb_param tx_cb_param;
	struct msm_gpi_dma_async_tx_cb_param rx_cb_param;
	struct dma_async_tx_descriptor *tx_desc;
	struct dma_async_tx_descriptor *rx_desc;
	struct gsi_desc_cb desc_cb;
};

struct spi_geni_master {
	struct se_geni_rsc spi_rsc;
	resource_size_t phys_addr;
	resource_size_t size;
	void __iomem *base;
	int irq;
	struct device *dev;
	int rx_fifo_depth;
	int tx_fifo_depth;
	int tx_fifo_width;
	int tx_wm;
	bool setup;
	u32 cur_speed_hz;
	int cur_word_len;
	unsigned int tx_rem_bytes;
	unsigned int rx_rem_bytes;
	struct spi_transfer *cur_xfer;
	struct completion xfer_done;
	struct device *wrapper_dev;
	int oversampling;
	struct spi_geni_gsi *gsi, *gsi_lock_unlock;
	struct dma_chan *tx;
	struct dma_chan *rx;
	struct msm_gpi_ctrl tx_event;
	struct msm_gpi_ctrl rx_event;
	struct completion tx_cb;
	struct completion rx_cb;
	bool qn_err;
	int cur_xfer_mode;
	int num_tx_eot;
	int num_rx_eot;
	int num_xfers;
	void *ipc;
	bool gsi_mode; /* GSI Mode */
	bool shared_ee; /* Dual EE use case */
	bool shared_se; /* True Multi EE use case */
	bool is_le_vm;	/* LE VM usecase */
	bool is_la_vm;	/* LA VM property */
	bool dis_autosuspend;
	bool cmd_done;
	bool set_miso_sampling;
	u32 miso_sampling_ctrl_val;
	bool le_gpi_reset_done;
	bool disable_dma;
	bool slave_setup;
	bool slave_state;
	bool slave_cross_connected;
	u32 xfer_timeout_offset;
	bool master_cross_connect;
	bool is_deep_sleep; /* For deep sleep restore the config similar to the probe. */
	bool is_dma_err;
	bool is_dma_not_done;
};

static void spi_slv_setup(struct spi_geni_master *mas);
static void spi_master_setup(struct spi_geni_master *mas);
static void geni_spi_dma_unprepare(struct spi_master *spi,
					struct spi_transfer *xfer);

static ssize_t spi_slave_state_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	struct platform_device *pdev = container_of(dev, struct
						platform_device, dev);
	struct spi_master *spi = platform_get_drvdata(pdev);
	struct spi_geni_master *geni_mas;

	geni_mas = spi_master_get_devdata(spi);

	if (geni_mas)
		ret = scnprintf(buf, sizeof(int), "%d\n",
				geni_mas->slave_state);
	return ret;
}

static ssize_t spi_slave_state_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct platform_device *pdev = container_of(dev, struct
						platform_device, dev);
	struct spi_master *spi = platform_get_drvdata(pdev);
	struct spi_geni_master *geni_mas;

	geni_mas = spi_master_get_devdata(spi);
	if (geni_mas)
		GENI_SE_DBG(geni_mas->ipc, false, geni_mas->dev,
			"%s: slave_state:%d\n", __func__, geni_mas->slave_state);

	return 1;
}

static DEVICE_ATTR_RW(spi_slave_state);

static void spi_master_setup(struct spi_geni_master *mas)
{
	geni_write_reg(OTHER_IO_OE | IO2_DATA_IN_SEL | RX_DATA_IN_SEL |
		IO_MACRO_IO3_SEL | IO_MACRO_IO2_SEL | IO_MACRO_IO0_SEL,
					mas->base, SE_GENI_CFG_REG80);
	geni_write_reg(START_TRIGGER, mas->base, SE_GENI_CFG_SEQ_START);

	/* ensure data is written to hardware register */
	wmb();
}

static void spi_slv_setup(struct spi_geni_master *mas)
{
	geni_write_reg(SPI_SLAVE_EN, mas->base, SE_SPI_SLAVE_EN);

	if (mas->slave_cross_connected) {
		geni_write_reg(GENI_IO_MUX_1_EN, mas->base, GENI_OUTPUT_CTRL);
		geni_write_reg(IO1_SEL_TX | IO2_DATA_IN_SEL_PAD2 |
			IO3_DATA_IN_SEL_PAD2, mas->base, SE_GENI_CFG_REG80);
	} else {
		geni_write_reg(GENI_IO_MUX_0_EN, mas->base, GENI_OUTPUT_CTRL);
	}
	geni_write_reg(START_TRIGGER, mas->base, SE_GENI_CFG_SEQ_START);
	/* ensure data is written to hardware register */
	wmb();
	dev_info(mas->dev, "spi slave setup done\n");
}

static void geni_spi_dma_err(struct spi_geni_master *mas, u32 dma_status, bool type)
{
	GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s: %s status:0x%x\n",
				__func__, (type ? "DMA-RX" : "DMA-TX"), dma_status);

	/* here checking tx status bits for tx and rx, because bits common
	 * for tx and rx
	 */
	if (dma_status & TX_SBE)
		GENI_SE_DBG(mas->ipc, false, mas->dev,
			"%s: AHB master bus error during DMA transaction\n", __func__);
	if (dma_status & TX_GENI_CANCEL_IRQ)
		GENI_SE_DBG(mas->ipc, false, mas->dev,
			"%s: GENI Cancel Interrupt Status\n", __func__);
	if (dma_status & TX_GENI_CMD_FAILURE)
		GENI_SE_DBG(mas->ipc, false, mas->dev,
			"%s: GENI cmd failure\n", __func__);
}

static int spi_slv_abort(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);

	complete_all(&mas->tx_cb);
	complete_all(&mas->rx_cb);
	return 0;
}

static struct spi_master *get_spi_master(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct spi_master *spi = platform_get_drvdata(pdev);

	return spi;
}

static int get_spi_clk_cfg(u32 speed_hz, struct spi_geni_master *mas,
			int *clk_idx, int *clk_div)
{
	unsigned long sclk_freq;
	unsigned long res_freq;
	struct se_geni_rsc *rsc = &mas->spi_rsc;
	int ret = 0;

	ret = geni_se_clk_freq_match(&mas->spi_rsc,
				(speed_hz * mas->oversampling), clk_idx,
				&sclk_freq, false);
	if (ret) {
		dev_err(mas->dev, "%s: Failed(%d) to find src clk for 0x%x\n",
						__func__, ret, speed_hz);
		return ret;
	}

	*clk_div = DIV_ROUND_UP(sclk_freq,  (mas->oversampling*speed_hz));

	if (!(*clk_div)) {
		dev_err(mas->dev, "%s:Err:sclk:%lu oversampling:%d speed:%u\n",
			__func__, sclk_freq, mas->oversampling, speed_hz);
		return -EINVAL;
	}

	res_freq = (sclk_freq / (*clk_div));

	dev_dbg(mas->dev, "%s: req %u resultant %lu sclk %lu, idx %d, div %d\n",
		__func__, speed_hz, res_freq, sclk_freq, *clk_idx, *clk_div);

	ret = clk_set_rate(rsc->se_clk, sclk_freq);
	if (ret) {
		dev_err(mas->dev, "%s: clk_set_rate failed %d\n",
							__func__, ret);
		return ret;
	}
	return 0;
}

static void spi_setup_word_len(struct spi_geni_master *mas, u32 mode,
						int bits_per_word)
{
	struct spi_master *spi = get_spi_master(mas->dev);
	int pack_words = 1;
	bool msb_first = (mode & SPI_LSB_FIRST) ? false : true;
	u32 word_len = geni_read_reg(mas->base, SE_SPI_WORD_LEN);
	unsigned long cfg0, cfg1;

	/*
	 * If bits_per_word isn't a byte aligned value, set the packing to be
	 * 1 SPI word per FIFO word.
	 */
	if (!(mas->tx_fifo_width % bits_per_word))
		pack_words = mas->tx_fifo_width / bits_per_word;
	se_config_packing(mas->base, bits_per_word, pack_words, msb_first);

	word_len &= ~WORD_LEN_MSK;
	if (spi->slave)
		word_len |= (bits_per_word & WORD_LEN_MSK);
	else
		word_len |= ((bits_per_word - MIN_WORD_LEN) & WORD_LEN_MSK);

	geni_write_reg(word_len, mas->base, SE_SPI_WORD_LEN);
	se_get_packing_config(bits_per_word, pack_words, msb_first,
							&cfg0, &cfg1);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		    "%s: spi_slave: %d cfg0: 0x%x cfg1: 0x%x bpw: %d pack_words: %d word_len: %d\n",
		    __func__, spi->slave, cfg0, cfg1, bits_per_word, pack_words, word_len);
}

static int setup_fifo_params(struct spi_device *spi_slv,
					struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	u16 mode = spi_slv->mode;
	u32 loopback_cfg = geni_read_reg(mas->base, SE_SPI_LOOPBACK);
	u32 cpol = geni_read_reg(mas->base, SE_SPI_CPOL);
	u32 cpha = geni_read_reg(mas->base, SE_SPI_CPHA);
	u32 demux_sel = 0;
	u32 demux_output_inv = 0;
	u32 clk_sel = 0;
	u32 m_clk_cfg = 0;
	int ret = 0;
	int idx;
	int div;
	struct spi_geni_qcom_ctrl_data *delay_params = NULL;
	u32 spi_delay_params = 0;

	loopback_cfg &= ~LOOPBACK_MSK;
	cpol &= ~CPOL;
	cpha &= ~CPHA;

	if (mode & SPI_LOOP)
		loopback_cfg |= LOOPBACK_ENABLE;

	if (mode & SPI_CPOL)
		cpol |= CPOL;

	if (mode & SPI_CPHA)
		cpha |= CPHA;

	/* SPI slave supports only mode 1, log unsuppoted mode and exit */
	if (spi->slave && !(cpol == 0 && cpha == 1)) {
		GENI_SE_DBG(mas->ipc, false, mas->dev,
		"%s: Unsupported SPI Slave mode cpol %d cpha %d\n",
		__func__, cpol, cpha);
		return -EINVAL;
	}

	if (spi_slv->mode & SPI_CS_HIGH)
		demux_output_inv |= BIT(spi_slv->chip_select);

	if (spi_slv->controller_data) {
		u32 cs_clk_delay = 0;
		u32 inter_words_delay = 0;

		delay_params =
		(struct spi_geni_qcom_ctrl_data *) spi_slv->controller_data;
		cs_clk_delay =
		(delay_params->spi_cs_clk_delay << SPI_CS_CLK_DELAY_SHFT)
							& SPI_CS_CLK_DELAY_MSK;
		inter_words_delay =
			delay_params->spi_inter_words_delay &
						SPI_INTER_WORDS_DELAY_MSK;
		spi_delay_params =
		(inter_words_delay | cs_clk_delay);
	}

	demux_sel = spi_slv->chip_select;
	mas->cur_speed_hz = spi_slv->max_speed_hz;
	mas->cur_word_len = spi_slv->bits_per_word;

	ret = get_spi_clk_cfg(mas->cur_speed_hz, mas, &idx, &div);
	if (ret) {
		dev_err(mas->dev, "Err setting clks ret(%d) for %d\n",
							ret, mas->cur_speed_hz);
		goto setup_fifo_params_exit;
	}

	clk_sel |= (idx & CLK_SEL_MSK);
	m_clk_cfg |= ((div << CLK_DIV_SHFT) | SER_CLK_EN);
	spi_setup_word_len(mas, spi_slv->mode, spi_slv->bits_per_word);
	geni_write_reg(loopback_cfg, mas->base, SE_SPI_LOOPBACK);
	geni_write_reg(demux_sel, mas->base, SE_SPI_DEMUX_SEL);
	geni_write_reg(cpha, mas->base, SE_SPI_CPHA);
	geni_write_reg(cpol, mas->base, SE_SPI_CPOL);
	geni_write_reg(demux_output_inv, mas->base, SE_SPI_DEMUX_OUTPUT_INV);
	geni_write_reg(clk_sel, mas->base, SE_GENI_CLK_SEL);
	geni_write_reg(m_clk_cfg, mas->base, GENI_SER_M_CLK_CFG);
	geni_write_reg(spi_delay_params, mas->base, SE_SPI_DELAY_COUNTERS);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"%s:Loopback%d demux_sel0x%x demux_op_inv 0x%x clk_cfg 0x%x\n",
		__func__, loopback_cfg, demux_sel, demux_output_inv, m_clk_cfg);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"%s:clk_sel 0x%x cpol %d cpha %d delay 0x%x\n", __func__,
					clk_sel, cpol, cpha, spi_delay_params);
	/* Ensure message level attributes are written before returning */
	mb();
setup_fifo_params_exit:
	return ret;
}


static int select_xfer_mode(struct spi_master *spi,
				struct spi_message *spi_msg)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	int mode = SE_DMA;
	int fifo_disable = (geni_read_reg(mas->base, GENI_IF_FIFO_DISABLE_RO) &
							FIFO_IF_DISABLE);
	bool dma_chan_valid =
		!(IS_ERR_OR_NULL(mas->tx) || IS_ERR_OR_NULL(mas->rx));

	/*
	 * If FIFO Interface is disabled and there are no DMA channels then we
	 * can't do this transfer.
	 * If FIFO interface is disabled, we can do GSI only,
	 * else pick FIFO mode.
	 */
	if (fifo_disable && !dma_chan_valid)
		mode = -EINVAL;
	else if (!fifo_disable)
		mode = SE_DMA;
	else if (dma_chan_valid)
		mode = GSI_DMA;
	return mode;
}

static struct msm_gpi_tre *setup_lock_tre(struct spi_geni_master *mas)
{
	struct msm_gpi_tre *lock_t = &mas->gsi_lock_unlock->lock_t;

	lock_t->dword[0] = MSM_GPI_LOCK_TRE_DWORD0;
	lock_t->dword[1] = MSM_GPI_LOCK_TRE_DWORD1;
	lock_t->dword[2] = MSM_GPI_LOCK_TRE_DWORD2;
	/* lock tre: ieob set */
	lock_t->dword[3] = MSM_GPI_LOCK_TRE_DWORD3(0, 0, 0, 1, 0);

	return lock_t;
}

static struct msm_gpi_tre *setup_config0_tre(struct spi_transfer *xfer,
				struct spi_geni_master *mas, u16 mode,
				u32 cs_clk_delay, u32 inter_words_delay)
{
	struct msm_gpi_tre *c0_tre = &mas->gsi[mas->num_xfers].config0_tre;
	u8 flags = 0;
	u8 word_len = 0;
	u8 pack = 0;
	int div = 0;
	int idx = 0;
	int ret = 0;
	int m_clk_cfg;

	if (IS_ERR_OR_NULL(c0_tre))
		return c0_tre;

	if (mode & SPI_LOOP)
		flags |= GSI_LOOPBACK_EN;

	if (mode & SPI_CPOL)
		flags |= GSI_CPOL;

	if (mode & SPI_CPHA)
		flags |= GSI_CPHA;

	word_len = xfer->bits_per_word - MIN_WORD_LEN;
	pack |= (GSI_TX_PACK_EN | GSI_RX_PACK_EN);
	if (mas->is_le_vm) {
		idx = geni_read_reg(mas->base, SE_GENI_CLK_SEL);
		m_clk_cfg = geni_read_reg(mas->base, GENI_SER_M_CLK_CFG);
		div = (m_clk_cfg & CLK_DIV_MSK) >> CLK_DIV_SHFT;
	} else {
		ret = get_spi_clk_cfg(mas->cur_speed_hz, mas, &idx, &div);
		if (ret) {
			dev_err(mas->dev, "%s:Err setting clks:%d\n",
				__func__, ret);
			return ERR_PTR(ret);
		}
	}

	c0_tre->dword[0] = MSM_GPI_SPI_CONFIG0_TRE_DWORD0(pack, flags,
								word_len);
	c0_tre->dword[1] = MSM_GPI_SPI_CONFIG0_TRE_DWORD1(0, cs_clk_delay,
							inter_words_delay);
	c0_tre->dword[2] = MSM_GPI_SPI_CONFIG0_TRE_DWORD2(idx, div);
	c0_tre->dword[3] = MSM_GPI_SPI_CONFIG0_TRE_DWORD3(0, 0, 0, 0, 1);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"%s: flags 0x%x word %d pack %d freq %d idx %d div %d\n",
		__func__, flags, word_len, pack, mas->cur_speed_hz, idx, div);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"%s: cs_clk_delay %d inter_words_delay %d\n", __func__,
				 cs_clk_delay, inter_words_delay);
	return c0_tre;
}

static struct msm_gpi_tre *setup_go_tre(int cmd, int cs, int rx_len, int flags,
				struct spi_geni_master *mas)
{
	struct msm_gpi_tre *go_tre = &mas->gsi[mas->num_xfers].go_tre;
	int chain;
	int eot;
	int eob;
	int link_rx = 0;

	if (IS_ERR_OR_NULL(go_tre))
		return go_tre;

	go_tre->dword[0] = MSM_GPI_SPI_GO_TRE_DWORD0(flags, cs, cmd);
	go_tre->dword[1] = MSM_GPI_SPI_GO_TRE_DWORD1;
	go_tre->dword[2] = MSM_GPI_SPI_GO_TRE_DWORD2(rx_len);
	if (cmd == SPI_RX_ONLY) {
		eot = 0;
		chain = 0;
		eob = 1;	/* GO TRE on TX: processing needed */
	} else {
		eot = 0;
		chain = 1;
		eob = 0;
	}
	if (cmd & SPI_RX_ONLY)
		link_rx = 1;
	go_tre->dword[3] = MSM_GPI_SPI_GO_TRE_DWORD3(link_rx, 0, eot, eob,
								chain);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
	"%s: rx len %d flags 0x%x cs %d cmd %d eot %d eob %d chain %d\n",
		__func__, rx_len, flags, cs, cmd, eot, eob, chain);
	return go_tre;
}

static struct msm_gpi_tre *setup_dma_tre(struct msm_gpi_tre *tre,
					dma_addr_t buf, u32 len,
					struct spi_geni_master *mas,
					bool is_tx)
{
	if (IS_ERR_OR_NULL(tre))
		return tre;

	tre->dword[0] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD0(buf);
	tre->dword[1] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD1(buf);
	tre->dword[2] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD2(len);
	tre->dword[3] = MSM_GPI_DMA_W_BUFFER_TRE_DWORD3(0, 0, is_tx, 0, 0);
	return tre;
}

static struct msm_gpi_tre *setup_unlock_tre(struct spi_geni_master *mas)
{
	struct msm_gpi_tre *unlock_t = &mas->gsi_lock_unlock->unlock_t;

	/* unlock tre: ieob set */
	unlock_t->dword[0] = MSM_GPI_UNLOCK_TRE_DWORD0;
	unlock_t->dword[1] = MSM_GPI_UNLOCK_TRE_DWORD1;
	unlock_t->dword[2] = MSM_GPI_UNLOCK_TRE_DWORD2;
	unlock_t->dword[3] = MSM_GPI_UNLOCK_TRE_DWORD3(0, 0, 0, 1, 0);

	return unlock_t;
}

static void spi_gsi_ch_cb(struct dma_chan *ch, struct msm_gpi_cb const *cb,
				void *ptr)
{
	struct spi_master *spi = ptr;
	struct spi_geni_master *mas;

	if (!ptr || !cb) {
		pr_err("%s: Invalid ev_cb buffer\n", __func__);
		return;
	}

	mas = spi_master_get_devdata(spi);
	switch (cb->cb_event) {
	case MSM_GPI_QUP_NOTIFY:
	case MSM_GPI_QUP_MAX_EVENT:
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
				"%s:cb_ev%d status%llu ts%llu count%llu\n",
				__func__, cb->cb_event, cb->status,
				cb->timestamp, cb->count);
		break;
	case MSM_GPI_QUP_ERROR:
	case MSM_GPI_QUP_CH_ERROR:
	case MSM_GPI_QUP_FW_ERROR:
	case MSM_GPI_QUP_PENDING_EVENT:
	case MSM_GPI_QUP_EOT_DESC_MISMATCH:
	case MSM_GPI_QUP_SW_ERROR:
		SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"%s: cb_ev %d status %llu ts %llu count %llu\n",
				__func__, cb->cb_event, cb->status,
				cb->timestamp, cb->count);
		SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"err.routine %u, err.type %u, err.code %u\n",
				cb->error_log.routine,
				cb->error_log.type,
				cb->error_log.error_code);
		mas->qn_err = true;
		complete_all(&mas->tx_cb);
		complete_all(&mas->rx_cb);

		break;
	}
}

static void spi_gsi_rx_callback(void *cb)
{
	struct msm_gpi_dma_async_tx_cb_param *cb_param =
			(struct msm_gpi_dma_async_tx_cb_param *)cb;
	struct gsi_desc_cb *desc_cb;
	struct spi_master *spi;
	struct spi_transfer *xfer;
	struct spi_geni_master *mas;

	if (!(cb_param && cb_param->userdata)) {
		pr_err("%s: Invalid rx_cb buffer\n", __func__);
		return;
	}

	desc_cb = (struct gsi_desc_cb *)cb_param->userdata;
	spi = desc_cb->spi;
	xfer = desc_cb->xfer;
	mas = spi_master_get_devdata(spi);

	if (xfer->rx_buf) {
		if (cb_param->status == MSM_GPI_TCE_UNEXP_ERR) {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
			"%s: Unexpected GSI CB error\n", __func__);
			return;
		}
		if (cb_param->length == xfer->len) {
			SPI_LOG_DBG(mas->ipc, false, mas->dev,
			"%s\n", __func__);
			complete(&mas->rx_cb);
		} else {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
			"%s: Length mismatch. Expected %d Callback %d\n",
			__func__, xfer->len, cb_param->length);
		}
	}
}

static void spi_gsi_tx_callback(void *cb)
{
	struct msm_gpi_dma_async_tx_cb_param *cb_param = cb;
	struct gsi_desc_cb *desc_cb;
	struct spi_master *spi;
	struct spi_transfer *xfer;
	struct spi_geni_master *mas;

	if (!(cb_param && cb_param->userdata)) {
		pr_err("%s: Invalid tx_cb buffer\n", __func__);
		return;
	}

	desc_cb = (struct gsi_desc_cb *)cb_param->userdata;
	spi = desc_cb->spi;
	xfer = desc_cb->xfer;
	mas = spi_master_get_devdata(spi);

	/*
	 * Case when lock/unlock support is required:
	 * The callback comes on tx channel as lock/unlock
	 * tres are submitted on tx channel. Check if there's
	 * no xfer scheduled, that specifies a gsi completion
	 * callback for lock/unlock tre being submitted.
	 */
	if (!xfer) {
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"Lock/unlock IEOB received %s\n", __func__);
		complete(&mas->tx_cb);
		return;
	}

	if (xfer->tx_buf) {
		if (cb_param->status == MSM_GPI_TCE_UNEXP_ERR) {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
			"%s: Unexpected GSI CB error\n", __func__);
			return;
		}
		if (cb_param->length == xfer->len) {
			SPI_LOG_DBG(mas->ipc, false, mas->dev,
			"%s\n", __func__);
			complete(&mas->tx_cb);
		} else {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
			"%s: Length mismatch. Expected %d Callback %d\n",
			__func__, xfer->len, cb_param->length);
		}
	}
}

/*
 * Locking the GPII:
 * For a shared_se usecase, lock the bus per message.
 * Lock bus is done in prepare_message and unlock bus
 * is done in unprepare_message.
 * For an LE-VM usecase, lock the bus per session.
 * Lock bus is done in runtime_resume and unlock
 * bus is done in runtime_suspend.
 */
static int spi_geni_lock_bus(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	struct msm_gpi_tre *lock_t = NULL;
	int ret = 0, timeout = 0;
	struct scatterlist *xfer_tx_sg = mas->gsi_lock_unlock->tx_sg;
	unsigned long flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;

	reinit_completion(&mas->tx_cb);

	SPI_LOG_DBG(mas->ipc, false, mas->dev, "%s\n", __func__);

	lock_t = setup_lock_tre(mas);
	sg_init_table(xfer_tx_sg, 1);
	sg_set_buf(xfer_tx_sg, lock_t, sizeof(*lock_t));
	mas->gsi_lock_unlock->desc_cb.spi = spi;

	mas->gsi_lock_unlock->tx_desc = dmaengine_prep_slave_sg(mas->tx,
					mas->gsi_lock_unlock->tx_sg, 1,
					DMA_MEM_TO_DEV, flags);
	if (IS_ERR_OR_NULL(mas->gsi_lock_unlock->tx_desc)) {
		dev_err(mas->dev, "Err setting up tx desc\n");
		ret = -EIO;
		goto err_spi_geni_lock_bus;
	}

	mas->gsi_lock_unlock->tx_desc->callback = spi_gsi_tx_callback;
	mas->gsi_lock_unlock->tx_desc->callback_param =
					&mas->gsi_lock_unlock->tx_cb_param;
	mas->gsi_lock_unlock->tx_cb_param.userdata =
					&mas->gsi_lock_unlock->desc_cb;
	/* Issue TX */
	mas->gsi_lock_unlock->tx_cookie =
			dmaengine_submit(mas->gsi_lock_unlock->tx_desc);
	dma_async_issue_pending(mas->tx);

	timeout = wait_for_completion_timeout(&mas->tx_cb,
					msecs_to_jiffies(SPI_XFER_TIMEOUT_MS));
	if (timeout <= 0) {
		SPI_LOG_ERR(mas->ipc, true, mas->dev,
		"%s failed\n", __func__);
		geni_se_dump_dbg_regs(&mas->spi_rsc, mas->base, mas->ipc);
		ret = -ETIMEDOUT;
		goto err_spi_geni_lock_bus;
	}
	return ret;
err_spi_geni_lock_bus:
	if (ret)
		dmaengine_terminate_all(mas->tx);
	return ret;
}

static void spi_geni_unlock_bus(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	struct msm_gpi_tre *unlock_t = NULL;
	int ret = 0, timeout = 0;
	struct scatterlist *xfer_tx_sg = mas->gsi_lock_unlock->tx_sg;
	unsigned long flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;

	/* if gpi reset happened for levm, no need to do unlock */
	if (mas->is_le_vm && mas->le_gpi_reset_done) {
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
			    "%s:gpi reset happened for levm, no need to do unlock\n", __func__);
		return;
	}

	reinit_completion(&mas->tx_cb);

	SPI_LOG_DBG(mas->ipc, false, mas->dev, "%s\n", __func__);

	unlock_t = setup_unlock_tre(mas);
	sg_init_table(xfer_tx_sg, 1);
	sg_set_buf(xfer_tx_sg, unlock_t, sizeof(*unlock_t));
	mas->gsi_lock_unlock->desc_cb.spi = spi;

	mas->gsi_lock_unlock->tx_desc = dmaengine_prep_slave_sg(mas->tx,
					mas->gsi_lock_unlock->tx_sg, 1,
					DMA_MEM_TO_DEV, flags);
	if (IS_ERR_OR_NULL(mas->gsi_lock_unlock->tx_desc)) {
		dev_err(mas->dev, "Err setting up tx desc\n");
		ret = -EIO;
		goto err_spi_geni_unlock_bus;
	}

	mas->gsi_lock_unlock->tx_desc->callback = spi_gsi_tx_callback;
	mas->gsi_lock_unlock->tx_desc->callback_param =
					&mas->gsi_lock_unlock->tx_cb_param;
	mas->gsi_lock_unlock->tx_cb_param.userdata =
					&mas->gsi_lock_unlock->desc_cb;
	/* Issue TX */
	mas->gsi_lock_unlock->tx_cookie =
			dmaengine_submit(mas->gsi_lock_unlock->tx_desc);
	dma_async_issue_pending(mas->tx);

	timeout = wait_for_completion_timeout(&mas->tx_cb,
					msecs_to_jiffies(SPI_XFER_TIMEOUT_MS));
	if (timeout <= 0) {
		SPI_LOG_ERR(mas->ipc, true, mas->dev,
			"%s failed\n", __func__);
		geni_se_dump_dbg_regs(&mas->spi_rsc, mas->base, mas->ipc);
		ret = -ETIMEDOUT;
		goto err_spi_geni_unlock_bus;
	}

err_spi_geni_unlock_bus:
	if (ret)
		dmaengine_terminate_all(mas->tx);
}

static int setup_gsi_xfer(struct spi_transfer *xfer,
				struct spi_geni_master *mas,
				struct spi_device *spi_slv,
				struct spi_master *spi)
{
	int ret = 0;
	struct msm_gpi_tre *c0_tre = NULL;
	struct msm_gpi_tre *go_tre = NULL;
	struct msm_gpi_tre *tx_tre = NULL;
	struct msm_gpi_tre *rx_tre = NULL;
	struct scatterlist *xfer_tx_sg = mas->gsi[mas->num_xfers].tx_sg;
	struct scatterlist *xfer_rx_sg = &mas->gsi[mas->num_xfers].rx_sg;
	int rx_nent = 0;
	int tx_nent = 0;
	u8 cmd = 0;
	u8 cs = 0;
	u32 rx_len = 0;
	int go_flags = 0;
	unsigned long flags = DMA_PREP_INTERRUPT | DMA_CTRL_ACK;
	struct spi_geni_qcom_ctrl_data *delay_params = NULL;
	u32 cs_clk_delay = 0;
	u32 inter_words_delay = 0;

	if (mas->is_le_vm && mas->le_gpi_reset_done) {
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
			    "%s doing gsi lock, due to levm gsi reset\n", __func__);
		ret = spi_geni_lock_bus(spi);
		if (ret) {
			SPI_LOG_DBG(mas->ipc, true, mas->dev,
				    "%s lock bus failed: %d\n", __func__, ret);
			return ret;
		}
		mas->le_gpi_reset_done = false;
	}

	if (spi_slv->controller_data) {
		delay_params =
		(struct spi_geni_qcom_ctrl_data *) spi_slv->controller_data;

		cs_clk_delay =
			delay_params->spi_cs_clk_delay;
		inter_words_delay =
			delay_params->spi_inter_words_delay;
	}

	if ((xfer->bits_per_word != mas->cur_word_len) ||
		(xfer->speed_hz != mas->cur_speed_hz)) {
		mas->cur_word_len = xfer->bits_per_word;
		mas->cur_speed_hz = xfer->speed_hz;
		tx_nent++;
		c0_tre = setup_config0_tre(xfer, mas, spi_slv->mode,
					cs_clk_delay, inter_words_delay);
		if (IS_ERR_OR_NULL(c0_tre)) {
			dev_err(mas->dev, "%s:Err setting c0tre:%d\n",
							__func__, ret);
			return PTR_ERR(c0_tre);
		}
	}

	if (!(mas->cur_word_len % MIN_WORD_LEN)) {
		rx_len = ((xfer->len << 3) / mas->cur_word_len);
	} else {
		int bytes_per_word = (mas->cur_word_len / BITS_PER_BYTE) + 1;

		rx_len = (xfer->len / bytes_per_word);
	}

	if (xfer->tx_buf && xfer->rx_buf) {
		cmd = SPI_FULL_DUPLEX;
		tx_nent += 2;
		rx_nent++;
	} else if (xfer->tx_buf) {
		cmd = SPI_TX_ONLY;
		tx_nent += 2;
		rx_len = 0;
	} else if (xfer->rx_buf) {
		cmd = SPI_RX_ONLY;
		tx_nent++;
		rx_nent++;
	}

	cs |= spi_slv->chip_select;
	if (!xfer->cs_change) {
		if (!list_is_last(&xfer->transfer_list,
					&spi->cur_msg->transfers))
			go_flags |= FRAGMENTATION;
	}
	go_tre = setup_go_tre(cmd, cs, rx_len, go_flags, mas);

	sg_init_table(xfer_tx_sg, tx_nent);
	if (rx_nent)
		sg_init_table(xfer_rx_sg, rx_nent);

	if (c0_tre)
		sg_set_buf(xfer_tx_sg++, c0_tre, sizeof(*c0_tre));

	sg_set_buf(xfer_tx_sg++, go_tre, sizeof(*go_tre));
	mas->gsi[mas->num_xfers].desc_cb.spi = spi;
	mas->gsi[mas->num_xfers].desc_cb.xfer = xfer;
	if (cmd & SPI_RX_ONLY) {
		rx_tre = &mas->gsi[mas->num_xfers].rx_dma_tre;
		rx_tre = setup_dma_tre(rx_tre, xfer->rx_dma, xfer->len, mas, 0);
		if (IS_ERR_OR_NULL(rx_tre)) {
			dev_err(mas->dev, "Err setting up rx tre\n");
			return PTR_ERR(rx_tre);
		}
		sg_set_buf(xfer_rx_sg, rx_tre, sizeof(*rx_tre));
		mas->gsi[mas->num_xfers].rx_desc =
			dmaengine_prep_slave_sg(mas->rx,
				&mas->gsi[mas->num_xfers].rx_sg, rx_nent,
						DMA_DEV_TO_MEM, flags);
		if (IS_ERR_OR_NULL(mas->gsi[mas->num_xfers].rx_desc)) {
			dev_err(mas->dev, "Err setting up rx desc\n");
			return -EIO;
		}
		mas->gsi[mas->num_xfers].rx_desc->callback =
					spi_gsi_rx_callback;
		mas->gsi[mas->num_xfers].rx_desc->callback_param =
					&mas->gsi[mas->num_xfers].rx_cb_param;
		mas->gsi[mas->num_xfers].rx_cb_param.userdata =
					&mas->gsi[mas->num_xfers].desc_cb;
		mas->num_rx_eot++;
	}

	if (cmd & SPI_TX_ONLY) {
		tx_tre = &mas->gsi[mas->num_xfers].tx_dma_tre;
		tx_tre = setup_dma_tre(tx_tre, xfer->tx_dma, xfer->len, mas, 1);
		if (IS_ERR_OR_NULL(tx_tre)) {
			dev_err(mas->dev, "Err setting up tx tre\n");
			return PTR_ERR(tx_tre);
		}
		sg_set_buf(xfer_tx_sg++, tx_tre, sizeof(*tx_tre));
		mas->num_tx_eot++;
	}
	mas->gsi[mas->num_xfers].tx_desc = dmaengine_prep_slave_sg(mas->tx,
					mas->gsi[mas->num_xfers].tx_sg, tx_nent,
					DMA_MEM_TO_DEV, flags);
	if (IS_ERR_OR_NULL(mas->gsi[mas->num_xfers].tx_desc)) {
		dev_err(mas->dev, "Err setting up tx desc\n");
		return -EIO;
	}
	mas->gsi[mas->num_xfers].tx_desc->callback = spi_gsi_tx_callback;
	mas->gsi[mas->num_xfers].tx_desc->callback_param =
					&mas->gsi[mas->num_xfers].tx_cb_param;
	mas->gsi[mas->num_xfers].tx_cb_param.userdata =
					&mas->gsi[mas->num_xfers].desc_cb;
	mas->gsi[mas->num_xfers].tx_cookie =
			dmaengine_submit(mas->gsi[mas->num_xfers].tx_desc);
	if (cmd & SPI_RX_ONLY)
		mas->gsi[mas->num_xfers].rx_cookie =
			dmaengine_submit(mas->gsi[mas->num_xfers].rx_desc);
	dma_async_issue_pending(mas->tx);
	if (cmd & SPI_RX_ONLY)
		dma_async_issue_pending(mas->rx);
	mas->num_xfers++;
	return ret;
}

static int spi_geni_map_buf(struct spi_geni_master *mas,
				struct spi_message *msg)
{
	struct spi_transfer *xfer;
	int ret = 0;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (xfer->rx_buf) {
			ret = geni_se_iommu_map_buf(mas->wrapper_dev,
						&xfer->rx_dma, xfer->rx_buf,
						xfer->len, DMA_FROM_DEVICE);
			if (ret) {
				SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"%s: Mapping Rx buffer %d\n", __func__, ret);
				return ret;
			}
		}

		if (xfer->tx_buf) {
			ret = geni_se_iommu_map_buf(mas->wrapper_dev,
						&xfer->tx_dma,
						(void *)xfer->tx_buf,
						xfer->len, DMA_TO_DEVICE);
			if (ret) {
				SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"%s: Mapping Tx buffer %d\n", __func__, ret);
				return ret;
			}
		}
	}
	return 0;
}

static void spi_geni_unmap_buf(struct spi_geni_master *mas,
				struct spi_message *msg)
{
	struct spi_transfer *xfer;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		if (xfer->rx_buf)
			geni_se_iommu_unmap_buf(mas->wrapper_dev, &xfer->rx_dma,
						xfer->len, DMA_FROM_DEVICE);
		if (xfer->tx_buf)
			geni_se_iommu_unmap_buf(mas->wrapper_dev, &xfer->tx_dma,
						xfer->len, DMA_TO_DEVICE);
	}
}

static int spi_geni_prepare_message(struct spi_master *spi,
					struct spi_message *spi_msg)
{
	int ret = 0;
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	int count;

	if (mas->shared_ee) {
		if (mas->setup) {
			/* Client to respect system suspend */
			if (!pm_runtime_enabled(mas->dev)) {
				SPI_LOG_ERR(mas->ipc, false, mas->dev,
					"%s: System suspended\n", __func__);
				return -EACCES;
			}

			ret = pm_runtime_get_sync(mas->dev);
			if (ret < 0) {
				dev_err(mas->dev,
					"%s:pm_runtime_get_sync failed %d\n",
							__func__, ret);
				WARN_ON_ONCE(1);
				pm_runtime_put_noidle(mas->dev);
				/* Set device in suspended since resume failed */
				pm_runtime_set_suspended(mas->dev);
				goto exit_prepare_message;
			}
			ret = 0;

			if (mas->dis_autosuspend) {
				count =
				atomic_read(&mas->dev->power.usage_count);
				if (count <= 0)
					SPI_LOG_ERR(mas->ipc, false, mas->dev,
					"resume usage count mismatch:%d",
								count);
			}
		} else {
			mas->setup = true;
		}

		if (mas->shared_se) {
			ret = spi_geni_lock_bus(spi);
			if (ret) {
				SPI_LOG_ERR(mas->ipc, true, mas->dev,
					"%s failed: %d\n", __func__, ret);
				return ret;
			}
		}
	}

	if (pm_runtime_status_suspended(mas->dev) && !mas->is_le_vm) {
		if (!pm_runtime_enabled(mas->dev)) {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"%s: System suspended\n", __func__);
			return -EACCES;
		}

		ret = pm_runtime_get_sync(mas->dev);
		if (ret < 0) {
			dev_err(mas->dev,
			"%s:pm_runtime_get_sync failed %d\n", __func__, ret);
			WARN_ON_ONCE(1);
			pm_runtime_put_noidle(mas->dev);
			/* Set device in suspended since resume failed */
			pm_runtime_set_suspended(mas->dev);
			return ret;
		}
	}

	mas->cur_xfer_mode = select_xfer_mode(spi, spi_msg);

	if (mas->cur_xfer_mode < 0) {
		dev_err(mas->dev, "%s: Couldn't select mode %d\n", __func__,
							mas->cur_xfer_mode);
		ret = -EINVAL;
	} else if (mas->cur_xfer_mode == GSI_DMA) {
		memset(mas->gsi, 0,
				(sizeof(struct spi_geni_gsi) * NUM_SPI_XFER));
		geni_se_select_mode(mas->base, GSI_DMA);
		ret = spi_geni_map_buf(mas, spi_msg);
	} else {
		geni_se_select_mode(mas->base, mas->cur_xfer_mode);
		ret = setup_fifo_params(spi_msg->spi, spi);
	}

exit_prepare_message:
	return ret;
}

static int spi_geni_unprepare_message(struct spi_master *spi_mas,
					struct spi_message *spi_msg)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi_mas);
	int count = 0;

	mas->cur_speed_hz = 0;
	mas->cur_word_len = 0;
	if (mas->cur_xfer_mode == GSI_DMA)
		spi_geni_unmap_buf(mas, spi_msg);

	if (mas->shared_ee) {
		if (mas->shared_se)
			spi_geni_unlock_bus(spi_mas);

		if (mas->dis_autosuspend) {
			pm_runtime_put_sync(mas->dev);
			count = atomic_read(&mas->dev->power.usage_count);
			if (count < 0)
				SPI_LOG_ERR(mas->ipc, false, mas->dev,
					"suspend usage count mismatch:%d",
								count);
		} else if (!pm_runtime_status_suspended(mas->dev) &&
				pm_runtime_enabled(mas->dev)) {
			pm_runtime_mark_last_busy(mas->dev);
			pm_runtime_put_autosuspend(mas->dev);
		}
	}

	return 0;
}

static void spi_geni_set_sampling_rate(struct spi_geni_master *mas,
	unsigned int major, unsigned int minor)
{
	u32 cpol, cpha, cfg_reg108, cfg_reg109, cfg_seq_start;

	cpol = geni_read_reg(mas->base, SE_SPI_CPOL);
	cpha = geni_read_reg(mas->base, SE_SPI_CPHA);
	cfg_reg108 = geni_read_reg(mas->base, SE_GENI_CFG_REG108);
	cfg_reg109 = geni_read_reg(mas->base, SE_GENI_CFG_REG109);
	/* clear CPOL bit */
	cfg_reg108 &= ~(1 << CPOL_CTRL_SHFT);

	if (major == 1 && minor == 0) {
		/* Write 1 to RX_SI_EN2IO_DELAY reg */
		cfg_reg108 &= ~(0x7 << RX_SI_EN2IO_DELAY_SHFT);
		cfg_reg108 |= (1 << RX_SI_EN2IO_DELAY_SHFT);
		/* Write 0 to RX_IO_POS_FF_EN_SEL reg */
		cfg_reg108 &= ~(1 << RX_IO_POS_FF_EN_SEL_SHFT);
	} else if ((major < 2) || (major == 2 && minor < 5)) {
		/* Write 0 to RX_IO_EN2CORE_EN_DELAY reg */
		cfg_reg108 &= ~(0x7 << RX_IO_EN2CORE_EN_DELAY_SHFT);
	} else {
		/*
		 * Write miso_sampling_ctrl_set to
		 * RX_IO_EN2CORE_EN_DELAY reg
		 */
		cfg_reg108 &= ~(0x7 << RX_IO_EN2CORE_EN_DELAY_SHFT);
		cfg_reg108 |= (mas->miso_sampling_ctrl_val <<
				RX_IO_EN2CORE_EN_DELAY_SHFT);
	}

	geni_write_reg(cfg_reg108, mas->base, SE_GENI_CFG_REG108);

	if (cpol == 0 && cpha == 0)
		cfg_reg109 = 1;
	else if (cpol == 1 && cpha == 0)
		cfg_reg109 = 0;
	geni_write_reg(cfg_reg109, mas->base,
				SE_GENI_CFG_REG109);
	if (!(major == 1 && minor == 0))
		geni_write_reg(1, mas->base, SE_GENI_CFG_SEQ_START);
	cfg_reg108 = geni_read_reg(mas->base, SE_GENI_CFG_REG108);
	cfg_reg109 = geni_read_reg(mas->base, SE_GENI_CFG_REG109);
	cfg_seq_start = geni_read_reg(mas->base, SE_GENI_CFG_SEQ_START);

	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"%s cfg108: 0x%x cfg109: 0x%x cfg_seq_start: 0x%x\n",
		__func__, cfg_reg108, cfg_reg109, cfg_seq_start);
}

/*
 * spi_geni_mas_setup is done once per spi session.
 * In LA, it is called in prepare_transfer_hardware whereas
 * in LE, it is called in runtime_resume. Make sure this api
 * is called before any actual transfer begins as it involves
 * generic SW/HW intializations required for a spi transfer.
 */
static int spi_geni_mas_setup(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	int proto = get_se_proto(mas->base);
	unsigned int major;
	unsigned int minor;
	unsigned int step;
	int hw_ver;
	int ret = 0;

	if (spi->slave) {
		if (mas->slave_setup)
			goto setup_ipc;
		proto = get_se_proto(mas->base);
		if (unlikely(proto != SPI_SLAVE)) {
			dev_err(mas->dev, "Invalid proto %d\n", proto);
			return -ENXIO;
		}
	}

	if (unlikely(!mas->setup)) {
		proto = get_se_proto(mas->base);

		if ((unlikely(proto != SPI)) && (!spi->slave)) {
			dev_err(mas->dev, "Invalid proto %d\n", proto);
			return -ENXIO;
		}

		if (spi->slave)
			spi_slv_setup(mas);

		if (mas->master_cross_connect)
			spi_master_setup(mas);
	}

	mas->tx_fifo_depth = get_tx_fifo_depth(&mas->spi_rsc);
	mas->rx_fifo_depth = get_rx_fifo_depth(&mas->spi_rsc);
	mas->tx_fifo_width = get_tx_fifo_width(mas->base);
	mas->oversampling = 1;
	geni_se_init(mas->base, 0x0, (mas->tx_fifo_depth - 2));

	/* Transmit an entire FIFO worth of data per IRQ */
	mas->tx_wm = 1;

	mas->gsi_mode =
		(geni_read_reg(mas->base, GENI_IF_FIFO_DISABLE_RO) &
					FIFO_IF_DISABLE);

	if (mas->gsi_mode) {
		mas->tx = dma_request_slave_channel(mas->dev, "tx");
		if (IS_ERR_OR_NULL(mas->tx)) {
			dev_info(mas->dev, "Failed to get tx DMA ch %ld\n",
						PTR_ERR(mas->tx));
			goto setup_ipc;
		}
		mas->rx = dma_request_slave_channel(mas->dev, "rx");
		if (IS_ERR_OR_NULL(mas->rx)) {
			dev_info(mas->dev, "Failed to get rx DMA ch %ld\n",
						PTR_ERR(mas->rx));
			dma_release_channel(mas->tx);
			goto setup_ipc;
		}
		mas->gsi = devm_kzalloc(mas->dev,
			(sizeof(struct spi_geni_gsi) * NUM_SPI_XFER),
			GFP_KERNEL);
		if (IS_ERR_OR_NULL(mas->gsi)) {
			dev_err(mas->dev, "Failed to get GSI mem\n");
			dma_release_channel(mas->tx);
			dma_release_channel(mas->rx);
			mas->tx = NULL;
			mas->rx = NULL;
			goto setup_ipc;
		}
		if (mas->shared_se || mas->is_le_vm) {
			mas->gsi_lock_unlock = devm_kzalloc(mas->dev,
				(sizeof(struct spi_geni_gsi)),
				GFP_KERNEL);
			if (IS_ERR_OR_NULL(mas->gsi_lock_unlock)) {
				dev_err(mas->dev, "Failed to get GSI lock mem\n");
				dma_release_channel(mas->tx);
				dma_release_channel(mas->rx);
				mas->tx = NULL;
				mas->rx = NULL;
				goto setup_ipc;
			}
		}
		mas->tx_event.init.callback = spi_gsi_ch_cb;
		mas->tx_event.init.cb_param = spi;
		mas->tx_event.cmd = MSM_GPI_INIT;
		mas->tx->private = &mas->tx_event;
		mas->rx_event.init.callback = spi_gsi_ch_cb;
		mas->rx_event.init.cb_param = spi;
		mas->rx_event.cmd = MSM_GPI_INIT;
		mas->rx->private = &mas->rx_event;
		ret = dmaengine_slave_config(mas->tx, NULL);
		if (ret) {
			dev_err(mas->dev, "Failed to Config Tx, ret:%d\n", ret);
			dma_release_channel(mas->tx);
			dma_release_channel(mas->rx);
			mas->tx = NULL;
			mas->rx = NULL;
			goto setup_ipc;
		}
		ret = dmaengine_slave_config(mas->rx, NULL);
		if (ret) {
			dev_err(mas->dev, "Failed to Config Rx, ret:%d\n", ret);
			dma_release_channel(mas->tx);
			dma_release_channel(mas->rx);
			mas->tx = NULL;
			mas->rx = NULL;
			goto setup_ipc;
		}
	}
setup_ipc:
	dev_info(mas->dev, "tx_fifo %d rx_fifo %d tx_width %d\n",
		mas->tx_fifo_depth, mas->rx_fifo_depth,
		mas->tx_fifo_width);
	if (!mas->shared_ee)
		mas->setup = true;

	if (spi->slave)
		mas->slave_setup = true;

	/*
	 * Bypass hw_version read for LE. QUP common registers
	 * should not be accessed from SVM as that memory is
	 * assigned to PVM. So, bypass the reading of hw version
	 *  registers and rely on PVM for the specific HW initialization
	 *  done based on different hw versions.
	 */
	if (mas->is_le_vm)
		return ret;

	hw_ver = geni_se_qupv3_hw_version(mas->wrapper_dev, &major,
						&minor, &step);
	if (hw_ver)
		dev_err(mas->dev, "%s:Err getting HW version %d\n",
						__func__, hw_ver);
	else {
		if ((major == 1) && (minor == 0))
			mas->oversampling = 2;
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
			"%s:Major:%d Minor:%d step:%dos%d\n",
		__func__, major, minor, step, mas->oversampling);
	}
	if (mas->set_miso_sampling)
		spi_geni_set_sampling_rate(mas, major, minor);

	if (mas->dis_autosuspend)
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
				"Auto Suspend is disabled\n");
	return ret;
}

static int spi_geni_prepare_transfer_hardware(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	int ret = 0, count = 0;

	/*
	 * Not required for LE as below intializations are specific
	 * to usecases. For LE, client takes care of get_sync.
	 */
	if (mas->is_le_vm)
		return 0;

	/* Client to respect system suspend */
	if (!pm_runtime_enabled(mas->dev)) {
		SPI_LOG_ERR(mas->ipc, false, mas->dev,
			"%s: System suspended\n", __func__);
		return -EACCES;
	}

	if (mas->gsi_mode && !mas->shared_ee) {
		struct se_geni_rsc *rsc;
		int ret = 0;

		if (!mas->is_la_vm) {
			/* Do this only for non TVM LA usecase */
			/* May not be needed here, but maintain parity */
			rsc = &mas->spi_rsc;
			ret = pinctrl_select_state(rsc->geni_pinctrl,
						rsc->geni_gpio_active);
		}

		if (ret)
			SPI_LOG_ERR(mas->ipc, false, mas->dev,
			"%s: Error %d pinctrl_select_state\n", __func__, ret);
	}

	if (!mas->setup || !mas->shared_ee) {
		ret = pm_runtime_get_sync(mas->dev);
		if (ret < 0) {
			dev_err(mas->dev,
				"%s:pm_runtime_get_sync failed %d\n",
							__func__, ret);
			WARN_ON_ONCE(1);
			pm_runtime_put_noidle(mas->dev);
			/* Set device in suspended since resume failed */
			pm_runtime_set_suspended(mas->dev);
			return ret;
		}

		if (!mas->setup) {
			ret = spi_geni_mas_setup(spi);
			if (ret) {
				SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"%s mas_setup failed: %d\n", __func__, ret);
				return ret;
			}
		}
		ret = 0;

		if (mas->dis_autosuspend) {
			count = atomic_read(&mas->dev->power.usage_count);
			if (count <= 0)
				SPI_LOG_ERR(mas->ipc, false, mas->dev,
				"resume usage count mismatch:%d", count);
		}
	}

	return ret;
}

static int spi_geni_unprepare_transfer_hardware(struct spi_master *spi)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	int count = 0;

	if (mas->shared_ee || mas->is_le_vm)
		return 0;

	if (mas->gsi_mode) {
		struct se_geni_rsc *rsc;
		int ret = 0;

		if (!mas->is_la_vm) {
			/* Do this only for non TVM LA usecase */
			rsc = &mas->spi_rsc;
			ret = pinctrl_select_state(rsc->geni_pinctrl,
						rsc->geni_gpio_sleep);
		}

		if (ret)
			SPI_LOG_ERR(mas->ipc, false, mas->dev,
			"%s: Error %d pinctrl_select_state\n", __func__, ret);
	}

	if (mas->dis_autosuspend) {
		pm_runtime_put_sync(mas->dev);
		count = atomic_read(&mas->dev->power.usage_count);
		if (count < 0)
			SPI_LOG_ERR(mas->ipc, false, mas->dev,
				"suspend usage count mismatch:%d", count);
	} else if (!pm_runtime_status_suspended(mas->dev) &&
			pm_runtime_enabled(mas->dev)) {
		pm_runtime_mark_last_busy(mas->dev);
		pm_runtime_put_autosuspend(mas->dev);
	}
	return 0;
}

static int setup_fifo_xfer(struct spi_transfer *xfer,
				struct spi_geni_master *mas, u16 mode,
				struct spi_master *spi)
{
	int ret = 0;
	u32 m_cmd = 0;
	u32 m_param = 0;
	u32 spi_tx_cfg = geni_read_reg(mas->base, SE_SPI_TRANS_CFG);
	u32 trans_len = 0, fifo_size = 0;

	if (xfer->bits_per_word != mas->cur_word_len) {
		spi_setup_word_len(mas, mode, xfer->bits_per_word);
		mas->cur_word_len = xfer->bits_per_word;
	}

	/* Speed and bits per word can be overridden per transfer */
	if (xfer->speed_hz != mas->cur_speed_hz) {
		u32 clk_sel = 0;
		u32 m_clk_cfg = 0;
		int idx = 0;
		int div = 0;

		ret = get_spi_clk_cfg(xfer->speed_hz, mas, &idx, &div);
		if (ret) {
			dev_err(mas->dev, "%s:Err setting clks:%d\n",
								__func__, ret);
			return ret;
		}
		mas->cur_speed_hz = xfer->speed_hz;
		clk_sel |= (idx & CLK_SEL_MSK);
		m_clk_cfg |= ((div << CLK_DIV_SHFT) | SER_CLK_EN);
		geni_write_reg(clk_sel, mas->base, SE_GENI_CLK_SEL);
		geni_write_reg(m_clk_cfg, mas->base, GENI_SER_M_CLK_CFG);
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
			"%s: freq %d idx %d div %d\n", __func__, xfer->speed_hz, idx, div);
	}

	mas->tx_rem_bytes = 0;
	mas->rx_rem_bytes = 0;
	if (xfer->tx_buf && xfer->rx_buf)
		m_cmd = SPI_FULL_DUPLEX;
	else if (xfer->tx_buf)
		m_cmd = SPI_TX_ONLY;
	else if (xfer->rx_buf)
		m_cmd = SPI_RX_ONLY;

	if (!spi->slave)
		spi_tx_cfg &= ~CS_TOGGLE;

	if (!(mas->cur_word_len % MIN_WORD_LEN)) {
		trans_len =
			((xfer->len << 3) / mas->cur_word_len) & TRANS_LEN_MSK;
	} else {
		int bytes_per_word = (mas->cur_word_len / BITS_PER_BYTE) + 1;

		trans_len = (xfer->len / bytes_per_word) & TRANS_LEN_MSK;
	}

	if (!xfer->cs_change) {
		if (!list_is_last(&xfer->transfer_list,
					&spi->cur_msg->transfers))
			m_param |= FRAGMENTATION;
	}

	mas->cur_xfer = xfer;
	if (m_cmd & SPI_TX_ONLY) {
		mas->tx_rem_bytes = xfer->len;
		geni_write_reg(trans_len, mas->base, SE_SPI_TX_TRANS_LEN);
	}

	if (m_cmd & SPI_RX_ONLY) {
		geni_write_reg(trans_len, mas->base, SE_SPI_RX_TRANS_LEN);
		mas->rx_rem_bytes = xfer->len;
	}

	fifo_size =
		(mas->tx_fifo_depth * mas->tx_fifo_width / mas->cur_word_len);
	/*
	 * Controller has support to transfer data either in FIFO mode
	 * or in SE_DMA mode. Either force the controller to choose FIFO
	 * mode for transfers or select the mode dynamically based on
	 * size of data.
	 */
	if (spi->slave)
		mas->cur_xfer_mode = SE_DMA;

	if (mas->disable_dma || trans_len <= fifo_size)
		mas->cur_xfer_mode = FIFO_MODE;
	geni_se_select_mode(mas->base, mas->cur_xfer_mode);

	if (!spi->slave)
		geni_write_reg(spi_tx_cfg, mas->base, SE_SPI_TRANS_CFG);

	geni_setup_m_cmd(mas->base, m_cmd, m_param);
	SPI_LOG_DBG(mas->ipc, false, mas->dev,
		"%s: trans_len %d xferlen%d tx_cfg 0x%x cmd 0x%x cs%d mode%d freq %d\n",
		__func__, trans_len, xfer->len, spi_tx_cfg, m_cmd, xfer->cs_change,
		mas->cur_xfer_mode, xfer->speed_hz);
	if ((m_cmd & SPI_RX_ONLY) && (mas->cur_xfer_mode == SE_DMA)) {
		ret =  geni_se_rx_dma_prep(mas->wrapper_dev, mas->base,
				xfer->rx_buf, xfer->len, &xfer->rx_dma);
		if (ret || !xfer->rx_buf) {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"Failed to setup Rx dma %d\n", ret);
			xfer->rx_dma = 0;
			return ret;
		}
	}
	if (m_cmd & SPI_TX_ONLY) {
		if (mas->cur_xfer_mode == FIFO_MODE) {
			geni_write_reg(mas->tx_wm, mas->base,
					SE_GENI_TX_WATERMARK_REG);
		} else if (mas->cur_xfer_mode == SE_DMA) {
			ret =  geni_se_tx_dma_prep(mas->wrapper_dev, mas->base,
					(void *)xfer->tx_buf, xfer->len,
							&xfer->tx_dma);
			if (ret || !xfer->tx_buf) {
				SPI_LOG_ERR(mas->ipc, true, mas->dev,
					"Failed to setup tx dma %d\n", ret);
				xfer->tx_dma = 0;
				return ret;
			}
		}
	}

	/* Ensure all writes are done before the WM interrupt */
	mb();
	return ret;
}

static void handle_fifo_timeout(struct spi_master *spi,
					struct spi_transfer *xfer)
{
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	unsigned long timeout;
	u32 rx_fifo_status;
	int rx_wc, i;

	geni_se_dump_dbg_regs(&mas->spi_rsc, mas->base, mas->ipc);

	if (mas->cur_xfer_mode == FIFO_MODE)
		geni_write_reg(0, mas->base, SE_GENI_TX_WATERMARK_REG);
	if (spi->slave)
		goto dma_unprep;

	reinit_completion(&mas->xfer_done);

	/* Dummy read the rx fifo for any spurious data*/
	if (xfer->rx_buf) {
		rx_fifo_status = geni_read_reg(mas->base,
					SE_GENI_RX_FIFO_STATUS);
		rx_wc = (rx_fifo_status & RX_FIFO_WC_MSK);
		for (i = 0; i < rx_wc; i++)
			geni_read_reg(mas->base, SE_GENI_RX_FIFOn);
	}

	geni_cancel_m_cmd(mas->base);

	/* Ensure cmd cancel is written */
	mb();
	timeout = wait_for_completion_timeout(&mas->xfer_done, HZ);
	if (!timeout) {
		reinit_completion(&mas->xfer_done);
		geni_abort_m_cmd(mas->base);
		/* Ensure cmd abort is written */
		mb();
		timeout = wait_for_completion_timeout(&mas->xfer_done,
								HZ);
		if (!timeout)
			dev_err(mas->dev,
				"Failed to cancel/abort m_cmd\n");
	}
dma_unprep:
	geni_spi_dma_unprepare(spi, xfer);
	if (spi->slave && !mas->dis_autosuspend)
		pm_runtime_put_sync_suspend(mas->dev);
}

static int spi_geni_transfer_one(struct spi_master *spi,
				struct spi_device *slv,
				struct spi_transfer *xfer)
{
	int ret = 0;
	struct spi_geni_master *mas = spi_master_get_devdata(spi);
	unsigned long timeout, xfer_timeout;

	if ((xfer->tx_buf == NULL) && (xfer->rx_buf == NULL)) {
		dev_err(mas->dev, "Invalid xfer both tx rx are NULL\n");
		return -EINVAL;
	}

	/* Check for zero length transfer */
	if (xfer->len < 1) {
		dev_err(mas->dev, "Zero length transfer\n");
		return -EINVAL;
	}

	/* Double check PM status, client might have not taken wakelock and
	 * continue to queue more transfers. Post auto-suspend, system suspend
	 * can keep driver to forced suspend, hence it's client's responsibility
	 * to not allow system suspend to trigger.
	 */
	if (pm_runtime_status_suspended(mas->dev)) {
		SPI_LOG_ERR(mas->ipc, true, mas->dev,
			"%s: device is PM suspended\n", __func__);
		return -EACCES;
	}

	xfer_timeout = (1000 * xfer->len * BITS_PER_BYTE) / xfer->speed_hz;
	if (mas->xfer_timeout_offset)
		xfer_timeout += mas->xfer_timeout_offset;
	else
		xfer_timeout += SPI_XFER_TIMEOUT_OFFSET;

	SPI_LOG_ERR(mas->ipc, false, mas->dev,
		    "current xfer_timeout:%lu ms.\n", xfer_timeout);
	xfer_timeout = msecs_to_jiffies(xfer_timeout);

	if (mas->cur_xfer_mode != GSI_DMA) {
		reinit_completion(&mas->xfer_done);
		ret = setup_fifo_xfer(xfer, mas, slv->mode, spi);
		if (ret) {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"setup_fifo_xfer failed: %d\n", ret);
			mas->cur_xfer = NULL;
			goto err_fifo_geni_transfer_one;
		}

		if (spi->slave) {
			mas->slave_state = true;
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				    "%s: slave_state true:%d\n", __func__, mas->slave_state);
		}
		timeout = wait_for_completion_timeout(&mas->xfer_done,
					xfer_timeout);
		if (spi->slave) {
			mas->slave_state = false;
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				    "%s: slave_state false:%d\n", __func__, mas->slave_state);
		}

		if (!timeout) {
			u32 dma_tx_status = geni_read_reg(mas->base, SE_DMA_TX_IRQ_STAT);
			u32 dma_rx_status = geni_read_reg(mas->base, SE_DMA_RX_IRQ_STAT);

			if (((dma_tx_status & TX_DMA_DONE) != TX_DMA_DONE) &&
				((dma_rx_status & RX_DMA_DONE) != RX_DMA_DONE))
				mas->is_dma_not_done = true;

			SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"Xfer[len %d tx %pK rx %pK n %d] timed out.\n",
						xfer->len, xfer->tx_buf,
						xfer->rx_buf,
						xfer->bits_per_word);
			mas->cur_xfer = NULL;
			ret = -ETIMEDOUT;
			goto err_fifo_geni_transfer_one;
		}

		if (mas->is_dma_err) {
			mas->is_dma_err = false;
			mas->cur_xfer = NULL;
			/* handle_fifo_timeout will do dma_unprep */
			goto err_fifo_geni_transfer_one;
		}

		if (mas->cur_xfer_mode == SE_DMA) {
			if (xfer->tx_buf)
				geni_se_tx_dma_unprep(mas->wrapper_dev,
					xfer->tx_dma, xfer->len);
			if (xfer->rx_buf)
				geni_se_rx_dma_unprep(mas->wrapper_dev,
					xfer->rx_dma, xfer->len);
		}
	} else {
		mas->num_tx_eot = 0;
		mas->num_rx_eot = 0;
		mas->num_xfers = 0;
		mas->qn_err = false;
		reinit_completion(&mas->tx_cb);
		reinit_completion(&mas->rx_cb);

		ret = setup_gsi_xfer(xfer, mas, slv, spi);
		if (ret) {
			SPI_LOG_ERR(mas->ipc, true, mas->dev,
				"setup_gsi_xfer failed: %d\n", ret);
			mas->cur_xfer = NULL;
			goto err_gsi_geni_transfer_one;
		}
		if ((mas->num_xfers >= NUM_SPI_XFER) ||
			(list_is_last(&xfer->transfer_list,
					&spi->cur_msg->transfers))) {
			int i;

			for (i = 0 ; i < mas->num_tx_eot; i++) {
				timeout =
				wait_for_completion_timeout(
					&mas->tx_cb, xfer_timeout);
				if (timeout <= 0) {
					SPI_LOG_ERR(mas->ipc, true, mas->dev,
					"Tx[%d] timeout%lu\n", i, timeout);
					ret = -ETIMEDOUT;
					goto err_gsi_geni_transfer_one;
				}
			}
			for (i = 0 ; i < mas->num_rx_eot; i++) {
				timeout =
				wait_for_completion_timeout(
					&mas->rx_cb, xfer_timeout);
				if (timeout <= 0) {
					SPI_LOG_ERR(mas->ipc, true, mas->dev,
					 "Rx[%d] timeout%lu\n", i, timeout);
					ret = -ETIMEDOUT;
					goto err_gsi_geni_transfer_one;
				}
			}
			if (mas->qn_err) {
				ret = -EIO;
				mas->qn_err = false;
				goto err_gsi_geni_transfer_one;
			}
		}
	}
	return ret;
err_gsi_geni_transfer_one:
	geni_se_dump_dbg_regs(&mas->spi_rsc, mas->base, mas->ipc);
	dmaengine_terminate_all(mas->tx);
	if (mas->is_le_vm)
		mas->le_gpi_reset_done = true;
	return ret;
err_fifo_geni_transfer_one:

	handle_fifo_timeout(spi, xfer);
	return ret;
}

static void geni_spi_handle_tx(struct spi_geni_master *mas)
{
	int i = 0;
	int tx_fifo_width = (mas->tx_fifo_width >> 3);
	int max_bytes = 0;
	const u8 *tx_buf = NULL;

	if (!mas->cur_xfer)
		return;

	/*
	 * For non-byte aligned bits-per-word values:
	 * Assumption is that each SPI word will be accomodated in
	 * ceil (bits_per_word / bits_per_byte)
	 * and the next SPI word starts at the next byte.
	 * In such cases, we can fit 1 SPI word per FIFO word so adjust the
	 * max byte that can be sent per IRQ accordingly.
	 */
	if ((mas->tx_fifo_width % mas->cur_word_len))
		max_bytes = (mas->tx_fifo_depth - mas->tx_wm) *
				((mas->cur_word_len / BITS_PER_BYTE) + 1);
	else
		max_bytes = (mas->tx_fifo_depth - mas->tx_wm) * tx_fifo_width;
	tx_buf = mas->cur_xfer->tx_buf;
	tx_buf += (mas->cur_xfer->len - mas->tx_rem_bytes);
	max_bytes = min_t(int, mas->tx_rem_bytes, max_bytes);
	while (i < max_bytes) {
		int j;
		u32 fifo_word = 0;
		u8 *fifo_byte;
		int bytes_per_fifo = tx_fifo_width;
		int bytes_to_write = 0;

		if ((mas->tx_fifo_width % mas->cur_word_len))
			bytes_per_fifo =
				(mas->cur_word_len / BITS_PER_BYTE) + 1;
		bytes_to_write = min_t(int, (max_bytes - i), bytes_per_fifo);
		fifo_byte = (u8 *)&fifo_word;
		for (j = 0; j < bytes_to_write; j++)
			fifo_byte[j] = tx_buf[i++];
		geni_write_reg(fifo_word, mas->base, SE_GENI_TX_FIFOn);
		/* Ensure FIFO writes are written in order */
		mb();
	}
	mas->tx_rem_bytes -= max_bytes;
	if (!mas->tx_rem_bytes) {
		geni_write_reg(0, mas->base, SE_GENI_TX_WATERMARK_REG);
		/* Barrier here before return to prevent further ISRs */
		mb();
	}
}

static void geni_spi_handle_rx(struct spi_geni_master *mas)
{
	int i = 0;
	int fifo_width = (mas->tx_fifo_width >> 3);
	u32 rx_fifo_status = geni_read_reg(mas->base, SE_GENI_RX_FIFO_STATUS);
	int rx_bytes = 0;
	int rx_wc = 0;
	u8 *rx_buf = NULL;

	if (!mas->cur_xfer)
		return;

	rx_buf = mas->cur_xfer->rx_buf;
	rx_wc = (rx_fifo_status & RX_FIFO_WC_MSK);
	if (rx_fifo_status & RX_LAST) {
		int rx_last_byte_valid =
			(rx_fifo_status & RX_LAST_BYTE_VALID_MSK)
					>> RX_LAST_BYTE_VALID_SHFT;
		if (rx_last_byte_valid && (rx_last_byte_valid < 4)) {
			rx_wc -= 1;
			rx_bytes += rx_last_byte_valid;
		}
	}
	if (!(mas->tx_fifo_width % mas->cur_word_len))
		rx_bytes += rx_wc * fifo_width;
	else
		rx_bytes += rx_wc *
			((mas->cur_word_len / BITS_PER_BYTE) + 1);
	rx_bytes = min_t(int, mas->rx_rem_bytes, rx_bytes);
	rx_buf += (mas->cur_xfer->len - mas->rx_rem_bytes);
	while (i < rx_bytes) {
		u32 fifo_word = 0;
		u8 *fifo_byte;
		int bytes_per_fifo = fifo_width;
		int read_bytes = 0;
		int j;

		if ((mas->tx_fifo_width % mas->cur_word_len))
			bytes_per_fifo =
				(mas->cur_word_len / BITS_PER_BYTE) + 1;
		read_bytes = min_t(int, (rx_bytes - i), bytes_per_fifo);
		fifo_word = geni_read_reg(mas->base, SE_GENI_RX_FIFOn);
		fifo_byte = (u8 *)&fifo_word;
		for (j = 0; j < read_bytes; j++)
			rx_buf[i++] = fifo_byte[j];
	}
	mas->rx_rem_bytes -= rx_bytes;
}

static void geni_spi_dma_unprepare(struct spi_master *spi,
					struct spi_transfer *xfer)
{
	unsigned long timeout;
	struct spi_geni_master *mas = spi_master_get_devdata(spi);

	if (mas->cur_xfer_mode == SE_DMA) {
		if (xfer->tx_buf && xfer->tx_dma) {
			reinit_completion(&mas->xfer_done);
			writel_relaxed(1, mas->base + SE_DMA_TX_FSM_RST);
			timeout =
			wait_for_completion_timeout(&mas->xfer_done, HZ);
			if (!timeout)
				dev_err(mas->dev, "DMA TX RESET failed\n");
		}

		if (xfer->rx_buf && xfer->rx_dma) {
			reinit_completion(&mas->xfer_done);
			writel_relaxed(1, mas->base + SE_DMA_RX_FSM_RST);
			timeout =
			wait_for_completion_timeout(&mas->xfer_done, HZ);
			if (!timeout)
				dev_err(mas->dev, "DMA RX RESET failed\n");
		}

		if (spi->slave && mas->is_dma_not_done) {
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s: doing abort for spi slave\n", __func__);
			reinit_completion(&mas->xfer_done);
			geni_abort_m_cmd(mas->base);
			mas->is_dma_not_done = false;
			/* Ensure cmd abort is written */
			mb();
			timeout = wait_for_completion_timeout(&mas->xfer_done, HZ);
			if (!timeout)
				dev_err(mas->dev,
					"Failed to cancel/abort m_cmd\n");
		}

		if (xfer->tx_buf && xfer->tx_dma)
			geni_se_tx_dma_unprep(mas->wrapper_dev,
					xfer->tx_dma, xfer->len);
		if (xfer->rx_buf && xfer->rx_dma)
			geni_se_rx_dma_unprep(mas->wrapper_dev,
				xfer->rx_dma, xfer->len);
	}
}

static void handle_dma_xfer(u32 dma_tx_status, u32 dma_rx_status, struct spi_geni_master *data)
{
	struct spi_geni_master *mas = data;

	if (dma_tx_status) {
		geni_write_reg(dma_tx_status, mas->base, SE_DMA_TX_IRQ_CLR);
		if (dma_tx_status & DMA_TX_ERROR_STATUS) {
			geni_spi_dma_err(mas, dma_tx_status, 0);
			mas->is_dma_err = true;
			mas->cmd_done = true;
			goto exit;
		} else if (dma_tx_status & TX_RESET_DONE) {
			mas->cmd_done = true;
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s: Tx Reset done. DMA_TX_IRQ_STAT:0x%x\n",
				__func__, dma_tx_status);
			goto exit;
		} else if (dma_tx_status & TX_DMA_DONE) {
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s: TX DMA done.\n", __func__);
			mas->tx_rem_bytes = 0;
		}
	}

	if (dma_rx_status) {
		u32 dma_rx_len = geni_read_reg(mas->base, SE_DMA_RX_LEN);
		u32 dma_rx_len_in =  geni_read_reg(mas->base, SE_DMA_RX_LEN_IN);

		geni_write_reg(dma_rx_status, mas->base, SE_DMA_RX_IRQ_CLR);
		if (dma_rx_status & DMA_RX_ERROR_STATUS) {
			geni_spi_dma_err(mas, dma_rx_status, 1);
			mas->is_dma_err = true;
			mas->cmd_done = true;
			goto exit;
		} else if (dma_rx_status & RX_RESET_DONE) {
			mas->cmd_done = true;
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s: Rx Reset done. DMA_RX_IRQ_STAT:0x%x\n",
				__func__, dma_rx_status);
			goto exit;
		} else if (dma_rx_status & RX_DMA_DONE) {
			mas->cmd_done = true;
			GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s: RX DMA done.\n", __func__);
			if (dma_rx_len != dma_rx_len_in) {
				mas->rx_rem_bytes = dma_rx_len - dma_rx_len_in;
				mas->is_dma_err = true;
				GENI_SE_DBG(mas->ipc, false, mas->dev,
				"%s:Data Mismatch, rx_rem:%d, tx_irq_sts:0x%x rx_irq_sts:0x%x\n",
				__func__, mas->rx_rem_bytes, dma_tx_status, dma_rx_status);
				geni_se_dump_dbg_regs(&mas->spi_rsc, mas->base, mas->ipc);
			} else {
				mas->rx_rem_bytes = 0;
			}
		}
	}

	if (!mas->tx_rem_bytes && !mas->rx_rem_bytes)
		mas->cmd_done = true;
exit:
	return;
}

static irqreturn_t geni_spi_irq(int irq, void *data)
{
	struct spi_geni_master *mas = data;
	u32 m_irq = 0;

	if (pm_runtime_status_suspended(mas->dev)) {
		SPI_LOG_DBG(mas->ipc, false, mas->dev,
				"%s: device is suspended\n", __func__);
		goto exit_geni_spi_irq;
	}
	m_irq = geni_read_reg(mas->base, SE_GENI_M_IRQ_STATUS);
	if (mas->cur_xfer_mode == FIFO_MODE) {
		if ((m_irq & M_RX_FIFO_WATERMARK_EN) ||
						(m_irq & M_RX_FIFO_LAST_EN))
			geni_spi_handle_rx(mas);

		if ((m_irq & M_TX_FIFO_WATERMARK_EN))
			geni_spi_handle_tx(mas);

		if ((m_irq & M_CMD_DONE_EN) || (m_irq & M_CMD_CANCEL_EN) ||
			(m_irq & M_CMD_ABORT_EN)) {
			mas->cmd_done = true;
			/*
			 * If this happens, then a CMD_DONE came before all the
			 * buffer bytes were sent out. This is unusual, log this
			 * condition and disable the WM interrupt to prevent the
			 * system from stalling due an interrupt storm.
			 * If this happens when all Rx bytes haven't been
			 * received, log the condition.
			 */
			if (mas->tx_rem_bytes) {
				geni_write_reg(0, mas->base,
						SE_GENI_TX_WATERMARK_REG);
				SPI_LOG_DBG(mas->ipc, false, mas->dev,
					"%s:Premature Done.tx_rem%d bpw%d\n",
					__func__, mas->tx_rem_bytes,
						mas->cur_word_len);
			}
			if (mas->rx_rem_bytes)
				SPI_LOG_DBG(mas->ipc, false, mas->dev,
					"%s:Premature Done.rx_rem%d bpw%d\n",
						__func__, mas->rx_rem_bytes,
							mas->cur_word_len);
		}
	} else if (mas->cur_xfer_mode == SE_DMA) {
		u32 dma_tx_status = geni_read_reg(mas->base,
							SE_DMA_TX_IRQ_STAT);
		u32 dma_rx_status = geni_read_reg(mas->base,
							SE_DMA_RX_IRQ_STAT);

		handle_dma_xfer(dma_tx_status, dma_rx_status, mas);

		if ((m_irq & M_CMD_CANCEL_EN) || (m_irq & M_CMD_ABORT_EN))
			mas->cmd_done = true;

		GENI_SE_DBG(mas->ipc, false, mas->dev,
			    "dma_txirq:0x%x dma_rxirq:0x%x cmd_done=%d\n",
			    dma_tx_status, dma_rx_status, mas->cmd_done);
	}
exit_geni_spi_irq:
	geni_write_reg(m_irq, mas->base, SE_GENI_M_IRQ_CLEAR);
	if (mas->cmd_done) {
		mas->cmd_done = false;
		complete(&mas->xfer_done);
	}
	return IRQ_HANDLED;
}

/**
 * spi_get_dt_property: To read DTSI property.
 * @pdev: structure to platform driver.
 * @geni_mas: structure to spi geni.
 * @spi: structure to spi master.
 * @res: resource details.
 * This function will read SPI DTSI property.
 *
 * return: None.
 */
static void spi_get_dt_property(struct platform_device *pdev,
				struct spi_geni_master *geni_mas,
				struct spi_master *spi,
				struct resource *res)
{
	bool rt_pri;

	rt_pri = of_property_read_bool(pdev->dev.of_node, "qcom,rt");
	if (rt_pri)
		spi->rt = true;
	geni_mas->dis_autosuspend =
		of_property_read_bool(pdev->dev.of_node,
				"qcom,disable-autosuspend");
	/*
	 * shared_se property is set when spi is being used simultaneously
	 * from two Execution Environments.
	 */
	if (of_property_read_bool(pdev->dev.of_node, "qcom,shared_se")) {
		geni_mas->shared_se = true;
		geni_mas->shared_ee = true;
	} else {

		/*
		 * shared_ee property will be set when spi is being used from
		 * dual Execution Environments unlike gsi_mode flag
		 * which is set if SE is in GSI mode.
		 */
		geni_mas->shared_ee =
		of_property_read_bool(pdev->dev.of_node, "qcom,shared_ee");
	}

	geni_mas->set_miso_sampling = of_property_read_bool(pdev->dev.of_node,
				"qcom,set-miso-sampling");
	if (geni_mas->set_miso_sampling) {
		if (!of_property_read_u32(pdev->dev.of_node,
				"qcom,miso-sampling-ctrl-val",
				&geni_mas->miso_sampling_ctrl_val))
			dev_info(&pdev->dev, "MISO_SAMPLING_SET: %d\n",
				geni_mas->miso_sampling_ctrl_val);
	}

	geni_mas->disable_dma = of_property_read_bool(pdev->dev.of_node,
		"qcom,disable-dma");

	if (of_property_read_bool(pdev->dev.of_node, "qcom,master-cross-connect"))
		geni_mas->master_cross_connect = true;

	of_property_read_u32(pdev->dev.of_node, "qcom,xfer-timeout-offset",
			     &geni_mas->xfer_timeout_offset);
	if (geni_mas->xfer_timeout_offset)
		dev_info(&pdev->dev, "%s: DT based xfer timeout offset: %d\n",
			 __func__, geni_mas->xfer_timeout_offset);

}

static int spi_geni_probe(struct platform_device *pdev)
{
	int ret;
	struct spi_master *spi;
	struct spi_geni_master *geni_mas;
	struct se_geni_rsc *rsc;
	struct resource *res;
	struct platform_device *wrapper_pdev;
	struct device_node *wrapper_ph_node;
	bool slave_en;

	slave_en  = of_property_read_bool(pdev->dev.of_node,
					  "qcom,slv-ctrl");

	spi = __spi_alloc_controller(&pdev->dev, sizeof(struct spi_geni_master), slave_en);
	if (!spi) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "Failed to alloc spi struct\n");
		goto spi_geni_probe_err;
	}

	if (slave_en)
		spi->slave_abort = spi_slv_abort;

	platform_set_drvdata(pdev, spi);
	geni_mas = spi_master_get_devdata(spi);
	rsc = &geni_mas->spi_rsc;
	geni_mas->dev = &pdev->dev;
	spi->dev.of_node = pdev->dev.of_node;
	wrapper_ph_node = of_parse_phandle(pdev->dev.of_node,
					"qcom,wrapper-core", 0);
	if (IS_ERR_OR_NULL(wrapper_ph_node)) {
		ret = PTR_ERR(wrapper_ph_node);
		dev_err(&pdev->dev, "No wrapper core defined\n");
		goto spi_geni_probe_err;
	}
	wrapper_pdev = of_find_device_by_node(wrapper_ph_node);
	of_node_put(wrapper_ph_node);
	if (IS_ERR_OR_NULL(wrapper_pdev)) {
		ret = PTR_ERR(wrapper_pdev);
		dev_err(&pdev->dev, "Cannot retrieve wrapper device\n");
		goto spi_geni_probe_err;
	}
	geni_mas->wrapper_dev = &wrapper_pdev->dev;

	if (of_property_read_bool(pdev->dev.of_node, "qcom,le-vm")) {
		geni_mas->is_le_vm = true;
		dev_info(&pdev->dev, "LE-VM usecase\n");
	}

	if (of_property_read_bool(pdev->dev.of_node, "qcom,la-vm")) {
		geni_mas->is_la_vm = true;
		dev_info(&pdev->dev, "LA-VM usecase\n");
	}

	geni_mas->spi_rsc.wrapper_dev = &wrapper_pdev->dev;
	geni_mas->spi_rsc.ctrl_dev = geni_mas->dev;
	/*
	 * For LE, clocks, gpio and icb voting will be provided by
	 * by LA. The SPI operates in GSI mode only for LE usecase,
	 * se irq not required. Below properties will not be present
	 * in SPI LE dt.
	 */
	if (!geni_mas->is_le_vm) {
		ret = geni_se_resources_init(rsc, SPI_CORE2X_VOTE,
					(DEFAULT_SE_CLK * DEFAULT_BUS_WIDTH));
		if (ret) {
			dev_err(&pdev->dev, "Error geni_se_resources_init\n");
			goto spi_geni_probe_err;
		}

		rsc->geni_pinctrl = devm_pinctrl_get(&pdev->dev);
		if (IS_ERR_OR_NULL(rsc->geni_pinctrl)) {
			dev_err(&pdev->dev, "No pinctrl config specified!\n");
			ret = PTR_ERR(rsc->geni_pinctrl);
			goto spi_geni_probe_err;
		}

		rsc->geni_gpio_active = pinctrl_lookup_state(rsc->geni_pinctrl,
							PINCTRL_DEFAULT);
		if (IS_ERR_OR_NULL(rsc->geni_gpio_active)) {
			dev_err(&pdev->dev, "No default config specified!\n");
			ret = PTR_ERR(rsc->geni_gpio_active);
			goto spi_geni_probe_err;
		}

		rsc->geni_gpio_sleep = pinctrl_lookup_state(rsc->geni_pinctrl,
							PINCTRL_SLEEP);
		if (IS_ERR_OR_NULL(rsc->geni_gpio_sleep)) {
			dev_err(&pdev->dev, "No sleep config specified!\n");
			ret = PTR_ERR(rsc->geni_gpio_sleep);
			goto spi_geni_probe_err;
		}

		ret = pinctrl_select_state(rsc->geni_pinctrl,
						rsc->geni_gpio_sleep);
		if (ret) {
			dev_err(&pdev->dev, "Failed to set sleep configuration\n");
			goto spi_geni_probe_err;
		}

		rsc->se_clk = devm_clk_get(&pdev->dev, "se-clk");
		if (IS_ERR(rsc->se_clk)) {
			ret = PTR_ERR(rsc->se_clk);
			dev_err(&pdev->dev,
			"Err getting SE Core clk %d\n", ret);
			goto spi_geni_probe_err;
		}

		rsc->m_ahb_clk = devm_clk_get(&pdev->dev, "m-ahb");
		if (IS_ERR(rsc->m_ahb_clk)) {
			ret = PTR_ERR(rsc->m_ahb_clk);
			dev_err(&pdev->dev, "Err getting M AHB clk %d\n", ret);
			goto spi_geni_probe_err;
		}

		rsc->s_ahb_clk = devm_clk_get(&pdev->dev, "s-ahb");
		if (IS_ERR(rsc->s_ahb_clk)) {
			ret = PTR_ERR(rsc->s_ahb_clk);
			dev_err(&pdev->dev, "Err getting S AHB clk %d\n", ret);
			goto spi_geni_probe_err;
		}

		geni_mas->irq = platform_get_irq(pdev, 0);
		if (geni_mas->irq < 0) {
			dev_err(&pdev->dev, "Err getting IRQ\n");
			ret = geni_mas->irq;
			goto spi_geni_probe_unmap;
		}

		irq_set_status_flags(geni_mas->irq, IRQ_NOAUTOEN);
		ret = devm_request_irq(&pdev->dev, geni_mas->irq,
			geni_spi_irq, IRQF_TRIGGER_HIGH, "spi_geni", geni_mas);
		if (ret) {
			dev_err(&pdev->dev, "Request_irq failed:%d: err:%d\n",
					   geni_mas->irq, ret);
			goto spi_geni_probe_unmap;
		}
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "could not set DMA mask\n");
			goto spi_geni_probe_err;
		}
	}

	if (of_property_read_u32(pdev->dev.of_node, "spi-max-frequency",
				&spi->max_speed_hz)) {
		dev_err(&pdev->dev, "Max frequency not specified.\n");
		ret = -ENXIO;
		goto spi_geni_probe_err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "se_phys");
	if (!res) {
		ret = -ENXIO;
		dev_err(&pdev->dev, "Err getting IO region\n");
		goto spi_geni_probe_err;
	}

	spi_get_dt_property(pdev, geni_mas, spi, res);

	geni_mas->phys_addr = res->start;
	geni_mas->size = resource_size(res);
	geni_mas->base = devm_ioremap(&pdev->dev, res->start,
				      resource_size(res));
	if (!geni_mas->base) {
		ret = -ENOMEM;
		dev_err(&pdev->dev, "Err IO mapping iomem\n");
		goto spi_geni_probe_err;
	}

	geni_mas->spi_rsc.base = geni_mas->base;
	geni_mas->slave_cross_connected =
		of_property_read_bool(pdev->dev.of_node, "slv-cross-connected");
	spi->mode_bits = (SPI_CPOL | SPI_CPHA | SPI_LOOP | SPI_CS_HIGH);
	spi->bits_per_word_mask = SPI_BPW_RANGE_MASK(4, 32);
	spi->num_chipselect = SPI_NUM_CHIPSELECT;
	spi->prepare_transfer_hardware = spi_geni_prepare_transfer_hardware;
	spi->prepare_message = spi_geni_prepare_message;
	spi->unprepare_message = spi_geni_unprepare_message;
	spi->transfer_one = spi_geni_transfer_one;
	spi->unprepare_transfer_hardware
			= spi_geni_unprepare_transfer_hardware;
	spi->auto_runtime_pm = false;

	init_completion(&geni_mas->xfer_done);
	init_completion(&geni_mas->tx_cb);
	init_completion(&geni_mas->rx_cb);
	pm_runtime_set_suspended(&pdev->dev);
	/* for levm skip auto suspend timer */
	if (!geni_mas->is_le_vm && !geni_mas->dis_autosuspend) {
		pm_runtime_set_autosuspend_delay(&pdev->dev,
					SPI_AUTO_SUSPEND_DELAY);
		pm_runtime_use_autosuspend(&pdev->dev);
	}
	pm_runtime_enable(&pdev->dev);

	geni_mas->ipc = ipc_log_context_create(4, dev_name(geni_mas->dev), 0);
	if (!geni_mas->ipc && IS_ENABLED(CONFIG_IPC_LOGGING))
		dev_err(&pdev->dev, "Error creating IPC logs\n");

	ret = spi_register_master(spi);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register SPI master\n");
		goto spi_geni_probe_unmap;
	}

	ret = sysfs_create_file(&(geni_mas->dev->kobj),
			&dev_attr_spi_slave_state.attr);

	dev_info(&pdev->dev, "%s: completed\n", __func__);
	return ret;
spi_geni_probe_unmap:
	devm_iounmap(&pdev->dev, geni_mas->base);
spi_geni_probe_err:
	dev_info(&pdev->dev, "%s: ret:%d\n", __func__, ret);
	spi_master_put(spi);
	return ret;
}

static int spi_geni_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct spi_geni_master *geni_mas = spi_master_get_devdata(master);

	sysfs_remove_file(&pdev->dev.kobj, &dev_attr_spi_slave_state.attr);
	se_geni_resources_off(&geni_mas->spi_rsc);
	spi_unregister_master(master);
	pm_runtime_put_noidle(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	return 0;
}

static int spi_geni_gpi_pause_resume(struct spi_geni_master *geni_mas, bool is_suspend)
{
	int tx_ret = 0;

	if (geni_mas->tx) {
		if (is_suspend)
			tx_ret = dmaengine_pause(geni_mas->tx);
		else
			tx_ret = dmaengine_resume(geni_mas->tx);

		if (tx_ret) {
			SPI_LOG_ERR(geni_mas->ipc, true, geni_mas->dev,
			"%s failed: tx:%d status:%d\n",
			__func__, tx_ret, is_suspend);
			return -EINVAL;
		}
	}
	return 0;
}

#if IS_ENABLED(CONFIG_PM)

static int spi_geni_levm_suspend_proc(struct spi_geni_master *geni_mas, struct spi_master *spi)
{
	int ret = 0;

	spi_geni_unlock_bus(spi);

	if (geni_mas->gsi_mode) {
		ret = spi_geni_gpi_pause_resume(geni_mas, true);
		if (ret) {
			SPI_LOG_DBG(geni_mas->ipc, false, geni_mas->dev, "%s:\n", __func__);
			return ret;
		}
	}
	SPI_LOG_DBG(geni_mas->ipc, false, geni_mas->dev, "%s:\n", __func__);
	return 0;
}

static int spi_geni_runtime_suspend(struct device *dev)
{
	int ret = 0;
	struct spi_master *spi = get_spi_master(dev);
	struct spi_geni_master *geni_mas = spi_master_get_devdata(spi);

	disable_irq(geni_mas->irq);
	if (geni_mas->is_le_vm)
		return spi_geni_levm_suspend_proc(geni_mas, spi);

	SPI_LOG_DBG(geni_mas->ipc, false, geni_mas->dev, "%s:\n", __func__);

	if (geni_mas->gsi_mode) {
		ret = spi_geni_gpi_pause_resume(geni_mas, true);
		if (ret)
			return ret;
	}

	/* For tui usecase LA should control clk/gpio/icb */
	if (geni_mas->is_la_vm)
		goto exit_rt_suspend;

	/* Do not unconfigure the GPIOs for a shared_se usecase */
	if (geni_mas->shared_ee && !geni_mas->shared_se)
		goto exit_rt_suspend;

	if (geni_mas->gsi_mode) {
		ret = se_geni_clks_off(&geni_mas->spi_rsc);
		if (ret)
			SPI_LOG_ERR(geni_mas->ipc, false, geni_mas->dev,
			"%s: Error %d turning off clocks\n", __func__, ret);
		return ret;
	}

exit_rt_suspend:
	ret = se_geni_resources_off(&geni_mas->spi_rsc);
	return ret;
}

static int spi_geni_levm_resume_proc(struct spi_geni_master *geni_mas, struct spi_master *spi)
{
	int ret = 0;

	if (!geni_mas->setup) {
		ret = spi_geni_mas_setup(spi);
		if (ret) {
			SPI_LOG_ERR(geni_mas->ipc, true, geni_mas->dev,
			"%s mas_setup failed: %d\n", __func__, ret);
			return ret;
		}
	}

	if (geni_mas->gsi_mode) {
		ret = spi_geni_gpi_pause_resume(geni_mas, false);
		if (ret) {
			SPI_LOG_ERR(geni_mas->ipc, false, geni_mas->dev, "%s:\n", __func__);
			return ret;
		}
	}

	ret = spi_geni_lock_bus(spi);
	if (ret) {
		SPI_LOG_ERR(geni_mas->ipc, true, geni_mas->dev,
			"%s lock_bus failed: %d\n", __func__, ret);
		return ret;
	}
	SPI_LOG_DBG(geni_mas->ipc, false, geni_mas->dev, "%s:\n", __func__);
	/* Return here as LE VM doesn't need resourc/clock management */
	return ret;
}

static int spi_geni_runtime_resume(struct device *dev)
{
	int ret = 0;
	struct spi_master *spi = get_spi_master(dev);
	struct spi_geni_master *geni_mas = spi_master_get_devdata(spi);

	if (geni_mas->is_le_vm)
		return spi_geni_levm_resume_proc(geni_mas, spi);

	SPI_LOG_DBG(geni_mas->ipc, false, geni_mas->dev, "%s:\n", __func__);

	if (geni_mas->shared_ee || geni_mas->is_la_vm)
		goto exit_rt_resume;

	if (geni_mas->gsi_mode) {
		ret = se_geni_clks_on(&geni_mas->spi_rsc);
		if (ret) {
			SPI_LOG_ERR(geni_mas->ipc, false, geni_mas->dev,
			"%s: Error %d turning on clocks\n", __func__, ret);
			return ret;
		}

		ret = spi_geni_gpi_pause_resume(geni_mas, false);
		return ret;
	}

exit_rt_resume:
	ret = se_geni_resources_on(&geni_mas->spi_rsc);
	if (ret) {
		SPI_LOG_ERR(geni_mas->ipc, false, geni_mas->dev,
		"%s: Error %d turning on clocks\n", __func__, ret);
		return ret;
	}

	if (geni_mas->gsi_mode)
		ret = spi_geni_gpi_pause_resume(geni_mas, false);

	enable_irq(geni_mas->irq);
	return ret;
}

static int spi_geni_resume(struct device *dev)
{
	return 0;
}

static int spi_geni_suspend(struct device *dev)
{
	int ret = 0;

	if (!pm_runtime_status_suspended(dev)) {
		struct spi_master *spi = get_spi_master(dev);
		struct spi_geni_master *geni_mas = spi_master_get_devdata(spi);

		SPI_LOG_ERR(geni_mas->ipc, true, dev,
			    ":%s: runtime PM is active\n", __func__);
		if (list_empty(&spi->queue) && !spi->cur_msg) {
			SPI_LOG_ERR(geni_mas->ipc, true, dev,
					"%s: Force suspend", __func__);
			ret = spi_geni_runtime_suspend(dev);
			if (ret) {
				SPI_LOG_ERR(geni_mas->ipc, true, dev,
					"Force suspend Failed:%d", ret);
			} else {
				pm_runtime_disable(dev);
				pm_runtime_set_suspended(dev);
				pm_runtime_enable(dev);
			}
		} else {
			ret = -EBUSY;
		}
	}
	return ret;
}
#else
static int spi_geni_runtime_suspend(struct device *dev)
{
	return 0;
}

static int spi_geni_runtime_resume(struct device *dev)
{
	return 0;
}

static int spi_geni_resume(struct device *dev)
{
	return 0;
}

static int spi_geni_suspend(struct device *dev)
{
	return 0;
}
#endif

static const struct dev_pm_ops spi_geni_pm_ops = {
	SET_RUNTIME_PM_OPS(spi_geni_runtime_suspend,
					spi_geni_runtime_resume, NULL)
	SET_SYSTEM_SLEEP_PM_OPS(spi_geni_suspend, spi_geni_resume)
};

static const struct of_device_id spi_geni_dt_match[] = {
	{ .compatible = "qcom,spi-geni" },
	{}
};

static struct platform_driver spi_geni_driver = {
	.probe  = spi_geni_probe,
	.remove = spi_geni_remove,
	.driver = {
		.name = "spi_geni",
		.pm = &spi_geni_pm_ops,
		.of_match_table = spi_geni_dt_match,
	},
};

static int __init spi_dev_init(void)
{
	return platform_driver_register(&spi_geni_driver);
}

static void __exit spi_dev_exit(void)
{
	platform_driver_unregister(&spi_geni_driver);
}

module_init(spi_dev_init);
module_exit(spi_dev_exit);

MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:spi_geni");
