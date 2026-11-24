// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2018, Linaro Limited

#include <linux/input-event-codes.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include "common.h"
#include "qdsp6/q6afe.h"
#include "sdw.h"

#define SLIM_MAX_TX_PORTS 16
#define SLIM_MAX_RX_PORTS 16
#define WCD9335_DEFAULT_MCLK_RATE	9600000

struct apq8096_snd_data {
	/* SoundWire streams of the WSA881x amplifiers behind the codec */
	bool stream_prepared[AFE_PORT_MAX];
	struct snd_soc_jack jack;
	bool jack_setup;
};

static int apq8096_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	return 0;
}

/*
 * The SLIMbus link also carries the SoundWire master and amplifiers, which
 * come first so the master sees their ports: find the WCD9335 among them.
 */
static struct snd_soc_dai *apq8096_slim_codec_dai(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai;
	int i;

	for_each_rtd_codec_dais(rtd, i, codec_dai)
		if (strstr(codec_dai->name, "wcd9335"))
			return codec_dai;

	return snd_soc_rtd_to_codec(rtd, 0);
}

static int msm_snd_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = apq8096_slim_codec_dai(rtd);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0;

	ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
	if (ret != 0 && ret != -ENOTSUPP) {
		pr_err("failed to get codec chan map, err:%d\n", ret);
		goto end;
	} else if (ret == -ENOTSUPP) {
		return 0;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
						  rx_ch_cnt, rx_ch);
	else
		ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt, tx_ch,
						  0, NULL);
	if (ret != 0 && ret != -ENOTSUPP)
		pr_err("Failed to set cpu chan map, err:%d\n", ret);
	else if (ret == -ENOTSUPP)
		ret = 0;
end:
	return ret;
}

static int apq8096_snd_startup(struct snd_pcm_substream *substream)
{
	return qcom_snd_sdw_startup(substream);
}

static void apq8096_snd_shutdown(struct snd_pcm_substream *substream)
{
	qcom_snd_sdw_shutdown(substream);
}

static int apq8096_snd_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct apq8096_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_prepare(substream,
				    &data->stream_prepared[cpu_dai->id]);
}

static int apq8096_snd_hw_free(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct apq8096_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return qcom_snd_sdw_hw_free(substream,
				    &data->stream_prepared[cpu_dai->id]);
}

static const struct snd_soc_ops apq8096_ops = {
	.startup = apq8096_snd_startup,
	.shutdown = apq8096_snd_shutdown,
	.hw_params = msm_snd_hw_params,
	.prepare = apq8096_snd_prepare,
	.hw_free = apq8096_snd_hw_free,
};

static void apq8096_jack_free(struct snd_jack *jack)
{
	struct snd_soc_component *component = jack->private_data;

	snd_soc_component_set_jack(component, NULL, NULL);
}

/*
 * Headset detection is done by the codec. A codec that cannot detect the
 * headset on this board rejects set_jack, leave the card without a jack then.
 */
static int apq8096_jack_init(struct snd_soc_pcm_runtime *rtd,
			     struct snd_soc_component *component)
{
	struct snd_soc_card *card = rtd->card;
	struct apq8096_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_jack *jack;
	int ret;

	if (data->jack_setup)
		return 0;

	if (snd_soc_component_set_jack(component, NULL, NULL) == -ENOTSUPP)
		return 0;

	ret = snd_soc_card_jack_new(card, "Headset Jack",
				    SND_JACK_HEADSET | SND_JACK_LINEOUT |
				    SND_JACK_MECHANICAL |
				    SND_JACK_BTN_0 | SND_JACK_BTN_1 |
				    SND_JACK_BTN_2 | SND_JACK_BTN_3,
				    &data->jack);
	if (ret) {
		dev_err(card->dev, "Unable to add Headset Jack: %d\n", ret);
		return ret;
	}

	jack = data->jack.jack;
	snd_jack_set_key(jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
	snd_jack_set_key(jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);

	jack->private_data = component;
	jack->private_free = apq8096_jack_free;
	data->jack_setup = true;

	/* Audio keeps working without headset detection */
	ret = snd_soc_component_set_jack(component, &data->jack, NULL);
	if (ret)
		dev_warn(card->dev, "Failed to set jack: %d\n", ret);

	return 0;
}

static int apq8096_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = apq8096_slim_codec_dai(rtd);

	/*
	 * Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13
	 * TX14, TX15, TX16
	 */
	unsigned int rx_ch[SLIM_MAX_RX_PORTS] = {144, 145, 146, 147, 148, 149,
					150, 151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[SLIM_MAX_TX_PORTS] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};

	snd_soc_dai_set_channel_map(codec_dai, ARRAY_SIZE(tx_ch),
					tx_ch, ARRAY_SIZE(rx_ch), rx_ch);

	snd_soc_dai_set_sysclk(codec_dai, 0, WCD9335_DEFAULT_MCLK_RATE,
				SNDRV_PCM_STREAM_PLAYBACK);

	return apq8096_jack_init(rtd, codec_dai->component);
}

static void apq8096_add_be_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->be_hw_params_fixup = apq8096_be_hw_params_fixup;
			link->init = apq8096_init;
			link->ops = &apq8096_ops;
		}
	}
}

static int apq8096_platform_probe(struct platform_device *pdev)
{
	struct apq8096_snd_data *data;
	struct snd_soc_card *card;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->driver_name = "apq8096";
	card->dev = dev;
	card->owner = THIS_MODULE;
	dev_set_drvdata(dev, card);
	snd_soc_card_set_drvdata(card, data);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	apq8096_add_be_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id msm_snd_apq8096_dt_match[] = {
	{.compatible = "qcom,apq8096-sndcard"},
	{}
};

MODULE_DEVICE_TABLE(of, msm_snd_apq8096_dt_match);

static struct platform_driver msm_snd_apq8096_driver = {
	.probe  = apq8096_platform_probe,
	.driver = {
		.name = "msm-snd-apq8096",
		.of_match_table = msm_snd_apq8096_dt_match,
	},
};
module_platform_driver(msm_snd_apq8096_driver);
MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@linaro.org");
MODULE_DESCRIPTION("APQ8096 ASoC Machine Driver");
MODULE_LICENSE("GPL");
