// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Intel Keem Bay thermal Driver
 *
 * Copyright (C) 2020 Intel Corporation
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include "intel_tsens_thermal.h"
#include "keembay_tsens.h"

struct keembay_thermal_priv {
	const char *name;
	void __iomem *base_addr;
	/* sensor lock*/
	int current_temp[KEEMBAY_SENSOR_MAX];
	struct intel_tsens_plat_data *plat_data;
};

static void kmb_sensor_read_temp(void __iomem *regs_val,
				 int offset,
				 int sample_valid_mask,
				 int sample_value,
				 int bit_shift,
				 int *temp)
{
	int reg_val, kmb_raw_index;

	reg_val = ioread32(regs_val + offset);
	if (reg_val & sample_valid_mask) {
		reg_val = (reg_val >> bit_shift) & sample_value;
		kmb_raw_index = reg_val - KEEMBAY_SENSOR_BASE_TEMP;
		if (kmb_raw_index < 0)
			reg_val = raw_kmb[0];
		else if (kmb_raw_index > (raw_kmb_size - 1))
			reg_val = -255;
		else
			reg_val = raw_kmb[kmb_raw_index];
		*temp = reg_val;
	} else {
		*temp = -255;
	}
}

/*The lock is assumed to be held by the caller.*/
static int keembay_get_temp(struct platform_device *pdev, int type, int *temp)
{
	struct keembay_thermal_priv *priv = platform_get_drvdata(pdev);

	switch (type) {
	case KEEMBAY_SENSOR_MSS:
		kmb_sensor_read_temp(priv->base_addr,
				     AON_TSENS_DATA0,
				     MSS_T_SAMPLE_VALID,
				     MSS_T_SAMPLE,
				     MSS_BIT_SHIFT,
				     temp);
		priv->current_temp[KEEMBAY_SENSOR_MSS] = *temp;
		break;

	case KEEMBAY_SENSOR_CSS:
		kmb_sensor_read_temp(priv->base_addr,
				     AON_TSENS_DATA0,
				     CSS_T_SAMPLE_VALID,
				     CSS_T_SAMPLE,
				     CSS_BIT_SHIFT,
				     temp);
		priv->current_temp[KEEMBAY_SENSOR_CSS] = *temp;
		break;

	case KEEMBAY_SENSOR_NCE:
	{
		int nce0, nce1;

		kmb_sensor_read_temp(priv->base_addr,
				     AON_TSENS_DATA1,
				     NCE0_T_SAMPLE_VALID,
				     NCE0_T_SAMPLE,
				     NCE0_BIT_SHIFT,
				     &nce0);
		kmb_sensor_read_temp(priv->base_addr,
				     AON_TSENS_DATA1,
				     NCE1_T_SAMPLE_VALID,
				     NCE1_T_SAMPLE,
				     NCE1_BIT_SHIFT,
				     &nce1);
		*temp = max(nce0, nce1);
		priv->current_temp[KEEMBAY_SENSOR_NCE] = *temp;
	}
		break;

	case KEEMBAY_SENSOR_SOC:
	{
		int i;

		*temp = 0;
		for (i = 0; i < KEEMBAY_SENSOR_MAX; i++)
			*temp = max(*temp, priv->current_temp[i]);
	}
		break;

	default:
		break;
	}

	return 0;
}

static int keembay_thermal_probe(struct platform_device *pdev)
{
	struct intel_tsens_plat_data *plat_data;
	struct keembay_thermal_priv *priv;

	plat_data = pdev->dev.platform_data;
	if (!plat_data) {
		dev_err(&pdev->dev, "Platform data not found\n");
		return -EINVAL;
	}
	if (!plat_data->base_addr)
		return -EINVAL;

	priv = devm_kzalloc(&pdev->dev,
			    sizeof(struct keembay_thermal_priv),
			    GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->name = plat_data->name;
	priv->base_addr = plat_data->base_addr;
	priv->plat_data = plat_data;
	plat_data->get_temp = keembay_get_temp;
	platform_set_drvdata(pdev, priv);
	dev_info(&pdev->dev, "Thermal driver loaded for %s\n",
		 plat_data->name);
	return 0;
}

static struct platform_driver keembay_thermal_driver = {
	.probe = keembay_thermal_probe,
	.driver = {
		.name = "intel,keembay_thermal",
	},
};

module_platform_driver(keembay_thermal_driver);

MODULE_DESCRIPTION("Keem Bay Thermal Driver");
MODULE_AUTHOR("Sandeep Singh <sandeep1.singh@intel.com>");
MODULE_AUTHOR("Raja Subramanian, Lakshmi Bai <lakshmi.bai.raja.subramanian@intel.com>");
MODULE_AUTHOR("Udhayakumar C <udhayakumar.c@intel.com>");
MODULE_LICENSE("GPL v2");
