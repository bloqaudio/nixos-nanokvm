// SPDX-License-Identifier: GPL-2.0
/* Exercise the real CV18xx clock operations against memory-backed registers. */
#include <kunit/test.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/spinlock.h>

#include "clk-cv18xx-ip.h"
#include "clk-cv18xx-pll.h"

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
	struct clk_hw *parents[6];
};

static void cv18xx_unregister_fixed(void *hw)
{
	clk_disable_unprepare(((struct clk_hw *)hw)->clk);
	clk_hw_unregister_fixed_rate(hw);
}

static void cv18xx_unregister(void *hw)
{
	clk_hw_unregister(hw);
}

static int cv18xx_test_init(struct kunit *test)
{
	struct cv18xx_test_context *ctx;
	static const unsigned long rates[] = {
		25000000, 1400000000, 442368000, 900000000, 850000000, 1500000000,
	};
	struct clk_init_data init = {
		.name = "cv18xx-test-mmux",
		.ops = &cv1800_clk_mmux_ops,
		.num_parents = ARRAY_SIZE(rates),
		.flags = CLK_SET_RATE_NO_REPARENT | CLK_GET_RATE_NOCACHE,
	};
	unsigned int i;

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
	for (i = 0; i < ARRAY_SIZE(rates); i++) {
		char name[32];

		snprintf(name, sizeof(name), "cv18xx-test-parent-%u", i);
		ctx->parents[i] = clk_hw_register_fixed_rate(NULL, name, NULL, 0, rates[i]);
		KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ctx->parents[i]);
		KUNIT_ASSERT_EQ(test, clk_prepare_enable(ctx->parents[i]->clk), 0);
		kunit_add_action_or_reset(test, cv18xx_unregister_fixed, ctx->parents[i]);
	}
	/* The normal firmware configuration: MPLL / 1, lane zero. */
	ctx->regs[MUX0] = BIT(16) | (3 << 8) | BIT(3);
	ctx->regs[SELECT] = BIT(23);
	init.parent_hws = (const struct clk_hw **)ctx->parents;
	ctx->mmux.common.hw.init = &init;
	KUNIT_ASSERT_EQ(test, clk_hw_register(NULL, &ctx->mmux.common.hw), 0);
	kunit_add_action_or_reset(test, cv18xx_unregister, &ctx->mmux.common.hw);
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

static void cv18xx_mmux_cpufreq(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	struct clk *clk = ctx->mmux.common.hw.clk;
	static const unsigned long rates[] = { 425000000, 850000000, 212500000 };
	unsigned int i;

	for (i = 0; i < 96; i++) {
		unsigned long rate = rates[i % ARRAY_SIZE(rates)];

		KUNIT_EXPECT_EQ(test, clk_round_rate(clk, rate), (long)rate);
		KUNIT_ASSERT_EQ(test, clk_set_rate(clk, rate), 0);
		KUNIT_EXPECT_EQ(test, clk_get_rate(clk), rate);
		KUNIT_EXPECT_PTR_EQ(test, clk_hw_get_parent(&ctx->mmux.common.hw),
				   ctx->parents[4]);
		KUNIT_EXPECT_EQ(test, cv1800_clk_mmux_ops.get_parent(&ctx->mmux.common.hw), 4);
		KUNIT_EXPECT_EQ(test, ctx->regs[MUX1], 0U);
		KUNIT_EXPECT_EQ(test, ctx->regs[SELECT], BIT(23));
		KUNIT_EXPECT_EQ(test, ctx->regs[BYPASS], 0U);
		KUNIT_EXPECT_EQ(test, clk_hw_get_rate(ctx->parents[4]), 850000000UL);
	}
}

static void cv18xx_mmux_unsafe_temporary_rate(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	u32 before[NUM_REGS];

	memcpy(before, ctx->regs, sizeof(before));
	/* Even FPLL / 15 exceeds this endpoint. Reject before any MMIO write. */
	KUNIT_EXPECT_EQ(test, cv1800_clk_mmux_ops.set_rate(&ctx->mmux.common.hw,
						       85000000, 850000000), -ERANGE);
	KUNIT_EXPECT_MEMEQ(test, before, ctx->regs, sizeof(before));
	KUNIT_EXPECT_EQ(test, cv1800_clk_mmux_ops.set_rate(&ctx->mmux.common.hw,
						       0, 850000000), -EINVAL);
	KUNIT_EXPECT_MEMEQ(test, before, ctx->regs, sizeof(before));
}

static void cv18xx_pll_lock_status(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	unsigned int bit;

	/* G2 uses indices 0..4; G6 uses 0..2. Other PLLs may be updating. */
	for (bit = 0; bit < 5; bit++) {
		writel(BIT(bit + 16) | (GENMASK(4, 0) & ~BIT(bit)),
		       &ctx->regs[GATE]);
		KUNIT_EXPECT_EQ(test, cv1800_clk_wait_for_lock(&ctx->mmux.common,
							     GATE * 4, BIT(bit)), 0);
	}

	/* Updating, unlocked, or locked but still updating must not succeed. */
	writel(BIT(0), &ctx->regs[GATE]);
	KUNIT_EXPECT_EQ(test, cv1800_clk_wait_for_lock(&ctx->mmux.common,
						     GATE * 4, BIT(0)), -ETIMEDOUT);
	writel(0, &ctx->regs[GATE]);
	KUNIT_EXPECT_EQ(test, cv1800_clk_wait_for_lock(&ctx->mmux.common,
						     GATE * 4, BIT(0)), -ETIMEDOUT);
	writel(BIT(16) | BIT(0), &ctx->regs[GATE]);
	KUNIT_EXPECT_EQ(test, cv1800_clk_wait_for_lock(&ctx->mmux.common,
						     GATE * 4, BIT(0)), -ETIMEDOUT);
}

static void cv18xx_ipll_rates(struct kunit *test)
{
	struct cv18xx_test_context *ctx = test->priv;
	static const struct cv1800_clk_pll_limit limits = {
		.pre_div = _CV1800_PLL_LIMIT(1, 127),
		.div = _CV1800_PLL_LIMIT(6, 127),
		.post_div = _CV1800_PLL_LIMIT(1, 127),
		.ictrl = _CV1800_PLL_LIMIT(0, 7),
		.mode = _CV1800_PLL_LIMIT(0, 3),
	};
	struct cv1800_clk_pll pll = {
		.common = { .base = (void __iomem *)ctx->regs, .lock = &ctx->lock },
		.pll_reg = MUX0 * 4,
		.pll_status = CV1800_CLK_BIT(GATE * 4, 0),
		.pll_limit = &limits,
	};
	const struct clk_ops *ops = &cv1800_clk_ipll_ops;
	u32 before;

	writel(BIT(16), &ctx->regs[GATE]);
	writel(BIT(31) | BIT(7), &ctx->regs[MUX0]);
	KUNIT_ASSERT_EQ(test, ops->set_rate(&pll.common.hw, 1000000000, 25000000), 0);
	KUNIT_EXPECT_EQ(test, ops->recalc_rate(&pll.common.hw, 25000000), 1000000000UL);
	KUNIT_EXPECT_EQ(test, readl(&ctx->regs[MUX0]) & ~_PLL_ALL_FIELD_MASK,
			BIT(31) | BIT(7));

	before = readl(&ctx->regs[MUX0]);
	KUNIT_EXPECT_EQ(test, ops->set_rate(&pll.common.hw, 0, 25000000), -EINVAL);
	KUNIT_EXPECT_EQ(test, readl(&ctx->regs[MUX0]), before);
	KUNIT_EXPECT_EQ(test, ops->set_rate(&pll.common.hw, 1, 25000000), -EINVAL);
	KUNIT_EXPECT_EQ(test, readl(&ctx->regs[MUX0]), before);

	writel(BIT(0), &ctx->regs[GATE]);
	KUNIT_EXPECT_EQ(test, ops->set_rate(&pll.common.hw, 850000000, 25000000),
			-ETIMEDOUT);
}

static struct kunit_case cv18xx_clock_cases[] = {
	KUNIT_CASE(cv18xx_mmux_parents),
	KUNIT_CASE(cv18xx_mmux_missing_selector),
	KUNIT_CASE(cv18xx_mmux_rates),
	KUNIT_CASE(cv18xx_bypass_mux_parents),
	KUNIT_CASE(cv18xx_mmux_cpufreq),
	KUNIT_CASE(cv18xx_mmux_unsafe_temporary_rate),
	KUNIT_CASE(cv18xx_pll_lock_status),
	KUNIT_CASE(cv18xx_ipll_rates),
	{}
};

static struct kunit_suite cv18xx_clock_suite = {
	.name = "cv18xx-clock",
	.init = cv18xx_test_init,
	.test_cases = cv18xx_clock_cases,
};
kunit_test_suite(cv18xx_clock_suite);

MODULE_LICENSE("GPL");
