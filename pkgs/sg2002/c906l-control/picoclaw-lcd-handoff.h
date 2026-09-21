/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SG2002_C906L_PICOCLAW_LCD_HANDOFF_H
#define SG2002_C906L_PICOCLAW_LCD_HANDOFF_H

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/reset.h>

#define SG2002_PICOCLAW_EPHY_BASE	0x03009000U
#define SG2002_PICOCLAW_EPHY_SIZE	0x1000U
#define SG2002_PICOCLAW_PINMUX_BASE	0x03001000U
#define SG2002_PICOCLAW_PINMUX_SIZE	0x1000U

struct sg2002_picoclaw_lcd_handoff {
	bool required;
	bool prepared;
	struct clk *spi_clk;
	struct clk *pclk;
	struct reset_control *spi_reset;
	struct reset_control *gpio_reset;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins;
	void __iomem *ephy;
	void __iomem *pinmux;
};

static void sg2002_picoclaw_disable_clock(void *data)
{
	clk_disable_unprepare(data);
}

static int sg2002_picoclaw_enable_clock(struct device *dev, struct clk *clk)
{
	int ret;

	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, sg2002_picoclaw_disable_clock, clk);
}

static void sg2002_picoclaw_release_rate(void *data)
{
	clk_rate_exclusive_put(data);
}

static int sg2002_picoclaw_hold_rate(struct device *dev, struct clk *clk)
{
	int ret;

	ret = clk_rate_exclusive_get(clk);
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, sg2002_picoclaw_release_rate, clk);
}

static int sg2002_picoclaw_expect_rate(struct device *dev, struct clk *clk,
				       const char *property)
{
	u32 expected;
	unsigned long actual;

	if (of_property_read_u32(dev->of_node, property, &expected))
		return dev_err_probe(dev, -EINVAL, "missing %s\n", property);
	actual = clk_get_rate(clk);
	if (actual != expected)
		return dev_err_probe(dev, -EINVAL,
			"%s mismatch: expected %u Hz, got %lu Hz\n",
			property, expected, actual);
	return 0;
}

static int sg2002_picoclaw_update(struct device *dev, void __iomem *ephy,
				  u32 offset, u32 mask, u32 expected)
{
	u32 value = readl(ephy + offset);

	value = (value & ~mask) | expected;
	writel(value, ephy + offset);
	if ((readl(ephy + offset) & mask) != expected)
		return dev_err_probe(dev, -EIO,
			"PicoClaw handoff readback failed at EPHY+0x%x\n",
			offset);
	return 0;
}

static int sg2002_picoclaw_write_verify(struct device *dev,
					void __iomem *ephy, u32 offset,
					u32 value, u32 verify_mask)
{
	/* The vendor sequence replaces the complete register, while the SG2002
	 * pinout only defines the configuration fields selected by verify_mask.
	 * Preserve that write and do not mistake undocumented readback bits for
	 * part of the Linux-to-C906L handoff contract. */
	writel(value, ephy + offset);
	if ((readl(ephy + offset) & verify_mask) != (value & verify_mask))
		return dev_err_probe(dev, -EIO,
			"PicoClaw handoff readback failed at EPHY+0x%x\n",
			offset);
	return 0;
}

static int sg2002_picoclaw_expect(struct device *dev, void __iomem *registers,
				  u32 offset, u32 mask, u32 expected)
{
	if ((readl(registers + offset) & mask) != expected)
		return dev_err_probe(dev, -EIO,
			"PicoClaw pinmux readback failed at SYS+0x%x\n", offset);
	return 0;
}

/*
 * Prepare shared board state only after the caller has validated the exact
 * immutable firmware manifest.  SPI1/GPIOA themselves remain C906L leases;
 * this Linux driver retains only their clocks, resets, pinctrl state and the
 * Ethernet-pad route for the lifetime of the bound control endpoint.
 */
static int sg2002_picoclaw_lcd_prepare(struct device *dev,
				       struct sg2002_picoclaw_lcd_handoff *lcd)
{
	u32 resource[2];
	int ret;

	lcd->required = of_property_read_bool(dev->of_node,
					      "sophgo,picoclaw-lcd-handoff");
	if (!lcd->required)
		return 0;

	lcd->spi_clk = devm_clk_get(dev, "spi");
	if (IS_ERR(lcd->spi_clk))
		return dev_err_probe(dev, PTR_ERR(lcd->spi_clk),
				     "failed to acquire SPI source clock\n");
	lcd->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(lcd->pclk))
		return dev_err_probe(dev, PTR_ERR(lcd->pclk),
				     "failed to acquire SPI APB clock\n");
	lcd->spi_reset = devm_reset_control_get_exclusive(dev, "spi");
	if (IS_ERR(lcd->spi_reset))
		return dev_err_probe(dev, PTR_ERR(lcd->spi_reset),
				     "failed to acquire SPI1 reset\n");
	lcd->gpio_reset = devm_reset_control_get_exclusive(dev, "gpio");
	if (IS_ERR(lcd->gpio_reset))
		return dev_err_probe(dev, PTR_ERR(lcd->gpio_reset),
				     "failed to acquire GPIOA reset\n");
	lcd->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(lcd->pinctrl))
		return dev_err_probe(dev, PTR_ERR(lcd->pinctrl),
				     "failed to acquire PicoClaw pinctrl\n");
	lcd->pins = pinctrl_lookup_state(lcd->pinctrl,
					 "picoclaw-lcd-handoff");
	if (IS_ERR(lcd->pins))
		return dev_err_probe(dev, PTR_ERR(lcd->pins),
				     "missing PicoClaw LCD handoff pin state\n");

	if (of_property_read_u32_array(dev->of_node,
				       "sophgo,picoclaw-ephy-reg", resource, 2) ||
	    resource[0] != SG2002_PICOCLAW_EPHY_BASE ||
	    resource[1] != SG2002_PICOCLAW_EPHY_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "invalid PicoClaw EPHY aperture\n");
	lcd->ephy = devm_ioremap(dev, resource[0], resource[1]);
	if (!lcd->ephy)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map PicoClaw EPHY aperture\n");
	if (of_property_read_u32_array(dev->of_node,
				       "sophgo,picoclaw-pinmux-reg", resource, 2) ||
	    resource[0] != SG2002_PICOCLAW_PINMUX_BASE ||
	    resource[1] != SG2002_PICOCLAW_PINMUX_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "invalid PicoClaw pinmux aperture\n");
	lcd->pinmux = devm_ioremap(dev, resource[0], resource[1]);
	if (!lcd->pinmux)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map PicoClaw pinmux aperture\n");

	ret = sg2002_picoclaw_enable_clock(dev, lcd->spi_clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable SPI source clock\n");
	ret = sg2002_picoclaw_enable_clock(dev, lcd->pclk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable SPI APB clock\n");
	ret = sg2002_picoclaw_hold_rate(dev, lcd->spi_clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to hold SPI source rate\n");
	ret = sg2002_picoclaw_hold_rate(dev, lcd->pclk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to hold SPI APB rate\n");
	ret = sg2002_picoclaw_expect_rate(dev, lcd->spi_clk,
					  "sophgo,spi-clock-hz");
	if (ret)
		return ret;
	ret = sg2002_picoclaw_expect_rate(dev, lcd->pclk, "sophgo,pclk-hz");
	if (ret)
		return ret;
	ret = reset_control_deassert(lcd->spi_reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert SPI1 reset\n");
	ret = reset_control_deassert(lcd->gpio_reset);
	if (ret)
		return dev_err_probe(dev, ret, "failed to deassert GPIOA reset\n");

	/* Proven PicoClaw Ethernet-pad-to-SPI1 route.  Preserve unrelated bits
	 * and verify each write before the C906L can be activated. */
	ret = sg2002_picoclaw_update(dev, lcd->ephy, 0x804, 0x1, 0x1);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_update(dev, lcd->ephy, 0x808, 0x1f, 0x1);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_update(dev, lcd->ephy, 0x800, 0x4, 0x4);
	if (ret)
		return ret;
	usleep_range(1000, 1200);
	ret = sg2002_picoclaw_update(dev, lcd->ephy, 0x07c, 0x1f00, 0x500);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_update(dev, lcd->ephy, 0x078, 0xfff, 0xf00);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_write_verify(dev, lcd->ephy, 0x074,
					   0x606, 0x606);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_write_verify(dev, lcd->ephy, 0x070,
					   0x606, 0x606);
	if (ret)
		return ret;

	ret = pinctrl_select_state(lcd->pinctrl, lcd->pins);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to select PicoClaw LCD pin state\n");
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x124, 0x7, 0x6);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x128, 0x7, 0x6);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x12c, 0x7, 0x6);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x130, 0x7, 0x6);
	if (ret)
		return ret;
	/* SYS+0x064 (JTAG_CPU_TMS) is deliberately absent: that pad carries
	 * PWM_7 for the Linux backlight, not XGPIOA_19, so it is outside this
	 * lease and pwm-backlight may not have muxed it yet when we probe. */
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x058, 0x7, 0x3);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x070, 0x7, 0x3);
	if (ret)
		return ret;
	ret = sg2002_picoclaw_expect(dev, lcd->pinmux, 0x04c, 0x7, 0x3);
	if (ret)
		return ret;
	lcd->prepared = true;
	dev_info(dev, "prepared and retained PicoClaw LCD clock/pad handoff\n");
	return 0;
}

#endif
