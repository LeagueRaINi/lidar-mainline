// SPDX-License-Identifier: GPL-2.0
/*
 * Amazon Echo Plus (2nd generation, "lidar") ALSA SoC machine driver
 *
 * Copyright (c) 2016 MediaTek Inc.
 *
 * Derived from the MediaTek/Amazon mt8167 driver in Amazon's GPL release.
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* Every microphone ADC fills two 32 bit slots of the TDM frame. */
#define LIDAR_MIC_SLOT_WIDTH	32
#define LIDAR_MIC_MAX_CODECS	4

enum {
	LIDAR_OUTPUT_SPEAKER,
	LIDAR_OUTPUT_LINE,
};

enum {
	/* FE */
	DAI_LINK_DL1_PLAYBACK,
	DAI_LINK_MIC_CAPTURE,
	DAI_LINK_AWB_CAPTURE,
	/* BE */
	DAI_LINK_SPEAKER,
	DAI_LINK_MIC,
	DAI_LINK_DL_LOOPBACK,
};

struct lidar_priv {
	struct gpio_desc *amp_gpio;
	struct gpio_desc *mux_gpio;
	unsigned int route;
};

static int lidar_amp_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct lidar_priv *priv = snd_soc_card_get_drvdata(card);

	gpiod_set_value_cansleep(priv->amp_gpio, SND_SOC_DAPM_EVENT_ON(event));

	return 0;
}

/*
 * GPIO109 switches the DAC output between the speaker amplifier and the
 * 3.5 mm jack. The endpoint that is not selected is disabled, and the
 * speaker endpoint (and so the amplifier) is powered down before the switch
 * moves away from it.
 */
static int lidar_apply_route(struct snd_soc_card *card, unsigned int route)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	struct lidar_priv *priv = snd_soc_card_get_drvdata(card);

	if (route == LIDAR_OUTPUT_SPEAKER) {
		snd_soc_dapm_disable_pin(dapm, "Line Out");
		snd_soc_dapm_sync(dapm);
		gpiod_set_value_cansleep(priv->mux_gpio, 1);
		snd_soc_dapm_enable_pin(dapm, "Speaker");
	} else {
		snd_soc_dapm_disable_pin(dapm, "Speaker");
		snd_soc_dapm_sync(dapm);
		gpiod_set_value_cansleep(priv->mux_gpio, 0);
		snd_soc_dapm_enable_pin(dapm, "Line Out");
	}
	priv->route = route;

	return snd_soc_dapm_sync(dapm);
}

static int lidar_output_route_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct lidar_priv *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.enumerated.item[0] = priv->route;

	return 0;
}

static int lidar_output_route_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct lidar_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned int route = ucontrol->value.enumerated.item[0];
	int ret;

	if (route > LIDAR_OUTPUT_LINE)
		return -EINVAL;
	if (route == priv->route)
		return 0;

	ret = lidar_apply_route(card, route);

	return ret ? ret : 1;
}

static const char * const lidar_output_route_text[] = {
	[LIDAR_OUTPUT_SPEAKER] = "Speaker",
	[LIDAR_OUTPUT_LINE] = "Line Out",
};

static SOC_ENUM_SINGLE_EXT_DECL(lidar_output_route_enum,
				lidar_output_route_text);

static const struct snd_kcontrol_new lidar_controls[] = {
	SOC_ENUM_EXT("Output Route", lidar_output_route_enum,
		     lidar_output_route_get, lidar_output_route_put),
};

static const struct snd_soc_dapm_widget lidar_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", lidar_amp_event),
	SND_SOC_DAPM_LINE("Line Out", NULL),
};

static const struct snd_soc_dapm_route lidar_routes[] = {
	{"Speaker", NULL, "HPL"},
	{"Speaker", NULL, "HPR"},
	{"Speaker", NULL, "LOL"},
	{"Speaker", NULL, "LOR"},
	{"Line Out", NULL, "HPL"},
	{"Line Out", NULL, "HPR"},
	{"Line Out", NULL, "LOL"},
	{"Line Out", NULL, "LOR"},
};

static int lidar_late_probe(struct snd_soc_card *card)
{
	return lidar_apply_route(card, LIDAR_OUTPUT_SPEAKER);
}

/*
 * The DAC always gets the full rate master clock of the sample rate family,
 * 512 fs at 48 kHz.
 */
static int lidar_speaker_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	unsigned int mclk = params_rate(params) % 8000 ? 22579200 : 24576000;
	int ret;

	ret = snd_soc_dai_set_sysclk(snd_soc_rtd_to_cpu(rtd, 0), 0, mclk,
				     SND_SOC_CLOCK_OUT);
	if (ret)
		return ret;

	return snd_soc_dai_set_sysclk(snd_soc_rtd_to_codec(rtd, 0), 0, mclk,
				      SND_SOC_CLOCK_IN);
}

static const struct snd_soc_ops lidar_speaker_ops = {
	.hw_params = lidar_speaker_hw_params,
};

/*
 * The ADCs share the data line of one TDM frame in DSP mode B, each of them
 * sending its stereo pair in the next two slots. They are clocked from the
 * bit clock and launch data on its rising edge, which is the inverted bit
 * clock polarity in ASoC terms, while the TDM input of the SoC samples with
 * its default polarity.
 */
static int lidar_mic_init(struct snd_soc_pcm_runtime *rtd)
{
	unsigned int slots = 2 * rtd->dai_link->num_codecs;
	struct snd_soc_dai *codec_dai;
	int i, ret;

	ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_cpu(rtd, 0),
				  SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_BP_FP);
	if (ret)
		return ret;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		unsigned int mask = 0x3 << (2 * i);

		ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_DSP_B |
					  SND_SOC_DAIFMT_IB_NF |
					  SND_SOC_DAIFMT_BC_FC);
		if (ret)
			return ret;

		ret = snd_soc_dai_set_tdm_slot(codec_dai, mask, mask, slots,
					       LIDAR_MIC_SLOT_WIDTH);
		if (ret)
			return ret;
	}

	return 0;
}

/* The TDM frame layout is fixed by the ADCs, there is no conversion. */
static int lidar_mic_fe_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai_link *mic_link = &rtd->card->dai_link[DAI_LINK_MIC];
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	ret = snd_pcm_hw_constraint_single(runtime, SNDRV_PCM_HW_PARAM_CHANNELS,
					   2 * mic_link->num_codecs);
	if (ret < 0)
		return ret;

	ret = snd_pcm_hw_constraint_mask64(runtime, SNDRV_PCM_HW_PARAM_FORMAT,
					   SNDRV_PCM_FMTBIT_S32_LE);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct snd_soc_ops lidar_mic_fe_ops = {
	.startup = lidar_mic_fe_startup,
};

SND_SOC_DAILINK_DEFS(playback,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(mic_capture,
		     DAILINK_COMP_ARRAY(COMP_CPU("TDM_IN")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(awb_capture,
		     DAILINK_COMP_ARRAY(COMP_CPU("AWB")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(speaker,
		     DAILINK_COMP_ARRAY(COMP_CPU("I2S")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(mic,
		     DAILINK_COMP_ARRAY(COMP_CPU("TDM_IN_IO")),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(dl_loopback,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL Input")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link lidar_dai_links[] = {
	/* Front End DAI links */
	[DAI_LINK_DL1_PLAYBACK] = {
		.name = "DL1_FE",
		.stream_name = "Playback",
		.id = DAI_LINK_DL1_PLAYBACK,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.playback_only = 1,
		.dpcm_merged_format = 1,
		.dpcm_merged_rate = 1,
		SND_SOC_DAILINK_REG(playback),
	},
	[DAI_LINK_MIC_CAPTURE] = {
		.name = "TDM_IN_FE",
		.stream_name = "Microphone Capture",
		.id = DAI_LINK_MIC_CAPTURE,
		/* Start the memory interface before the TDM input feeds it. */
		.trigger = {
			SND_SOC_DPCM_TRIGGER_PRE,
			SND_SOC_DPCM_TRIGGER_PRE
		},
		.dynamic = 1,
		.capture_only = 1,
		.dpcm_merged_format = 1,
		.dpcm_merged_rate = 1,
		.ops = &lidar_mic_fe_ops,
		SND_SOC_DAILINK_REG(mic_capture),
	},
	[DAI_LINK_AWB_CAPTURE] = {
		.name = "AWB_FE",
		.stream_name = "Playback Reference Capture",
		.id = DAI_LINK_AWB_CAPTURE,
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(awb_capture),
	},
	/* Back End DAI links */
	[DAI_LINK_SPEAKER] = {
		.name = "I2S_BE",
		.id = DAI_LINK_SPEAKER,
		.no_pcm = 1,
		.playback_only = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		.ops = &lidar_speaker_ops,
		SND_SOC_DAILINK_REG(speaker),
	},
	[DAI_LINK_MIC] = {
		.name = "TDM_IN_BE",
		.id = DAI_LINK_MIC,
		.no_pcm = 1,
		.capture_only = 1,
		.init = lidar_mic_init,
		SND_SOC_DAILINK_REG(mic),
	},
	[DAI_LINK_DL_LOOPBACK] = {
		.name = "DL_BE",
		.id = DAI_LINK_DL_LOOPBACK,
		.no_pcm = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(dl_loopback),
	},
};

static struct snd_soc_card lidar_card = {
	.name = "amazon-lidar",
	.owner = THIS_MODULE,
	.dai_link = lidar_dai_links,
	.num_links = ARRAY_SIZE(lidar_dai_links),
	.controls = lidar_controls,
	.num_controls = ARRAY_SIZE(lidar_controls),
	.dapm_widgets = lidar_widgets,
	.num_dapm_widgets = ARRAY_SIZE(lidar_widgets),
	.dapm_routes = lidar_routes,
	.num_dapm_routes = ARRAY_SIZE(lidar_routes),
	.late_probe = lidar_late_probe,
};

static void lidar_put_node(void *data)
{
	of_node_put(data);
}

static void lidar_put_codecs(void *data)
{
	struct snd_soc_card *card = data;

	snd_soc_of_put_dai_link_codecs(&card->dai_link[DAI_LINK_SPEAKER]);
	snd_soc_of_put_dai_link_codecs(&card->dai_link[DAI_LINK_MIC]);
}

static int lidar_parse_codecs(struct device *dev, const char *name,
			      struct snd_soc_dai_link *link)
{
	struct device_node *link_node __free(device_node) =
		of_get_child_by_name(dev->of_node, name);
	struct device_node *codec_node __free(device_node) =
		of_get_child_by_name(link_node, "codec");
	int ret;

	if (!codec_node)
		return dev_err_probe(dev, -EINVAL, "missing %s codec node\n",
				     name);

	ret = snd_soc_of_get_dai_link_codecs(dev, codec_node, link);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get %s codecs\n",
				     name);

	return 0;
}

static int lidar_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &lidar_card;
	struct device *dev = &pdev->dev;
	struct device_node *platform_node;
	struct snd_soc_dai_link *link;
	struct lidar_priv *priv;
	int i, ret;

	platform_node = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	if (!platform_node)
		return dev_err_probe(dev, -EINVAL,
				     "missing mediatek,platform property\n");

	/* The DAI links refer to the node for as long as the card exists. */
	ret = devm_add_action_or_reset(dev, lidar_put_node, platform_node);
	if (ret)
		return ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->amp_gpio = devm_gpiod_get(dev, "amp-enable", GPIOD_OUT_LOW);
	if (IS_ERR(priv->amp_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->amp_gpio),
				     "failed to get amplifier enable GPIO\n");

	/* The output route control starts out as "Speaker". */
	priv->mux_gpio = devm_gpiod_get(dev, "dac-mux", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->mux_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->mux_gpio),
				     "failed to get DAC mux GPIO\n");

	card->dev = dev;
	snd_soc_card_set_drvdata(card, priv);

	for_each_card_prelinks(card, i, link) {
		link->cpus->of_node = platform_node;
		link->platforms->of_node = platform_node;
	}

	ret = devm_add_action(dev, lidar_put_codecs, card);
	if (ret)
		return ret;

	ret = lidar_parse_codecs(dev, "speaker-dai-link",
				 &card->dai_link[DAI_LINK_SPEAKER]);
	if (ret)
		return ret;

	link = &card->dai_link[DAI_LINK_MIC];
	ret = lidar_parse_codecs(dev, "mic-dai-link", link);
	if (ret)
		return ret;

	if (link->num_codecs > LIDAR_MIC_MAX_CODECS)
		return dev_err_probe(dev, -EINVAL, "too many microphone ADCs\n");

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id lidar_dt_match[] = {
	{ .compatible = "amazon,lidar-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, lidar_dt_match);

static struct platform_driver lidar_driver = {
	.driver = {
		.name = "mt8516-amazon-lidar",
		.of_match_table = lidar_dt_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = lidar_probe,
};
module_platform_driver(lidar_driver);

MODULE_DESCRIPTION("Amazon Echo Plus (2nd generation) ALSA SoC machine driver");
MODULE_LICENSE("GPL");
