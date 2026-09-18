// SPDX-License-Identifier: GPL-2.0
/* Exercise the real CV18xx clock operations against memory-backed registers. */
#include <kunit/test.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "clk-cv18xx-ip.h"

enum {
	MUX0,
	MUX1,
	BYPASS,
	SELECT,
	GATE,
	NUM_REGS,
};

/* C906 parent order: oscillator, TPLL, A0PLL, MIPI/DISP PLL, MPLL, FPLL. */
static const s8 c906_parent2sel[] = { -1, 0, 0, 0, 0, 1 };
static const u8 c906_sel2parent[2][4] = { { 1, 2, 3, 4 }, { 5, 5, 5, 5 } };

struct cv18xx_test_context {
	u32 regs[NUM_REGS];
	spinlock_t lock;
	struct cv1800_clk_mmux mmux;
};

static int cv18xx_test_init(struct kunit *test)
{
	struct cv18xx_test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);
	spin_lock_init(&ctx->lock);
	ctx->mmux = (struct cv1800_clk_mmux) {
		.common = { .base = (void __iomem *)ctx->regs, .lock = &ctx->lock },
		.gate = CV1800_CLK_BIT(GATE * 4, 13),
		.div = {
			CV1800_CLK_REG(MUX0 * 4, 16, 4, 1, CLK_DIVIDER_ONE_BASED),
			CV1800_CLK_REG(MUX1 * 4, 16, 4, 2, CLK_DIVIDER_ONE_BASED),
		},
		.mux = {
			CV1800_CLK_REG(MUX0 * 4, 8, 2, 0, 0),
			CV1800_CLK_REG(MUX1 * 4, 8, 2, 0, 0),
		},
		.bypass = CV1800_CLK_BIT(BYPASS * 4, 6),
		.clk_sel = CV1800_CLK_BIT(SELECT * 4, 23),
		.parent2sel = c906_parent2sel,
		.sel2parent = { c906_sel2parent[0], c906_sel2parent[1] },
	};
	test->priv = ctx;
	return 0;
}

static void cv18xx_seed_regs(struct cv18xx_test_context *ctx, unsigned int seed)
{
	unsigned int i;

	for (i = 0; i < NUM_REGS; i++)
		writel(seed, ctx->mmux.common.base + i * 4);
}

static void cv18xx_mmux_parents(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	const struct clk_ops *ops = &cv1800_clk_mmux_ops;
	struct clk_hw *hw = &ctx->mmux.common.hw;
	u32 before[NUM_REGS], allowed[NUM_REGS];
	unsigned int initial, parent, i;

	/* Begin on either mux lane or bypass, with nonzero neighbouring fields. */
	for (initial = 0; initial < 4; initial++) {
		for (parent = 0; parent < ARRAY_SIZE(c906_parent2sel); parent++) {
			cv18xx_seed_regs(ctx, 0x5a5a5a5a);
			writel((readl(&ctx->regs[SELECT]) & ~BIT(23)) |
			       (initial & 1 ? BIT(23) : 0), &ctx->regs[SELECT]);
			writel((readl(&ctx->regs[BYPASS]) & ~BIT(6)) |
			       (initial & 2 ? BIT(6) : 0), &ctx->regs[BYPASS]);
			memcpy(before, ctx->regs, sizeof(before));
			memset(allowed, 0, sizeof(allowed));
			allowed[BYPASS] = BIT(6);
			if (parent) {
				allowed[SELECT] = BIT(23);
				allowed[c906_parent2sel[parent]] = GENMASK(9, 8);
			}
			KUNIT_ASSERT_EQ(test, ops->set_parent(hw, parent), 0);
			KUNIT_EXPECT_EQ_MSG(test, ops->get_parent(hw), parent,
					    "initial=%u parent=%u", initial, parent);
			for (i = 0; i < NUM_REGS; i++)
				KUNIT_EXPECT_EQ_MSG(test,
					readl(&ctx->regs[i]) & ~allowed[i],
					before[i] & ~allowed[i],
					"register=%u parent=%u", i, parent);
		}
	}
}

static void cv18xx_mmux_missing_selector(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	static const u8 missing_mpll[] = { 1, 2, 3, 1 };
	u32 before[NUM_REGS];

	cv18xx_seed_regs(ctx, 0x5a5a5a5a);
	ctx->mmux.sel2parent[0] = missing_mpll;
	memcpy(before, ctx->regs, sizeof(before));
	KUNIT_EXPECT_EQ(test,
		cv1800_clk_mmux_ops.set_parent(&ctx->mmux.common.hw, 4), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, before, ctx->regs, sizeof(before));
}

static void cv18xx_mmux_rates(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	const struct clk_ops *ops = &cv1800_clk_mmux_ops;
	struct clk_hw *hw = &ctx->mmux.common.hw;
	u32 before[NUM_REGS];
	unsigned int lane;

	for (lane = 0; lane < 2; lane++) {
		cv18xx_seed_regs(ctx, 0);
		writel(lane ? 0 : BIT(23), &ctx->regs[SELECT]);
		KUNIT_ASSERT_EQ(test, ops->set_rate(hw, 425000000, 850000000), 0);
		KUNIT_EXPECT_EQ(test, ops->recalc_rate(hw, 850000000), 425000000UL);
		KUNIT_EXPECT_EQ(test, readl(&ctx->regs[1 - lane]), 0U);
	}

	/* A bypassed divider must report success without touching any register. */
	cv18xx_seed_regs(ctx, 0x5a5a5a5a);
	writel(BIT(6), &ctx->regs[BYPASS]);
	memcpy(before, ctx->regs, sizeof(before));
	KUNIT_EXPECT_EQ(test, ops->set_rate(hw, 25000000, 25000000), 0);
	KUNIT_EXPECT_EQ(test, ops->recalc_rate(hw, 25000000), 25000000UL);
	KUNIT_EXPECT_MEMEQ(test, before, ctx->regs, sizeof(before));
}

static void cv18xx_bypass_mux_parents(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	struct cv1800_clk_bypass_mux mux = {
		.mux = {
			.common = ctx->mmux.common,
			.mux = CV1800_CLK_REG(MUX0 * 4, 8, 2, 0, 0),
		},
		.bypass = CV1800_CLK_BIT(BYPASS * 4, 6),
	};
	const struct clk_ops *ops = &cv1800_clk_bypass_mux_ops;
	unsigned int initial, parent;

	for (initial = 0; initial < 5; initial++) {
		for (parent = 0; parent < 5; parent++) {
			cv18xx_seed_regs(ctx, 0);
			KUNIT_ASSERT_EQ(test, ops->set_parent(&mux.mux.common.hw, initial), 0);
			KUNIT_ASSERT_EQ(test, ops->set_parent(&mux.mux.common.hw, parent), 0);
			KUNIT_EXPECT_EQ(test, ops->get_parent(&mux.mux.common.hw), parent);
		}
	}
}

static struct kunit_case cv18xx_clock_cases[] = {
	KUNIT_CASE(cv18xx_mmux_parents),
	KUNIT_CASE(cv18xx_mmux_missing_selector),
	KUNIT_CASE(cv18xx_mmux_rates),
	KUNIT_CASE(cv18xx_bypass_mux_parents),
	{}
};

static struct kunit_suite cv18xx_clock_suite = {
	.name = "cv18xx-clock",
	.init = cv18xx_test_init,
	.test_cases = cv18xx_clock_cases,
};
kunit_test_suite(cv18xx_clock_suite);

MODULE_LICENSE("GPL");
