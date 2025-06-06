// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Rockchip SoC DP (Display Port) interface driver.
 *
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Andy Yan <andy.yan@rock-chips.com>
 *         Yakir Yang <ykk@rock-chips.com>
 *         Jeff Chen <jeff.chen@rock-chips.com>
 */

#include <linux/component.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/clk.h>

#include <video/of_videomode.h>
#include <video/videomode.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/bridge/analogix_dp.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "rockchip_drm_drv.h"

#define PSR_WAIT_LINE_FLAG_TIMEOUT_MS	100

#define GRF_REG_FIELD(_reg, _lsb, _msb) {	\
				.reg = _reg,	\
				.lsb = _lsb,	\
				.msb = _msb,	\
				.valid = true,	\
				}

struct rockchip_grf_reg_field {
	u32 reg;
	u32 lsb;
	u32 msb;
	bool valid;
};

/**
 * struct rockchip_dp_chip_data - splite the grf setting of kind of chips
 * @lcdc_sel: grf register field of lcdc_sel
 * @chip_type: specific chip type
 * @reg: register base address
 */
struct rockchip_dp_chip_data {
	const struct rockchip_grf_reg_field lcdc_sel;
	u32	chip_type;
	u32	reg;
};

struct rockchip_dp_device {
	struct drm_device        *drm_dev;
	struct device            *dev;
	struct rockchip_encoder  encoder;
	struct drm_display_mode  mode;

	struct clk               *pclk;
	struct clk               *grfclk;
	struct regmap            *grf;
	struct reset_control     *rst;

	const struct rockchip_dp_chip_data *data;

	struct analogix_dp_device *adp;
	struct analogix_dp_plat_data plat_data;
};

static struct rockchip_dp_device *encoder_to_dp(struct drm_encoder *encoder)
{
	struct rockchip_encoder *rkencoder = to_rockchip_encoder(encoder);

	return container_of(rkencoder, struct rockchip_dp_device, encoder);
}

static struct rockchip_dp_device *pdata_encoder_to_dp(struct analogix_dp_plat_data *plat_data)
{
	return container_of(plat_data, struct rockchip_dp_device, plat_data);
}

static int rockchip_grf_write(struct regmap *grf, u32 reg, u32 mask, u32 val)
{
	return regmap_write(grf, reg, (mask << 16) | (val & mask));
}

static int rockchip_grf_field_write(struct regmap *grf,
				    const struct rockchip_grf_reg_field *field,
				    u32 val)
{
	u32 mask;

	if (!field->valid)
		return 0;

	mask = GENMASK(field->msb, field->lsb);
	val <<= field->lsb;

	return rockchip_grf_write(grf, field->reg, mask, val);
}

static int rockchip_dp_pre_init(struct rockchip_dp_device *dp)
{
	reset_control_assert(dp->rst);
	usleep_range(10, 20);
	reset_control_deassert(dp->rst);

	return 0;
}

static int rockchip_dp_poweron(struct analogix_dp_plat_data *plat_data)
{
	struct rockchip_dp_device *dp = pdata_encoder_to_dp(plat_data);
	int ret;

	ret = clk_prepare_enable(dp->pclk);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "failed to enable pclk %d\n", ret);
		return ret;
	}

	ret = rockchip_dp_pre_init(dp);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "failed to dp pre init %d\n", ret);
		clk_disable_unprepare(dp->pclk);
		return ret;
	}

	return ret;
}

static int rockchip_dp_powerdown(struct analogix_dp_plat_data *plat_data)
{
	struct rockchip_dp_device *dp = pdata_encoder_to_dp(plat_data);

	clk_disable_unprepare(dp->pclk);

	return 0;
}

static int rockchip_dp_get_modes(struct analogix_dp_plat_data *plat_data,
				 struct drm_connector *connector)
{
	struct drm_display_info *di = &connector->display_info;
	/* VOP couldn't output YUV video format for eDP rightly */
	u32 mask = DRM_COLOR_FORMAT_YCBCR444 | DRM_COLOR_FORMAT_YCBCR422;

	if ((di->color_formats & mask)) {
		DRM_DEBUG_KMS("Swapping display color format from YUV to RGB\n");
		di->color_formats &= ~mask;
		di->color_formats |= DRM_COLOR_FORMAT_RGB444;
		di->bpc = 8;
	}

	return 0;
}

static bool
rockchip_dp_drm_encoder_mode_fixup(struct drm_encoder *encoder,
				   const struct drm_display_mode *mode,
				   struct drm_display_mode *adjusted_mode)
{
	/* do nothing */
	return true;
}

static void rockchip_dp_drm_encoder_mode_set(struct drm_encoder *encoder,
					     struct drm_display_mode *mode,
					     struct drm_display_mode *adjusted)
{
	/* do nothing */
}

static
struct drm_crtc *rockchip_dp_drm_get_new_crtc(struct drm_encoder *encoder,
					      struct drm_atomic_state *state)
{
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;

	connector = drm_atomic_get_new_connector_for_encoder(state, encoder);
	if (!connector)
		return NULL;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (!conn_state)
		return NULL;

	return conn_state->crtc;
}

static void rockchip_dp_drm_encoder_enable(struct drm_encoder *encoder,
					   struct drm_atomic_state *state)
{
	struct rockchip_dp_device *dp = encoder_to_dp(encoder);
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int ret;

	crtc = rockchip_dp_drm_get_new_crtc(encoder, state);
	if (!crtc)
		return;

	old_crtc_state = drm_atomic_get_old_crtc_state(state, crtc);
	/* Coming back from self refresh, nothing to do */
	if (old_crtc_state && old_crtc_state->self_refresh_active)
		return;

	ret = clk_prepare_enable(dp->grfclk);
	if (ret < 0) {
		DRM_DEV_ERROR(dp->dev, "failed to enable grfclk %d\n", ret);
		return;
	}

	ret = drm_of_encoder_active_endpoint_id(dp->dev->of_node, encoder);
	if (ret < 0)
		return;

	DRM_DEV_DEBUG(dp->dev, "vop %s output to dp\n", (ret) ? "LIT" : "BIG");

	ret = rockchip_grf_field_write(dp->grf, &dp->data->lcdc_sel, ret);
	if (ret != 0)
		DRM_DEV_ERROR(dp->dev, "Could not write to GRF: %d\n", ret);

	clk_disable_unprepare(dp->grfclk);
}

static void rockchip_dp_drm_encoder_disable(struct drm_encoder *encoder,
					    struct drm_atomic_state *state)
{
	struct rockchip_dp_device *dp = encoder_to_dp(encoder);
	struct drm_crtc *crtc;
	struct drm_crtc_state *new_crtc_state = NULL;
	int ret;

	crtc = rockchip_dp_drm_get_new_crtc(encoder, state);
	/* No crtc means we're doing a full shutdown */
	if (!crtc)
		return;

	new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	/* If we're not entering self-refresh, no need to wait for vact */
	if (!new_crtc_state || !new_crtc_state->self_refresh_active)
		return;

	ret = rockchip_drm_wait_vact_end(crtc, PSR_WAIT_LINE_FLAG_TIMEOUT_MS);
	if (ret)
		DRM_DEV_ERROR(dp->dev, "line flag irq timed out\n");
}

static int
rockchip_dp_drm_encoder_atomic_check(struct drm_encoder *encoder,
				      struct drm_crtc_state *crtc_state,
				      struct drm_connector_state *conn_state)
{
	struct rockchip_crtc_state *s = to_rockchip_crtc_state(crtc_state);
	struct drm_display_info *di = &conn_state->connector->display_info;

	/*
	 * The hardware IC designed that VOP must output the RGB10 video
	 * format to eDP controller, and if eDP panel only support RGB8,
	 * then eDP controller should cut down the video data, not via VOP
	 * controller, that's why we need to hardcode the VOP output mode
	 * to RGA10 here.
	 */

	s->output_mode = ROCKCHIP_OUT_MODE_AAAA;
	s->output_type = DRM_MODE_CONNECTOR_eDP;
	s->output_bpc = di->bpc;

	return 0;
}

static const struct drm_encoder_helper_funcs rockchip_dp_encoder_helper_funcs = {
	.mode_fixup = rockchip_dp_drm_encoder_mode_fixup,
	.mode_set = rockchip_dp_drm_encoder_mode_set,
	.atomic_enable = rockchip_dp_drm_encoder_enable,
	.atomic_disable = rockchip_dp_drm_encoder_disable,
	.atomic_check = rockchip_dp_drm_encoder_atomic_check,
};

static int rockchip_dp_of_probe(struct rockchip_dp_device *dp)
{
	struct device *dev = dp->dev;
	struct device_node *np = dev->of_node;

	dp->grf = syscon_regmap_lookup_by_phandle(np, "rockchip,grf");
	if (IS_ERR(dp->grf)) {
		DRM_DEV_ERROR(dev, "failed to get rockchip,grf property\n");
		return PTR_ERR(dp->grf);
	}

	dp->grfclk = devm_clk_get(dev, "grf");
	if (PTR_ERR(dp->grfclk) == -ENOENT) {
		dp->grfclk = NULL;
	} else if (PTR_ERR(dp->grfclk) == -EPROBE_DEFER) {
		return -EPROBE_DEFER;
	} else if (IS_ERR(dp->grfclk)) {
		DRM_DEV_ERROR(dev, "failed to get grf clock\n");
		return PTR_ERR(dp->grfclk);
	}

	dp->pclk = devm_clk_get(dev, "pclk");
	if (IS_ERR(dp->pclk)) {
		DRM_DEV_ERROR(dev, "failed to get pclk property\n");
		return PTR_ERR(dp->pclk);
	}

	dp->rst = devm_reset_control_get(dev, "dp");
	if (IS_ERR(dp->rst)) {
		DRM_DEV_ERROR(dev, "failed to get dp reset control\n");
		return PTR_ERR(dp->rst);
	}

	return 0;
}

static int rockchip_dp_drm_create_encoder(struct rockchip_dp_device *dp)
{
	struct drm_encoder *encoder = &dp->encoder.encoder;
	struct drm_device *drm_dev = dp->drm_dev;
	struct device *dev = dp->dev;
	int ret;

	encoder->possible_crtcs = drm_of_find_possible_crtcs(drm_dev,
							     dev->of_node);
	DRM_DEBUG_KMS("possible_crtcs = 0x%x\n", encoder->possible_crtcs);

	ret = drm_simple_encoder_init(drm_dev, encoder,
				      DRM_MODE_ENCODER_TMDS);
	if (ret) {
		DRM_ERROR("failed to initialize encoder with drm\n");
		return ret;
	}

	drm_encoder_helper_add(encoder, &rockchip_dp_encoder_helper_funcs);

	return 0;
}

static int rockchip_dp_bind(struct device *dev, struct device *master,
			    void *data)
{
	struct rockchip_dp_device *dp = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	int ret;

	dp->drm_dev = drm_dev;

	ret = rockchip_dp_drm_create_encoder(dp);
	if (ret) {
		DRM_ERROR("failed to create drm encoder\n");
		return ret;
	}

	rockchip_drm_encoder_set_crtc_endpoint_id(&dp->encoder,
						  dev->of_node, 0, 0);

	dp->plat_data.encoder = &dp->encoder.encoder;

	ret = analogix_dp_bind(dp->adp, drm_dev);
	if (ret)
		goto err_cleanup_encoder;

	return 0;
err_cleanup_encoder:
	dp->encoder.encoder.funcs->destroy(&dp->encoder.encoder);
	return ret;
}

static void rockchip_dp_unbind(struct device *dev, struct device *master,
			       void *data)
{
	struct rockchip_dp_device *dp = dev_get_drvdata(dev);

	analogix_dp_unbind(dp->adp);
	dp->encoder.encoder.funcs->destroy(&dp->encoder.encoder);
}

static const struct component_ops rockchip_dp_component_ops = {
	.bind = rockchip_dp_bind,
	.unbind = rockchip_dp_unbind,
};

static int rockchip_dp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct rockchip_dp_chip_data *dp_data;
	struct drm_panel *panel = NULL;
	struct rockchip_dp_device *dp;
	struct resource *res;
	int i;
	int ret;

	dp_data = of_device_get_match_data(dev);
	if (!dp_data)
		return -ENODEV;

	ret = drm_of_find_panel_or_bridge(dev->of_node, 1, 0, &panel, NULL);
	if (ret < 0 && ret != -ENODEV)
		return ret;

	dp = devm_kzalloc(dev, sizeof(*dp), GFP_KERNEL);
	if (!dp)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	i = 0;
	while (dp_data[i].reg) {
		if (dp_data[i].reg == res->start) {
			dp->data = &dp_data[i];
			break;
		}

		i++;
	}

	if (!dp->data)
		return dev_err_probe(dev, -EINVAL, "no chip-data for %s node\n",
				     dev->of_node->name);

	dp->dev = dev;
	dp->adp = ERR_PTR(-ENODEV);
	dp->plat_data.panel = panel;
	dp->plat_data.dev_type = dp->data->chip_type;
	dp->plat_data.power_on = rockchip_dp_poweron;
	dp->plat_data.power_off = rockchip_dp_powerdown;
	dp->plat_data.get_modes = rockchip_dp_get_modes;

	ret = rockchip_dp_of_probe(dp);
	if (ret < 0)
		return ret;

	platform_set_drvdata(pdev, dp);

	dp->adp = analogix_dp_probe(dev, &dp->plat_data);
	if (IS_ERR(dp->adp))
		return PTR_ERR(dp->adp);

	ret = component_add(dev, &rockchip_dp_component_ops);
	if (ret)
		return ret;

	return 0;
}

static void rockchip_dp_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &rockchip_dp_component_ops);
}

static int rockchip_dp_suspend(struct device *dev)
{
	struct rockchip_dp_device *dp = dev_get_drvdata(dev);

	if (IS_ERR(dp->adp))
		return 0;

	return analogix_dp_suspend(dp->adp);
}

static int rockchip_dp_resume(struct device *dev)
{
	struct rockchip_dp_device *dp = dev_get_drvdata(dev);

	if (IS_ERR(dp->adp))
		return 0;

	return analogix_dp_resume(dp->adp);
}

static DEFINE_RUNTIME_DEV_PM_OPS(rockchip_dp_pm_ops, rockchip_dp_suspend,
		rockchip_dp_resume, NULL);

static const struct rockchip_dp_chip_data rk3399_edp[] = {
	{
		.lcdc_sel = GRF_REG_FIELD(0x6250, 5, 5),
		.chip_type = RK3399_EDP,
		.reg = 0xff970000,
	},
	{ /* sentinel */ }
};

static const struct rockchip_dp_chip_data rk3288_dp[] = {
	{
		.lcdc_sel = GRF_REG_FIELD(0x025c, 5, 5),
		.chip_type = RK3288_DP,
		.reg = 0xff970000,
	},
	{ /* sentinel */ }
};

static const struct of_device_id rockchip_dp_dt_ids[] = {
	{.compatible = "rockchip,rk3288-dp", .data = &rk3288_dp },
	{.compatible = "rockchip,rk3399-edp", .data = &rk3399_edp },
	{}
};
MODULE_DEVICE_TABLE(of, rockchip_dp_dt_ids);

struct platform_driver rockchip_dp_driver = {
	.probe = rockchip_dp_probe,
	.remove_new = rockchip_dp_remove,
	.driver = {
		   .name = "rockchip-dp",
		   .pm = pm_ptr(&rockchip_dp_pm_ops),
		   .of_match_table = rockchip_dp_dt_ids,
	},
};
