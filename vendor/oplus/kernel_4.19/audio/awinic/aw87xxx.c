/*
 * aw87xxx.c  aw87xxx pa module
 *
 * Copyright (c) 2020 AWINIC Technology CO., LTD
 *
 * Author: Bruce <zhaolei@awinic.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/irq.h>
#include <linux/firmware.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/gameport.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include "aw87xxx.h"
#include "aw87339.h"
#include "aw87359.h"
#include "aw87369.h"
#include "aw87519.h"
#include "aw87559.h"
#include "aw87xxx_monitor.h"
#include "aw_bin_parse.h"
#include "awinic_dsp.h"
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
#include <soc/oplus/system/oplus_mm_kevent_fb.h>
#define OPLUS_AUDIO_EVENTID_SMARTPA_ERR    10041
#endif

#ifdef CONFIG_SND_SOC_OPLUS_PA_MANAGER
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.CODEC,2021/10/19,add PA manager*/
#include "../oplus_speaker_manager/oplus_speaker_manager_platform.h"
#include "../oplus_speaker_manager/oplus_speaker_manager_codec.h"
#endif /* CONFIG_SND_SOC_OPLUS_PA_MANAGER */

#ifdef CONFIG_SND_SOC_OPLUS_PA_MANAGER
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.CODEC,2021/10/19,add PA manager*/
void *oplus_pa_aw_node = NULL;
uint8_t aw_pa_mode = AW87XXX_MUSIC_MODE;
#endif /* CONFIG_SND_SOC_OPLUS_PA_MANAGER */

/*****************************************************************
* aw87xxx marco
******************************************************************/
#define AW87XXX_I2C_NAME	"aw87xxx_pa"
#define AW87XXX_DRIVER_VERSION	"V0.1.14"
#define AW87XXX_PRODUCT_NUM		4
#define AW87XXX_PRODUCT_NAME_LEN	8

enum {
	AW87XXX_LEFT_CHANNEL = 0,
	AW87XXX_RIGHT_CHANNEL = 1,
};

/*************************************************************************
 * aw87xxx variable
 ************************************************************************/
#ifdef OPLUS_BUG_COMPATIBILITY
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.AUDIODRIVER, 2021/11/19, add for bin file load,
 *when remove odm/firmware in firmware_class.c*/
static char aw87xxx_cfg_name[AW87XXX_MODE_MAX][AW87XXX_CFG_NAME_MAX] = {
	{"../../odm/firmware/awinic/aw87xxx_pid_xx_off_"},
	{"../../odm/firmware/awinic/aw87xxx_pid_xx_music_"},
	{"../../odm/firmware/awinic/aw87xxx_pid_xx_voice_"},
	{"../../odm/firmware/awinic/aw87xxx_pid_xx_fm_"},
	{"../../odm/firmware/awinic/aw87xxx_pid_xx_rcv_"},
};
static const char aw87xxx_vmax_cfg_name[] = { "../../odm/firmware/awinic/aw87xxx_vmax_" };
#else
static char aw87xxx_cfg_name[AW87XXX_MODE_MAX][AW87XXX_CFG_NAME_MAX] = {
	{"awinic/aw87xxx_pid_xx_off_"},
	{"awinic/aw87xxx_pid_xx_music_"},
	{"awinic/aw87xxx_pid_xx_voice_"},
	{"awinic/aw87xxx_pid_xx_fm_"},
	{"awinic/aw87xxx_pid_xx_rcv_"},
};
static const char aw87xxx_vmax_cfg_name[] = { "awinic/aw87xxx_vmax_" };
#endif /*OPLUS_BUG_COMPATIBILITY*/

static char aw87xxx_support_product[][AW87XXX_PRODUCT_NAME_LEN] = {
	{"aw87339"},
	{"aw87359"},
	{"aw87369"},
	{"aw87519"},
	{"aw87559"},
};
/* Sidong.Zhao@ODM_WT.mm.audiodriver.Codec, 2021/03/24, add code for speaker low voltage*/
int aw87xxx_spk_low_voltage_status = 0;

static LIST_HEAD(g_aw87xxx_device_list);
static DEFINE_MUTEX(g_aw87xxx_mutex_lock);
static unsigned int g_aw87xxx_dev_cnt;

/* Xuewen.Yang@MM.AudioDriver.Machine 2021/04/09,add for speaker pa aw87359 flag */
int is_aw_chip = 0;

/**********************************************************
* i2c write and read
**********************************************************/
static int aw87xxx_i2c_write(struct aw87xxx *aw87xxx,
			     unsigned char reg_addr, unsigned char reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_write_byte_data(aw87xxx->i2c_client,
						reg_addr, reg_data);
		if (ret < 0) {
			aw_dev_err(aw87xxx->dev,
				   "%s: i2c_write cnt=%d error=%d\n", __func__,
				   cnt, ret);
		} else {
			break;
		}
		cnt++;
		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

static int aw87xxx_i2c_read(struct aw87xxx *aw87xxx,
			    unsigned char reg_addr, unsigned char *reg_data)
{
	int ret = -1;
	unsigned char cnt = 0;

	while (cnt < AW_I2C_RETRIES) {
		ret = i2c_smbus_read_byte_data(aw87xxx->i2c_client, reg_addr);
		if (ret < 0) {
			aw_dev_err(aw87xxx->dev,
				   "%s: i2c_read cnt=%d error=%d\n", __func__,
				   cnt, ret);
		} else {
			*reg_data = ret;
			break;
		}
		cnt++;
		msleep(AW_I2C_RETRY_DELAY);
	}

	return ret;
}

/************************************************************************
* aw87xxx hardware control
************************************************************************/
static void aw87xxx_hw_on(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);

	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 0);
		mdelay(2);
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 1);
		mdelay(2);
	}
	aw87xxx->hwen_flag = 1;

}

static void aw87xxx_hw_off(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);

	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 0);
		mdelay(2);
	}
	aw87xxx->hwen_flag = 0;

}
/****************************************************************************
* aw87xxx hardware reset
*****************************************************************************/
static void aw87xxx_hw_reset(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);

	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 0);
		mdelay(2);
		gpio_set_value_cansleep(aw87xxx->reset_gpio, 1);
		mdelay(2);
	}
}
/*******************************************************************************
* aw87xxx control interface
******************************************************************************/
static int aw87xxx_audio_off(struct aw87xxx *aw87xxx)
{
	int i;
	struct aw87xxx_scene_param *aw_off = &aw87xxx->aw87xxx_scene_ls.aw_off;

	if (aw87xxx->hwen_flag) {
		mutex_lock(&aw87xxx->lock);
		if (aw_off->cfg_update_flag == AW87XXX_CFG_OK) {
			/*send firmware data */
			for (i = 0; i < aw_off->scene_cnt->len; i = i + 2) {
				aw_dev_dbg(aw87xxx->dev,
					   "%s: reg=0x%02x, val = 0x%02x\n",
					   __func__, aw_off->scene_cnt->data[i],
					   aw_off->scene_cnt->data[i + 1]);

				aw87xxx_i2c_write(aw87xxx,
						aw_off->scene_cnt->data[i],
						aw_off->scene_cnt->data[i + 1]);
			}
		} else {
			aw_dev_dbg(aw87xxx->dev, "%s\n", __func__);
			aw87xxx_i2c_write(aw87xxx, 0x01, 0x0c);
		}
		mutex_unlock(&aw87xxx->lock);
	}

	aw87xxx_hw_off(aw87xxx);

	aw87xxx_monitor_stop(&aw87xxx->monitor);

	return 0;
}

static unsigned char aw87339_spk_music_default[] = {0x39, 0x0E, 0xA3, 0x06, 0x05, 0x0C, 0x0F, 0x52, 0x09, 0x08, 0x97};
static unsigned char aw87339_spk_voice_default[] = {0x39, 0x0E, 0xA3, 0x06, 0x05, 0x10, 0x07, 0x52, 0x06, 0x08, 0x96};

static int aw87xxx_reg_cnt_update(struct aw87xxx *aw87xxx,
				  struct aw87xxx_scene_param *scene_param)
{
	int i;
	struct aw87xxx_container *load_cnt = scene_param->scene_cnt;
	unsigned char *default_parm = NULL;

	aw_dev_info(aw87xxx->dev, "%s enter, mode:%d\n",
		    __func__, scene_param->scene_mode);
	if (!aw87xxx->hwen_flag)
		aw87xxx_hw_on(aw87xxx);
/* Sidong.Zhao@ODM_WT.mm.audiodriver.Codec, 2021/03/24, add code for speaker low voltage*/
	mutex_lock(&aw87xxx->lock);
	if (scene_param->cfg_update_flag == AW87XXX_CFG_OK) {
		/*send firmware data */
		for (i = 0; i < load_cnt->len; i = i + 2) {
			aw_dev_dbg(aw87xxx->dev,
				   "%s: reg=0x%02x, val = 0x%02x\n", __func__,
				   load_cnt->data[i], load_cnt->data[i + 1]);

			aw87xxx_i2c_write(aw87xxx, load_cnt->data[i],
					  load_cnt->data[i + 1]);
		}

		if (scene_param->scene_mode == AW87XXX_MUSIC_MODE && (aw87xxx_spk_low_voltage_status > 0))
			aw87xxx_i2c_write(aw87xxx,0x03, 0x02);//set as 7V
	} else {
/*
		aw_dev_err(aw87xxx->dev, "%s: scene not ready\n", __func__);
		mutex_unlock(&aw87xxx->lock);
		return -ENOENT;
*/
		aw_dev_err(aw87xxx->dev, "%s: use default scene_param\n", __func__);
		if (scene_param->scene_mode == AW87XXX_MUSIC_MODE)
			default_parm = aw87339_spk_music_default;
		else if (scene_param->scene_mode == AW87XXX_VOICE_MODE)
			default_parm = aw87339_spk_voice_default;

		if (default_parm != NULL) {
			for (i = 1; i < 11; i++) {
				aw_dev_dbg(aw87xxx->dev,
					   "%s: reg=0x%02x, val = 0x%02x\n", __func__,
					   i, default_parm[i]);

				aw87xxx_i2c_write(aw87xxx, i,
						  default_parm[i]);
			}
		}
	}
	mutex_unlock(&aw87xxx->lock);

	aw87xxx_monitor_start(&aw87xxx->monitor);

	return 0;
}

/* Sidong.Zhao@ODM_WT.mm.audiodriver.Codec, 2021/03/24, add code for speaker low voltage*/
void aw87xxx_audio_spk_low_voltage_status(int bStatus)
{
	struct aw87xxx_scene_param *scene_param;
	struct aw87xxx_container *load_cnt;
	struct aw87xxx *aw87xxx;
	struct list_head *pos;

	aw87xxx_spk_low_voltage_status = bStatus;

	list_for_each(pos, &g_aw87xxx_device_list) {
		aw87xxx = list_entry(pos, struct aw87xxx, list);
		if (aw87xxx == NULL) {
			aw_dev_err(aw87xxx->dev,
				"%s struct aw87xxx not ready\n", __func__);
			return;
		}

		scene_param = &aw87xxx->aw87xxx_scene_ls.aw_music;
		load_cnt = scene_param->scene_cnt;
		if (aw87xxx->hwen_flag == 1) {
			if (aw87xxx_spk_low_voltage_status > 0) {
				pr_info("%s: aw87xxx write vol reg 0x03 when playing\n", __func__);
				aw87xxx_i2c_write(aw87xxx,0x03, 0x02);//set as 7V
			} else {
				pr_info("%s: aw87xxx write vol reg normal when playing\n", __func__);
				if (scene_param->cfg_update_flag == AW87XXX_CFG_OK) {
					aw87xxx_i2c_write(aw87xxx, load_cnt->data[4], load_cnt->data[5]);
				} else {
					aw87xxx_i2c_write(aw87xxx,0x03, aw87339_spk_music_default[3]);
				}
			}
		}
	}
}

unsigned char aw87xxx_show_current_mode(int32_t channel)
{
	struct aw87xxx *aw87xxx;
	struct list_head *pos;
	uint8_t ret = 0;

	list_for_each(pos, &g_aw87xxx_device_list) {
		aw87xxx = list_entry(pos, struct aw87xxx, list);
		if (aw87xxx == NULL) {
			aw_dev_err(aw87xxx->dev,
				"%s struct aw87xxx not ready\n", __func__);
			return -EPERM;
		}
		if (aw87xxx->pa_channel == channel) {
			aw_dev_info(aw87xxx->dev,
				    "%s: %s_i2c%d@0x%02X current_mode: %d, pa_channel: %d\n",
				    __func__,
				    aw87xxx_support_product[aw87xxx->product],
				    aw87xxx->i2c_seq,
				    aw87xxx->i2c_addr,
				    aw87xxx->current_mode, aw87xxx->pa_channel);

			ret = aw87xxx->current_mode;
		}
	}

	return ret;
}

int aw87xxx_audio_scene_load(uint8_t mode, int32_t channel)
{
	struct aw87xxx_scene_param *scene_param;
	struct list_head *pos;
	struct aw87xxx *aw87xxx;
	int ret = 0;

	list_for_each(pos, &g_aw87xxx_device_list) {
		aw87xxx = list_entry(pos, struct aw87xxx, list);
		if (aw87xxx == NULL) {
			pr_err("%s struct aw87xxx not ready\n", __func__);
			return -EPERM;
		}
		if (aw87xxx->pa_channel == channel) {
			aw_dev_info(aw87xxx->dev,
				    "%s enter, mode = %d, channel = %d\n",
				    __func__, mode, channel);
/*
			if (aw87xxx->aw87xxx_scene_ls.aw_off.cfg_update_flag != AW87XXX_CFG_OK) {
				aw_dev_err(aw87xxx->dev, "%s:off mode not exist, pa not work\n",
											__func__);
				return -EPERM;
			}
*/
			switch (mode) {
			case AW87XXX_OFF_MODE:
				aw87xxx->current_mode = mode;
				return aw87xxx_audio_off(aw87xxx);
			case AW87XXX_MUSIC_MODE:
				aw87xxx->current_mode = mode;
				scene_param =
				    &aw87xxx->aw87xxx_scene_ls.aw_music;
				ret =
				    aw87xxx_reg_cnt_update(aw87xxx,
							   scene_param);
				break;
			case AW87XXX_VOICE_MODE:
				aw87xxx->current_mode = mode;
				scene_param =
				    &aw87xxx->aw87xxx_scene_ls.aw_voice;
				ret =
				    aw87xxx_reg_cnt_update(aw87xxx,
							   scene_param);
				break;
			case AW87XXX_FM_MODE:
				aw87xxx->current_mode = mode;
				scene_param = &aw87xxx->aw87xxx_scene_ls.aw_fm;
				ret =
				    aw87xxx_reg_cnt_update(aw87xxx,
							   scene_param);
				break;
			case AW87XXX_RCV_MODE:
				aw87xxx->current_mode = mode;
				scene_param = &aw87xxx->aw87xxx_scene_ls.aw_rcv;
				ret =
				    aw87xxx_reg_cnt_update(aw87xxx,
							   scene_param);
				break;
			default:
				aw_dev_err(aw87xxx->dev,
					   "%s unsupported mode: %d\n",
					   __func__, mode);
				return -EINVAL;
			}
		}
	}
	return ret;
}

#ifdef CONFIG_SND_SOC_OPLUS_PA_MANAGER
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.CODEC,2021/10/19,add PA manager*/
void aw87xxx_enable_pa(int enable, int mode, int32_t channel) {
	pr_info("%s: enable = %d, mode = %d, channel = %d\n", __func__, enable, mode, channel);
	if(enable) {
		if(mode == SPK_MODE){
			aw87xxx_audio_scene_load(aw_pa_mode, channel);
		}else{
			aw87xxx_audio_scene_load(AW87XXX_RCV_MODE, channel);
		}
	} else {
		aw87xxx_audio_scene_load(AW87XXX_OFF_MODE, channel);
	}
}

void aw87xxx_enable_l_pa(int enable, int mode){
	aw87xxx_enable_pa(enable, mode, AW87XXX_LEFT_CHANNEL);
}

void aw87xxx_enable_r_pa(int enable, int mode){
	aw87xxx_enable_pa(enable, mode, AW87XXX_RIGHT_CHANNEL);
}

int ext_amp_low_voltage_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol){
	if (ucontrol->value.integer.value[0] >= 2) {
		aw87xxx_audio_spk_low_voltage_status(0);
	} else {
		aw87xxx_audio_spk_low_voltage_status(1);
	}

	pr_info("%s: value.integer.value = %ld\n", __func__, ucontrol->value.integer.value[0]);
	return 0;

}

int ext_amp_low_voltage_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
        ucontrol->value.integer.value[0] = aw87xxx_spk_low_voltage_status;
        pr_info("%s: aw87xxx_spk_low_voltage_status = %d\n", __func__, aw87xxx_spk_low_voltage_status);
        return 0;
}

int aw87xxx_spk_mode_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol){

	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;

	if (ucontrol->value.enumerated.item[0] >= e->items)
		return -EINVAL;

	if(ucontrol->value.integer.value[0] == 0){
		aw_pa_mode = AW87XXX_MUSIC_MODE;
	}else if(ucontrol->value.integer.value[0] == 1){
		aw_pa_mode = AW87XXX_VOICE_MODE;
	}else if(ucontrol->value.integer.value[0] == 2){
		aw_pa_mode = AW87XXX_FM_MODE;
	}

	return 0;
}

int aw87xxx_spk_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol){
	pr_info("%s() = %d\n", __func__, aw_pa_mode);
	//kcontrol enum 0~2 is "Music", "Voice", "Fm"
        //aw87xxx_scene_mode enum 1~3 is "Music", "Voice", "Fm", so need subtract 1
	ucontrol->value.integer.value[0] = aw_pa_mode -1;
	return 0;
}


bool is_mute_status = false;
int aw87xxx_speaker_mute_set(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;

	if (ucontrol->value.enumerated.item[0] >= e->items)
		return -EINVAL;

	if(ucontrol->value.integer.value[0]){
		aw87xxx_audio_scene_load(AW87XXX_OFF_MODE, AW87XXX_RIGHT_CHANNEL);
		aw87xxx_audio_scene_load(AW87XXX_OFF_MODE, AW87XXX_LEFT_CHANNEL);
		is_mute_status = true;
	}else{
		aw87xxx_audio_scene_load(aw_pa_mode, AW87XXX_RIGHT_CHANNEL);
		aw87xxx_audio_scene_load(aw_pa_mode, AW87XXX_LEFT_CHANNEL);
		is_mute_status = false;
	}

	return 0;
}

int aw87xxx_speaker_mute_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	pr_info("%s() = %d\n", __func__, is_mute_status);
	ucontrol->value.integer.value[0] = is_mute_status;
	return 0;
}
#endif /*CONFIG_SND_SOC_OPLUS_PA_MANAGER*/

/****************************************************************************
* aw873xx firmware cfg update
***************************************************************************/
static void aw87xxx_parse_chip_name(struct aw87xxx *aw87xxx,
				    uint32_t *chip_name, uint32_t len)
{
	int i;
	char chip_name_str[8] = { 0 };

	memcpy(chip_name_str, (char *)chip_name, len);
	for (i = 0; i < len; i++) {
		if (chip_name_str[i] == 'A')
			break;
	}
	memcpy(aw87xxx->chip_name, chip_name_str + i, len - i);
	aw_dev_info(aw87xxx->dev, "%s: chip name:%s\n",
		    __func__, aw87xxx->chip_name);
}

static int aw87xxx_get_parse_fw_way(struct aw87xxx *aw87xxx,
				    const struct firmware *cont)
{
	int i = 0;
	uint32_t sum_data = 0;
	uint32_t check_sum = 0;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	memcpy(&check_sum, cont->data, sizeof(check_sum));
	/*check bit */
	for (i = sizeof(check_sum); i < cont->size; i++)
		sum_data += cont->data[i];

	aw_dev_info(aw87xxx->dev, "%s: check_sum:%d sum_data:%d\n",
		    __func__, check_sum, sum_data);
	if (check_sum != sum_data) {
		aw_dev_info(aw87xxx->dev, "%s: use old parsing\n", __func__);
		return AW87XXX_OLD_FIRMWARE;
	} else {
		aw_dev_info(aw87xxx->dev, "%s: use frame head parsing\n",
			    __func__);
		return AW87XXX_FH_FIRMWARE;
	}
}

static int aw87xxx_parse_add_head_firmware(struct aw87xxx *aw87xxx,
					   struct aw87xxx_scene_param
					   *current_scene,
					   const struct firmware *cont)
{
	int i = 0;
	int ret = 0;
	struct ui_frame_header *frame_header;
	struct aw87xxx_container *curr_scene_cnt;

	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);
	if (current_scene->bin_file != NULL)
		current_scene->bin_file = NULL;

	current_scene->bin_file =
	    kzalloc(sizeof(struct aw_bin) + cont->size, GFP_KERNEL);

	if (!current_scene->bin_file) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		return -ENOMEM;
	}

	memcpy(&current_scene->bin_file->info.data, cont->data, cont->size);

	spin_lock(&aw87xxx->bin_parse_lock);
	ret = aw_parsing_bin_file(current_scene->bin_file);
	spin_unlock(&aw87xxx->bin_parse_lock);

	if (ret != 0) {
		aw_dev_err(aw87xxx->dev, "%s: File to parse bin file %d\n",
			   __func__, current_scene->scene_mode);
		return -EPERM;
	}

	if (current_scene->frame_header != NULL)
		current_scene->frame_header = NULL;

	frame_header = current_scene->frame_header =
	    kzalloc(sizeof(struct ui_frame_header), GFP_KERNEL);
	if (!frame_header) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		return -ENOMEM;
	}

	/*check addr wide, data wide and bin type */
	if (current_scene->bin_file->header_info[0].reg_byte_len !=
	    AW87XXX_ADDR_BYTE_LEN
	    || current_scene->bin_file->header_info[0].data_byte_len !=
	    AW87XXX_DATA_BYTE_LEN
	    || current_scene->bin_file->header_info[0].bin_data_type !=
	    AW87XXX_BIN_TYPE_REG) {
		aw_dev_err(aw87xxx->dev, "%s: %d bin file type mismatch\n",
			   __func__,
			   current_scene->bin_file->header_info[0].
			   bin_data_type);
	}

	aw_dev_info(aw87xxx->dev, "%s: bin_fh_ver:0x%08x\n",
		   __func__,
		   current_scene->bin_file->header_info[0].header_ver);
	aw_dev_info(aw87xxx->dev, "%s: ui_ver:0x%08x\n", __func__,
		   current_scene->bin_file->header_info[0].ui_ver);
	aw_dev_info(aw87xxx->dev, "%s: bin_data_ver:0x%08x\n", __func__,
		   current_scene->bin_file->header_info[0].bin_data_ver);

	aw_dev_info(aw87xxx->dev, "%s: reg_byte_len:0x%08x\n", __func__,
		    current_scene->bin_file->header_info[0].reg_byte_len);
	aw_dev_info(aw87xxx->dev, "%s: data_byte_len:0x%08x\n", __func__,
		    current_scene->bin_file->header_info[0].data_byte_len);
	aw_dev_info(aw87xxx->dev, "%s: bin_data_type:0x%08x\n", __func__,
		   current_scene->bin_file->header_info[0].bin_data_type);


	aw87xxx_parse_chip_name(aw87xxx,
		(uint32_t *)current_scene->bin_file->header_info[0].chip_type,
		sizeof(frame_header->chip_name));

	if (current_scene->scene_cnt != NULL)
		current_scene->scene_cnt = NULL;

	curr_scene_cnt = current_scene->scene_cnt =
	    kzalloc(current_scene->bin_file->header_info[0].bin_data_len +
		    sizeof(int), GFP_KERNEL);
	if (!curr_scene_cnt) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		kfree(frame_header);
		return -ENOMEM;
	}
	curr_scene_cnt->len =
	    current_scene->bin_file->header_info[0].valid_data_len;
	memcpy(curr_scene_cnt->data,
		cont->data + current_scene->bin_file->header_info[0].valid_data_addr,
		current_scene->bin_file->header_info[0].valid_data_len);

	current_scene->cfg_update_flag = AW87XXX_CFG_OK;

	for (i = 0; i < curr_scene_cnt->len; i = i + 2) {
		aw_dev_dbg(aw87xxx->dev,
			    "%s; mode is %d; addr:0x%02x, data:0x%02x\n",
			    __func__, current_scene->scene_mode,
			    curr_scene_cnt->data[i],
			    curr_scene_cnt->data[i + 1]);
	}

	aw_dev_info(aw87xxx->dev, "%s: %s update complete\n",
		    __func__, aw87xxx->cfg_name[current_scene->scene_mode]);
	return 0;
}

static void aw87xxx_parse_non_head_firmware(struct aw87xxx *aw87xxx,
				       struct aw87xxx_scene_param
				       *current_scene,
				       const struct firmware *cont)
{
	int i = 0;
	struct aw87xxx_container *scene_cnt;

	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);
	if (current_scene->scene_cnt != NULL)
		current_scene->scene_cnt = NULL;

	scene_cnt = current_scene->scene_cnt =
	    kzalloc(cont->size + sizeof(int), GFP_KERNEL);
	if (!scene_cnt) {
		aw_dev_err(aw87xxx->dev, "%s: Error allocating memory\n",
			   __func__);
		return;
	}

	scene_cnt->len = cont->size;
	memcpy(scene_cnt->data, cont->data, cont->size);
	current_scene->cfg_update_flag = AW87XXX_CFG_OK;

	for (i = 0; i < scene_cnt->len; i = i + 2) {
		aw_dev_dbg(aw87xxx->dev, "%s: addr:0x%02x, data:0x%02x\n",
			    __func__, scene_cnt->data[i],
			    scene_cnt->data[i + 1]);
	}

	aw_dev_info(aw87xxx->dev, "%s: %s update complete\n",
		    __func__, aw87xxx->cfg_name[current_scene->scene_mode]);
}

static void aw87xxx_parse_firmware(struct aw87xxx *aw87xxx,
				   struct aw87xxx_scene_param *current_scene,
				   const struct firmware *cont)
{
	int ret;

	ret = aw87xxx_get_parse_fw_way(aw87xxx, cont);
	if (ret == AW87XXX_OLD_FIRMWARE) {
		aw87xxx_parse_non_head_firmware(aw87xxx, current_scene, cont);
		aw_dev_info(aw87xxx->dev, "%s: old bin file", __func__);
	} else if (ret == AW87XXX_FH_FIRMWARE) {
		aw87xxx_parse_add_head_firmware(aw87xxx, current_scene, cont);
		aw_dev_info(aw87xxx->dev, "%s: new bin file", __func__);
	}
	release_firmware(cont);
}

static void aw87xxx_scene_cfg_loaded(const struct firmware *cont, void *context)
{
	int ram_timer_val = 2000;
	struct aw87xxx_scene_param *current_scene = context;
	struct aw87xxx *aw87xxx = NULL;

	switch (current_scene->scene_mode) {
	case AW87XXX_OFF_MODE:
		aw87xxx = container_of(current_scene,
				       struct aw87xxx, aw87xxx_scene_ls.aw_off);
		break;
	case AW87XXX_MUSIC_MODE:
		aw87xxx = container_of(current_scene,
				       struct aw87xxx,
				       aw87xxx_scene_ls.aw_music);
		break;
	case AW87XXX_VOICE_MODE:
		aw87xxx = container_of(current_scene,
				       struct aw87xxx,
				       aw87xxx_scene_ls.aw_voice);
		break;
	case AW87XXX_FM_MODE:
		aw87xxx = container_of(current_scene,
				       struct aw87xxx, aw87xxx_scene_ls.aw_fm);
		break;
	case AW87XXX_RCV_MODE:
		aw87xxx = container_of(current_scene,
				       struct aw87xxx, aw87xxx_scene_ls.aw_rcv);
		break;
	default:
		pr_err("%s: unsupported scence:%d\n",
		       __func__, current_scene->scene_mode);
		return;
	}

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	current_scene->update_num++;
	if (!cont) {
		aw_dev_err(aw87xxx->dev, "%s: failed to read %s\n",
			   __func__,
			   aw87xxx->cfg_name[current_scene->scene_mode]);

		release_firmware(cont);

		if (current_scene->update_num < AW87XXX_LOAD_CFG_RETRIES) {
			aw_dev_info(aw87xxx->dev,
				    "%s:restart hrtimer to load firmware\n",
				    __func__);
			schedule_delayed_work(&aw87xxx->ram_work,
					      msecs_to_jiffies(ram_timer_val));
		}
		return;
	}

	aw_dev_info(aw87xxx->dev, "%s: loaded %s - size: %zu\n",
		    __func__, aw87xxx->cfg_name[current_scene->scene_mode],
		    cont ? cont->size : 0);

	/* aw87xxx ram update */
	aw87xxx_parse_firmware(aw87xxx, current_scene, cont);
}

static int aw87xxx_scene_update(struct aw87xxx *aw87xxx, uint8_t scence_mode)
{
	struct aw87xxx_scene_param *current_scene = NULL;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
		__func__, aw87xxx_support_product[aw87xxx->product],
		aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	switch (scence_mode) {
	case AW87XXX_OFF_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.aw_off;
		break;
	case AW87XXX_MUSIC_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.aw_music;
		break;
	case AW87XXX_VOICE_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.aw_voice;
		break;
	case AW87XXX_FM_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.aw_fm;
		break;
	case AW87XXX_RCV_MODE:
		current_scene = &aw87xxx->aw87xxx_scene_ls.aw_rcv;
		break;
	default:
		aw_dev_err(aw87xxx->dev, "%s: unsupported scence:%d\n",
			   __func__, scence_mode);
		return -EINVAL;
	}

	return request_firmware_nowait(THIS_MODULE,
				       FW_ACTION_HOTPLUG,
				       aw87xxx->cfg_name[scence_mode],
				       aw87xxx->dev,
				       GFP_KERNEL,
				       current_scene, aw87xxx_scene_cfg_loaded);
}

static void aw87xxx_vmax_cfg_loaded(const struct firmware *cont, void *context)
{
	struct aw87xxx *aw87xxx = context;
	struct vmax_config *vmax_cfg = NULL;
	int vmax_cfg_num = 0, i;
	int ram_timer_val = 2000;

	aw_dev_info(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n", __func__,
		    aw87xxx_support_product[aw87xxx->product],
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	aw87xxx->monitor.update_num++;
	if (!cont) {
		aw_dev_err(aw87xxx->dev, "%s: failed to read %s\n",
			   __func__, aw87xxx->vmax_cfg_name);

		release_firmware(cont);

		if (aw87xxx->monitor.update_num < AW87XXX_LOAD_CFG_RETRIES) {
			aw_dev_info(aw87xxx->dev,
				    "%s:restart hrtimer to load firmware\n",
				    __func__);
			schedule_delayed_work(&aw87xxx->ram_work,
					      msecs_to_jiffies(ram_timer_val));
		}
		return;
	}

	if (aw87xxx->monitor.vmax_cfg != NULL)
		aw87xxx->monitor.vmax_cfg = NULL;

	vmax_cfg_num = cont->size / sizeof(struct vmax_single_config);

	vmax_cfg = aw87xxx->monitor.vmax_cfg =
			kzalloc((vmax_cfg_num * sizeof(struct vmax_single_config) +
					sizeof(vmax_cfg_num)), GFP_KERNEL);
	if (!vmax_cfg) {
		aw_dev_info(aw87xxx->dev, "%s: vmax_cfg kzalloc failed\n",
			   __func__);
		return;
	}

	vmax_cfg->vmax_cfg_num = vmax_cfg_num;
	for (i = 0; i < cont->size; i += 8) {
		vmax_cfg->vmax_cfg_total[i / 8].min_thr =
		    (cont->data[i + 3] << 24) + (cont->data[i + 2] << 16) +
		    (cont->data[i + 1] << 8) + (cont->data[i]);

		vmax_cfg->vmax_cfg_total[i / 8].vmax =
		    (cont->data[i + 7] << 24) + (cont->data[i + 6] << 16) +
		    (cont->data[i + 5] << 8) + (cont->data[i + 4]);

		aw_dev_info(aw87xxx->dev, "%s: minthr:%d ,vmax:0x%08x\n",
			    __func__, vmax_cfg->vmax_cfg_total[i / 8].min_thr,
			    vmax_cfg->vmax_cfg_total[i / 8].vmax);
	}

	release_firmware(cont);

	aw87xxx->monitor.cfg_update_flag = AW87XXX_CFG_OK;

	aw_dev_info(aw87xxx->dev, "%s: vbat monitor update complete\n",
		    __func__);
}

static int aw87xxx_vbat_monitor_update(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);

	return request_firmware_nowait(THIS_MODULE,
				FW_ACTION_HOTPLUG,
				aw87xxx->vmax_cfg_name,
				aw87xxx->dev,
				GFP_KERNEL,
				aw87xxx, aw87xxx_vmax_cfg_loaded);
}

static void aw87xxx_cfg_work_routine(struct work_struct *work)
{
	struct aw87xxx *aw87xxx = container_of(work,
					       struct aw87xxx, ram_work.work);
	struct aw87xxx_scene_list *aw_scene_ls = &aw87xxx->aw87xxx_scene_ls;

	aw_dev_info(aw87xxx->dev, "%s enter\n", __func__);
	aw_scene_ls->aw_off.scene_mode = AW87XXX_OFF_MODE;
	aw_scene_ls->aw_music.scene_mode = AW87XXX_MUSIC_MODE;
	aw_scene_ls->aw_voice.scene_mode = AW87XXX_VOICE_MODE;
	aw_scene_ls->aw_fm.scene_mode = AW87XXX_FM_MODE;
	aw_scene_ls->aw_rcv.scene_mode = AW87XXX_RCV_MODE;

	if (aw_scene_ls->aw_off.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx, aw_scene_ls->aw_off.scene_mode);
	if (aw_scene_ls->aw_music.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx, aw_scene_ls->aw_music.scene_mode);
	if (aw_scene_ls->aw_voice.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx, aw_scene_ls->aw_voice.scene_mode);
	if (aw_scene_ls->aw_fm.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx, aw_scene_ls->aw_fm.scene_mode);
	if (aw_scene_ls->aw_rcv.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_scene_update(aw87xxx, aw_scene_ls->aw_rcv.scene_mode);
	if (aw87xxx->monitor.cfg_update_flag == AW87XXX_CFG_WAIT)
		aw87xxx_vbat_monitor_update(aw87xxx);
}

static int aw87xxx_cfg_init(struct aw87xxx *aw87xxx)
{
#ifdef AWINIC_CFG_UPDATE_DELAY
	int cfg_timer_val = 5000;
#else
	int cfg_timer_val = 0;
#endif
	aw87xxx->aw87xxx_scene_ls.aw_off.cfg_update_flag = AW87XXX_CFG_WAIT;
	aw87xxx->monitor.cfg_update_flag = AW87XXX_CFG_WAIT;

	INIT_DELAYED_WORK(&aw87xxx->ram_work, aw87xxx_cfg_work_routine);
	schedule_delayed_work(&aw87xxx->ram_work,
			      msecs_to_jiffies(cfg_timer_val));

	return 0;
}

/****************************************************************************
* aw87xxx compatible
*****************************************************************************/
static int aw87xxx_product_select(struct aw87xxx *aw87xxx, uint8_t chip_type)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	switch (chip_type) {
	case AW87XXX_339:
	aw87xxx->product = AW87XXX_339;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87339_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87339_reg_access;
		break;
	case AW87XXX_359:
		aw87xxx->product = AW87XXX_359;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87359_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87359_reg_access;
		break;
	case AW87XXX_369:
		aw87xxx->product = AW87XXX_369;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87369_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87369_reg_access;
		break;
	case AW87XXX_519:
		aw87xxx->product = AW87XXX_519;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87519_REG_MAX;
		aw87xxx->reg_param.reg_access = aw87519_reg_access;
		break;
	case AW87XXX_559:
		aw87xxx->product = AW87XXX_559;
		aw_dev_info(aw87xxx->dev,
			    "%s: %s_i2c%d@0x%02X selected, chipid: 0x%02X\n",
			    __func__,
			    aw87xxx_support_product[aw87xxx->product],
			    aw87xxx->i2c_seq,
			    aw87xxx->i2c_addr, aw87xxx->chipid);
		aw87xxx->reg_param.reg_max = AW87559_REG_MAX;
		aw87xxx->reg_param.reg_access = AW87559_reg_access;
		break;
	default:
		aw_dev_err(aw87xxx->dev, "%s: unverified chip: %d\n",
			   __func__, chip_type);
		return -EINVAL;
	}
	return 0;
}

static void aw87xxx_parse_scene_mode_dt(struct aw87xxx *aw87xxx)
{
	int ret;
	const char *scene_mode_str;
	struct device_node *np = aw87xxx->dev->of_node;
	struct aw87xxx_scene_list *aw_scene_ls = &aw87xxx->aw87xxx_scene_ls;

	ret = of_property_read_string(np, "secne-mode", &scene_mode_str);
	if (ret < 0) {
		aw_dev_info(aw87xxx->dev,
			    "%s:read secne-mode failed,use default\n",
			    __func__);
		aw_scene_ls->aw_music.cfg_update_flag = AW87XXX_CFG_WAIT;
		aw_scene_ls->aw_voice.cfg_update_flag = AW87XXX_CFG_WAIT;
		aw_scene_ls->aw_fm.cfg_update_flag = AW87XXX_CFG_WAIT;
		aw_scene_ls->aw_rcv.cfg_update_flag = AW87XXX_CFG_WAIT;
		return;
	}
	aw_scene_ls->aw_music.cfg_update_flag = AW87XXX_CFG_NG;
	aw_scene_ls->aw_voice.cfg_update_flag = AW87XXX_CFG_NG;
	aw_scene_ls->aw_fm.cfg_update_flag = AW87XXX_CFG_NG;
	aw_scene_ls->aw_rcv.cfg_update_flag = AW87XXX_CFG_NG;

	if (strnstr(scene_mode_str, "music", sizeof("music"))) {
		aw_scene_ls->aw_music.cfg_update_flag = AW87XXX_CFG_WAIT;
	} else {
		aw_dev_info(aw87xxx->dev,
			    "%s:have not got secne-mode music\n", __func__);
	}
	if (strnstr(scene_mode_str, "voice", sizeof("voice"))) {
		aw_scene_ls->aw_voice.cfg_update_flag = AW87XXX_CFG_WAIT;
	} else {
		aw_dev_info(aw87xxx->dev,
			    "%s:have not got secne-mode voice\n", __func__);
	}
	if (strnstr(scene_mode_str, "fm", sizeof("fm"))) {
		aw_scene_ls->aw_fm.cfg_update_flag = AW87XXX_CFG_WAIT;
	} else {
		aw_dev_info(aw87xxx->dev,
			    "%s:have not get secne-mode fm\n", __func__);
	}
	if (strnstr(scene_mode_str, "rcv", sizeof("rcv"))) {
		aw_scene_ls->aw_rcv.cfg_update_flag = AW87XXX_CFG_WAIT;
	} else {
		aw_dev_info(aw87xxx->dev,
			    "%s:have not get secne-mode rcv\n", __func__);
	}
}

static void aw87xxx_cfg_free(struct aw87xxx *aw87xxx)
{
	int i;
	struct aw87xxx_scene_param *scene_param =
	    &aw87xxx->aw87xxx_scene_ls.aw_off;

	for (i = 0; i < AW87XXX_MODE_MAX; i++) {
		if (scene_param->frame_header != NULL) {
			kfree(scene_param->frame_header);
			scene_param->frame_header = NULL;
		}
		if (scene_param->scene_cnt != NULL) {
			kfree(scene_param->scene_cnt);
			scene_param->scene_cnt = NULL;
		}
		if (scene_param->bin_file != NULL) {
			kfree(scene_param->bin_file);
			scene_param->bin_file = NULL;
		}
		aw_dev_info(aw87xxx->dev, "kfree %d cfg",
			    scene_param->scene_mode);
		scene_param++;
	}
	memset(&aw87xxx->aw87xxx_scene_ls, 0,
	       sizeof(struct aw87xxx_scene_list));
	aw87xxx_parse_scene_mode_dt(aw87xxx);
	aw87xxx->aw87xxx_scene_ls.aw_off.cfg_update_flag = AW87XXX_CFG_WAIT;

	if (aw87xxx->monitor.vmax_cfg != NULL) {
		kfree(aw87xxx->monitor.vmax_cfg);
		aw87xxx->monitor.vmax_cfg = NULL;
	}
	aw87xxx->monitor.cfg_update_flag = AW87XXX_CFG_WAIT;
	aw87xxx->monitor.update_num = AW87XXX_CFG_NUM_INIT;
}

/****************************************************************************
* aw87xxx attribute
*****************************************************************************/
static ssize_t aw87xxx_get_reg(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	unsigned int i = 0;
	unsigned char reg_val = 0;
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	aw_dev_info(aw87xxx->dev, "%s enter, i2c%d@0x%02X, channel = %d\n",
		__func__, aw87xxx->i2c_client->adapter->nr,
		aw87xxx->i2c_client->addr, aw87xxx->pa_channel);
	for (i = 0; i < aw87xxx->reg_param.reg_max; i++) {
		if (!(aw87xxx->reg_param.reg_access[i] & AW87XXX_REG_RD_ACCESS))
			continue;
		aw87xxx_i2c_read(aw87xxx, i, &reg_val);
		len += snprintf(buf + len, PAGE_SIZE - len,
				"reg:0x%02X=0x%02X\n", i, reg_val);
	}

	return len;
}

static ssize_t aw87xxx_set_reg(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t len)
{
	unsigned int databuf[2] = { 0 };
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	if (sscanf(buf, "%x %x", &databuf[0], &databuf[1]) == 2)
		aw87xxx_i2c_write(aw87xxx, databuf[0], databuf[1]);

	return len;
}

static ssize_t aw87xxx_get_hwen(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	len += snprintf(buf + len, PAGE_SIZE - len, "hwen: %u\n",
			aw87xxx->hwen_flag);

	return len;
}

static ssize_t aw87xxx_set_hwen(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	ssize_t ret;
	unsigned int state;
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	ret = kstrtouint(buf, 10, &state);
	if (ret) {
		aw_dev_err(aw87xxx->dev, "%s: fail to change str to int\n",
			   __func__);
		return ret;
	}
	if (state == 0)
		aw87xxx_hw_off(aw87xxx); /*OFF*/
	else
		aw87xxx_hw_on(aw87xxx); /*ON*/
	return len;
}

static ssize_t aw87xxx_set_update(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	ssize_t ret;
	unsigned int state;
	int cfg_timer_val = 10;
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	aw_dev_dbg(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
			    __func__, aw87xxx_support_product[aw87xxx->product],
				aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	ret = kstrtouint(buf, 10, &state);
	if (ret) {
		aw_dev_err(aw87xxx->dev, "%s: fail to change str to int\n",
			   __func__);
		return ret;
	}
	if (state) {
		aw87xxx_cfg_free(aw87xxx);

		schedule_delayed_work(&aw87xxx->ram_work,
				      msecs_to_jiffies(cfg_timer_val));
	}

	return len;
}

static ssize_t aw87xxx_get_mode(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	len += snprintf(buf + len, PAGE_SIZE - len, "0: off mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "1: music mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "2: voice mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "3: fm mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "4: rcv mode\n");
	len += snprintf(buf + len, PAGE_SIZE - len, "current mode:%u\n",
			aw87xxx->current_mode);
	return len;
}

static ssize_t aw87xxx_set_mode(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t len)
{
	int ret;
	uint8_t mode;
	struct aw87xxx *aw87xxx = dev_get_drvdata(dev);

	aw_dev_dbg(aw87xxx->dev, "%s enter, %s_i2c%d@0x%02X\n",
		    __func__, aw87xxx_support_product[aw87xxx->product],
			aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	ret = kstrtou8(buf, 0, &mode);

	if (ret) {
		aw_dev_err(aw87xxx->dev, "%s: invalid input to mode!\n",
			   __func__);
		return ret;
	}

	if (mode < 5) {
		aw_dev_info(aw87xxx->dev, "%s: %d\n", __func__, mode);
		aw87xxx_audio_scene_load(mode, aw87xxx->pa_channel);
	} else {
		aw_dev_err(aw87xxx->dev, "%s: input num out of range!\n",
			__func__);
	}

	return len;
}

static DEVICE_ATTR(reg, S_IWUSR | S_IRUGO,
		aw87xxx_get_reg, aw87xxx_set_reg);
static DEVICE_ATTR(hwen, S_IWUSR | S_IRUGO,
		aw87xxx_get_hwen, aw87xxx_set_hwen);
static DEVICE_ATTR(update, S_IWUSR,
		NULL, aw87xxx_set_update);
static DEVICE_ATTR(mode, S_IWUSR | S_IRUGO,
		aw87xxx_get_mode, aw87xxx_set_mode);

static struct attribute *aw87xxx_attributes[] = {
	&dev_attr_reg.attr,
	&dev_attr_hwen.attr,
	&dev_attr_update.attr,
	&dev_attr_mode.attr,
	NULL
};

static struct attribute_group aw87xxx_attribute_group = {
	.attrs = aw87xxx_attributes
};

/****************************************************************************
* aw87xxx parse dts
*****************************************************************************/
static void aw87xxx_parse_gpio_dt(struct aw87xxx *aw87xxx,
					struct device_node *np)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
			aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	aw87xxx->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (aw87xxx->reset_gpio < 0) {
		aw_dev_err(aw87xxx->dev,
			   "%s: no reset gpio provided, hardware reset unavailable\n",
			__func__);
		aw87xxx->reset_gpio = -1;
	} else {
		aw_dev_info(aw87xxx->dev, "%s: reset gpio provided ok\n",
			 __func__);
	}
}

static void aw87xxx_int_to_str(struct aw87xxx *aw87xxx)
{
	int32_t temp_channel = aw87xxx->pa_channel;

	aw_dev_info(aw87xxx->dev,
		"%s enter, dev_i2c%d@0x%02X, pa_channel: %d\n",
		__func__, aw87xxx->i2c_seq, aw87xxx->i2c_addr,
		aw87xxx->pa_channel);


	if (temp_channel >= 0) {
		snprintf(aw87xxx->channel, sizeof(aw87xxx->channel), "%d",
			 temp_channel);
		aw_dev_info(aw87xxx->dev, "%s: channel: %s", __func__,
					aw87xxx->channel);
	} else {
		aw87xxx->channel[0] = '-';
		aw87xxx->pa_channel = 0;
	}
}

static void aw87xxx_parse_channel_dt(struct aw87xxx *aw87xxx,
				     struct device_node *np)
{
	int ret = 0;

	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);

	snprintf(aw87xxx->bus_num, sizeof(aw87xxx->bus_num), "%u",
		 aw87xxx->i2c_seq);

	if (aw87xxx->i2c_addr == 0x58) {
		aw87xxx->name_suffix = "58";
	} else if (aw87xxx->i2c_addr == 0x59) {
		aw87xxx->name_suffix = "59";
	} else if (aw87xxx->i2c_addr == 0x5a) {
		aw87xxx->name_suffix = "5a";
	} else if (aw87xxx->i2c_addr == 0x5b) {
		aw87xxx->name_suffix = "5b";
	} else {
		aw_dev_info(aw87xxx->dev,
			 "%s:not stereo channel,use default single track\n",
			 __func__);
		goto chan_default;
	}

	ret = of_property_read_u32(np, "pa-channel", &aw87xxx->pa_channel);
	if (ret != 0) {
		aw87xxx->pa_channel = 0;
		aw_dev_err(aw87xxx->dev, "%s: pa-channel ,user default value!, ret: %d\n",
				__func__, ret);
	} else {
		aw_dev_info(aw87xxx->dev, "%s: pa-channel: %d\n",
				__func__, aw87xxx->pa_channel);
	}
	aw87xxx_int_to_str(aw87xxx);

	return;

 chan_default:
	aw87xxx->name_suffix = NULL;
}

static void aw87xxx_parse_dt(struct aw87xxx *aw87xxx, struct device_node *np)
{
	aw_dev_info(aw87xxx->dev, "%s enter, dev_i2c%d@0x%02X\n", __func__,
		    aw87xxx->i2c_seq, aw87xxx->i2c_addr);
	aw87xxx_parse_gpio_dt(aw87xxx, np);
	aw87xxx_parse_monitor_dt(&aw87xxx->monitor);
	aw87xxx_parse_scene_mode_dt(aw87xxx);
	aw87xxx_parse_channel_dt(aw87xxx, np);
}
/*****************************************************
* check chip id
*****************************************************/
static int aw87xxx_update_cfg_name(struct aw87xxx *aw87xxx)
{
#ifdef OPLUS_BUG_COMPATIBILITY
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.AUDIODRIVER, 2021/11/19, add for bin file load,
 *when remove odm/firmware in firmware_class.c*/
	char aw87xxx_head[] = { "../../odm/firmware/awinic/aw87xxx_pid_" };
#else
	char aw87xxx_head[] = { "awinic/aw87xxx_pid_" };
#endif /*OPLUS_BUG_COMPATIBILITY*/
	char aw87xxx_end[] = { ".bin" };
	char buf[3] = { 0 };
	int i = 0;
	uint8_t head_index = 0;
	int cfg_size = 0;

	memcpy(aw87xxx->cfg_name, aw87xxx_cfg_name, sizeof(aw87xxx_cfg_name));
	head_index = sizeof(aw87xxx_head) - 1;

	/*add product information */
	snprintf(buf, sizeof(buf), "%02x", aw87xxx->chipid);

	for (i = 0; i < sizeof(aw87xxx->cfg_name) / AW87XXX_CFG_NAME_MAX; i++)
		memcpy(aw87xxx->cfg_name[i] + head_index, buf, sizeof(buf) - 1);

	for (i = 0; i < sizeof(aw87xxx->cfg_name) / AW87XXX_CFG_NAME_MAX; i++) {
		if (aw87xxx->channel[0] != '-') {
			cfg_size = strlen(aw87xxx->cfg_name[i]);
			memcpy(aw87xxx->cfg_name[i] + cfg_size,
			       aw87xxx->channel, strlen(aw87xxx->channel));
			cfg_size = strlen(aw87xxx->cfg_name[i]);
			memcpy(aw87xxx->cfg_name[i] + cfg_size, aw87xxx_end,
			       strlen(aw87xxx_end));
			aw_dev_info(aw87xxx->dev, "%s:cfg_name: %s\n",
					__func__, aw87xxx->cfg_name[i]);
		} else {
			cfg_size = strlen(aw87xxx->cfg_name[i]);
			memcpy(aw87xxx->cfg_name[i] + cfg_size - 1, aw87xxx_end,
			       strlen(aw87xxx_end));
			aw_dev_info(aw87xxx->dev, "%s:cfg_name: %s\n",
						__func__, aw87xxx->cfg_name[i]);

		}
	}

	/*vamx bin */
	if (aw87xxx->channel[0] != '-') {
		memcpy(aw87xxx->vmax_cfg_name, aw87xxx_vmax_cfg_name,
		       strlen(aw87xxx_vmax_cfg_name));
		memcpy(aw87xxx->vmax_cfg_name + strlen(aw87xxx->vmax_cfg_name),
		       aw87xxx->channel, strlen(aw87xxx->channel));
		memcpy(aw87xxx->vmax_cfg_name + strlen(aw87xxx->vmax_cfg_name),
		       aw87xxx_end, strlen(aw87xxx_end));
		aw_dev_info(aw87xxx->dev,
			    "%s: dev_i2c%d@0x%02X, vmax_name: %s\n", __func__,
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr,
			    aw87xxx->vmax_cfg_name);
	} else {
		memcpy(aw87xxx->vmax_cfg_name, aw87xxx_vmax_cfg_name,
		       strlen(aw87xxx_vmax_cfg_name));
		memcpy(aw87xxx->vmax_cfg_name + strlen(aw87xxx->vmax_cfg_name) -
		       1, aw87xxx_end, strlen(aw87xxx_end));
		aw_dev_info(aw87xxx->dev,
			    "%s: dev_i2c%d@0x%02X, vmax_name: %s\n", __func__,
			    aw87xxx->i2c_seq, aw87xxx->i2c_addr,
			    aw87xxx->vmax_cfg_name);
	}

	return 0;
}

static int aw87xxx_read_chipid(struct aw87xxx *aw87xxx)
{
	unsigned int cnt = 0;
	int ret = -1;
	unsigned char reg_val = 0;

	while (cnt < AW_READ_CHIPID_RETRIES) {
		ret = aw87xxx_i2c_read(aw87xxx, AW87XXX_REG_CHIPID, &reg_val);
		aw_dev_info(aw87xxx->dev,
			"%s: the chip is aw87xxx chipid=0x%x\n",
			__func__, reg_val);
/* Xuewen.Yang@MM.AudioDriver.Machine 2021/04/09,add for speaker pa aw87359 flag */
		if((reg_val == AW87XXX_CHIPID_39) || (reg_val == AW87XXX_CHIPID_59) || (reg_val == AW87XXX_CHIPID_69) || (reg_val == AW87XXX_CHIPID_5A)){
			is_aw_chip = 1;
			aw_dev_info(aw87xxx->dev,
			"%s: the chip is aw87xxx is_aw_chip=0x%x\n",
			__func__, is_aw_chip);
		}

		switch (reg_val) {
		case AW87XXX_CHIPID_39:
			aw87xxx->chipid = reg_val;
			aw87xxx_product_select(aw87xxx, AW87XXX_339);
			aw87xxx_update_cfg_name(aw87xxx);
			return 0;
		case AW87XXX_CHIPID_59:
			aw87xxx->chipid = reg_val;
			if (gpio_is_valid(aw87xxx->reset_gpio))
				aw87xxx_product_select(aw87xxx, AW87XXX_519);
			else
				aw87xxx_product_select(aw87xxx, AW87XXX_359);
			aw87xxx_update_cfg_name(aw87xxx);
			return 0;
		case AW87XXX_CHIPID_69:
			aw87xxx->chipid = reg_val;
			aw87xxx_product_select(aw87xxx, AW87XXX_369);
			aw87xxx_update_cfg_name(aw87xxx);
			return 0;
		case AW87XXX_CHIPID_5A:
			aw87xxx->chipid = reg_val;
			aw87xxx_product_select(aw87xxx, AW87XXX_559);
			aw87xxx_update_cfg_name(aw87xxx);
			aw_dev_info(aw87xxx->dev,
				    "%s: find device chip id is (0x%x)\n",
				    __func__, reg_val);
			return 0;
		default:
			aw_dev_info(aw87xxx->dev,
				    "%s: unsupported device revision (0x%x)\n",
				    __func__, reg_val);
			break;
		}
		cnt++;

		mdelay(AW_READ_CHIPID_RETRY_DELAY);
	}
	aw_dev_err(aw87xxx->dev, "%s: aw87xxx chipid=0x%x error\n",
		   __func__, reg_val);

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_MM_FEEDBACK)
	/*just feedback: i2c control no error (ret == 0) and read chip id value is 0 (reg_val == 0)*/
	if ((ret == 0) && (reg_val == 0)) {
		mm_fb_audio_kevent_named_delay(OPLUS_AUDIO_EVENTID_SMARTPA_ERR, MM_FB_KEY_RATELIMIT_5MIN, \
			FEEDBACK_DELAY_60S, "payload@@aw87xxx read chip id error");
	}
#endif

	return -EINVAL;
}

static struct aw87xxx *aw87xxx_malloc_init(struct i2c_client *client)
{
	struct aw87xxx *aw87xxx =
	    devm_kzalloc(&client->dev, sizeof(struct aw87xxx), GFP_KERNEL);
	if (aw87xxx == NULL) {
		dev_err(&client->dev, "%s: devm_kzalloc failed.\n", __func__);
		return NULL;
	}

	aw87xxx->i2c_client = client;

	pr_info("%s enter , client_addr = 0x%02x\n", __func__,
		aw87xxx->i2c_client->addr);
	INIT_LIST_HEAD(&aw87xxx->list);

	return aw87xxx;
}

static void aw87xxx_softreset(struct aw87xxx *aw87xxx)
{
	aw_dev_info(aw87xxx->dev, "%s: enter", __func__);

	aw87xxx_i2c_write(aw87xxx, SOFTRESET_ADDR, SOFTRESET_VALUE);
	aw_dev_info(aw87xxx->dev, "%s: softreset, 0x%x = 0x%x", __func__,
					SOFTRESET_ADDR, SOFTRESET_VALUE);
}

/****************************************************************************
* aw87xxx i2c driver
*****************************************************************************/
static int aw87xxx_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct aw87xxx *aw87xxx = NULL;
	int ret = -1;

#ifdef CONFIG_SND_SOC_OPLUS_PA_MANAGER
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.CODEC,2021/10/19,add PA manager*/
	struct oplus_speaker_device *speaker_device = NULL;
	bool new_speaker_device_node = false;
	uint32_t real_addr = 0;

	pr_warning("%s, %d, client->addr = %#x\n", __func__, __LINE__, client->addr);
	ret = of_property_read_u32(np, "oplus-real-addr", &real_addr);
	if (ret == 0 && real_addr != 0) {
		pr_warning("%s, %d, client->addr = %#x, real_addr = %#x\n", __func__, __LINE__, client->addr, real_addr);
		client->addr = real_addr;
	} else {
		pr_err("%s, %d, no oplus-real-addr\n", __func__, __LINE__);
	}
#endif /* CONFIG_SND_SOC_OPLUS_PA_MANAGER */

	pr_info("%s enter , i2c%d@0x%02x\n", __func__,
		client->adapter->nr, client->addr);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		aw_dev_err(&client->dev, "%s: check_functionality failed\n",
			   __func__);
		ret = -ENODEV;
		goto exit_check_functionality_failed;
	}

	aw87xxx = aw87xxx_malloc_init(client);
	if (aw87xxx == NULL) {
		dev_err(&client->dev, "%s: failed to parse device tree node\n",
			__func__);
		ret = -ENOMEM;
		goto exit_devm_kzalloc_failed;
	}

	aw87xxx->i2c_seq = aw87xxx->i2c_client->adapter->nr;
	aw87xxx->i2c_addr = aw87xxx->i2c_client->addr;

	aw87xxx->dev = &client->dev;
	i2c_set_clientdata(client, aw87xxx);
	mutex_init(&aw87xxx->lock);
	aw87xxx_parse_dt(aw87xxx, np);

	if (gpio_is_valid(aw87xxx->reset_gpio)) {
		ret = devm_gpio_request_one(&client->dev,
					    aw87xxx->reset_gpio,
					    GPIOF_OUT_INIT_LOW, "aw87xxx_rst");
		if (ret) {
			aw_dev_err(&client->dev,
				   "%s: rst request failed\n", __func__);
			goto exit_gpio_request_failed;
		}
	}

	aw87xxx_hw_reset(aw87xxx);

	/* aw87xxx chip id */
	ret = aw87xxx_read_chipid(aw87xxx);
	if (ret < 0) {
		aw_dev_err(&client->dev,
			   "%s: aw873xx_read_chipid failed ret=%d\n", __func__,
			   ret);
#ifdef OPLUS_BUG_COMPATIBILITY
// Haiping.Bai@MULTIMEDIA.AUDIODRIVER.AUDIODRIVER, 2021/12/15, add for AW87xxx
		aw87xxx_hw_off(aw87xxx);
		aw_dev_err(&client->dev,
			   "%s: set aw87xxx_hw_off\n", __func__);
#endif /*OPLUS_BUG_COMPATIBILITY*/
		goto exit_i2c_check_id_failed;
	}

	/* aw87xxx device name */
	if (client->dev.of_node) {
		if (aw87xxx->name_suffix)
			dev_set_name(&client->dev, "%s_%s_%s", "aw87xxx_pa",
				     aw87xxx->bus_num, aw87xxx->name_suffix);
		else
			dev_set_name(&client->dev, "%s", "aw87xxx_pa");
	} else {
		aw_dev_err(&client->dev, "%s failed to set device name: %d\n",
			   __func__, ret);
	}

	ret = sysfs_create_group(&client->dev.kobj, &aw87xxx_attribute_group);
	if (ret < 0) {
		aw_dev_err(&client->dev,
			    "%s failed to create sysfs nodes\n", __func__);
	}

	/*add device to total list */
	mutex_lock(&g_aw87xxx_mutex_lock);
	g_aw87xxx_dev_cnt++;
	list_add(&aw87xxx->list, &g_aw87xxx_device_list);
	mutex_unlock(&g_aw87xxx_mutex_lock);
	pr_info("%s: dev_cnt %d\n", __func__, g_aw87xxx_dev_cnt);
	spin_lock_init(&aw87xxx->bin_parse_lock);

#ifdef CONFIG_SND_SOC_OPLUS_PA_MANAGER
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.CODEC,2021/10/19,add PA manager*/
	pr_info("%s():,oplus_register start\r\n", __func__);

	speaker_device = get_speaker_dev(aw87xxx->pa_channel + 1);
	if (speaker_device == NULL) {
		speaker_device = kzalloc(sizeof(struct oplus_speaker_device), GFP_KERNEL);
		new_speaker_device_node = true;
	}

	if ((speaker_device !=NULL) && (aw87xxx->pa_channel == 0)) {
		speaker_device->chipset = MFR_AWINIC;
		speaker_device->type = L_SPK;
		speaker_device->vdd_need = 0;
		speaker_device->speaker_enable_set = aw87xxx_enable_l_pa;
		speaker_device->boost_voltage_set = ext_amp_low_voltage_set;
		speaker_device->boost_voltage_get = ext_amp_low_voltage_get;
		speaker_device->spk_mode_set = aw87xxx_spk_mode_set;
		speaker_device->spk_mode_get = aw87xxx_spk_mode_get;
		speaker_device->speaker_mute_set = aw87xxx_speaker_mute_set;
		speaker_device->speaker_mute_get = aw87xxx_speaker_mute_get;
	} else if ((speaker_device !=NULL) && (aw87xxx->pa_channel == 1)) {
		speaker_device->chipset = MFR_AWINIC;
		speaker_device->type = R_SPK;
		speaker_device->vdd_need = 0;
		speaker_device->speaker_enable_set = aw87xxx_enable_r_pa;
		speaker_device->boost_voltage_set = ext_amp_low_voltage_set;
		speaker_device->boost_voltage_get = ext_amp_low_voltage_get;
		speaker_device->spk_mode_set = aw87xxx_spk_mode_set;
		speaker_device->spk_mode_get = aw87xxx_spk_mode_get;
		speaker_device->speaker_mute_set = aw87xxx_speaker_mute_set;
		speaker_device->speaker_mute_get = aw87xxx_speaker_mute_get;
	} else {
		pr_info("%s():,oplus_register fail\r\n",  __func__);
	}
	if (new_speaker_device_node == true) {
		pr_info("%s():,oplus_register speaker device\r\n",  __func__);
		oplus_pa_aw_node = oplus_speaker_pa_register(speaker_device);
	}
	pr_info("%s():,oplus_register end\r\n", __func__);

#endif /* CONFIG_SND_SOC_OPLUS_PA_MANAGER */

	aw87xxx_cfg_init(aw87xxx);

	aw87xxx_softreset(aw87xxx);

	aw87xxx_hw_off(aw87xxx);

	aw87xxx_monitor_init(&aw87xxx->monitor);

	aw87xxx->open_dsp_en = aw87xx_platform_init();
	return 0;

 exit_i2c_check_id_failed:
	if (gpio_is_valid(aw87xxx->reset_gpio))
		devm_gpio_free(&client->dev, aw87xxx->reset_gpio);
 exit_gpio_request_failed:
#ifdef OPLUS_BUG_COMPATIBILITY
// Haiping.Bai@MULTIMEDIA.AUDIODRIVER.AUDIODRIVER, 2021/12/15, add for AW87xxx
	i2c_set_clientdata(client, NULL);
	pr_err("%s, %d, check id failed! set its client to NULL!", __func__, __LINE__);
#endif
	devm_kfree(&client->dev, aw87xxx);
	aw87xxx = NULL;
 exit_devm_kzalloc_failed:
 exit_check_functionality_failed:
	return ret;
}

static int aw87xxx_i2c_remove(struct i2c_client *client)
{
	struct aw87xxx *aw87xxx = i2c_get_clientdata(client);

#ifdef CONFIG_SND_SOC_OPLUS_PA_MANAGER
/*Jin.Liu@MULTIMEDIA.AUDIODRIVER.CODEC,2021/10/19,add PA manager*/
	oplus_speaker_pa_remove(oplus_pa_aw_node);
#endif /*CONFIG_SND_SOC_OPLUS_PA_MANAGER*/

	if (gpio_is_valid(aw87xxx->reset_gpio))
		devm_gpio_free(&client->dev, aw87xxx->reset_gpio);

	list_del(&aw87xxx->list);
	devm_kfree(&client->dev, aw87xxx);
	aw87xxx = NULL;

	return 0;
}

static void aw87xxx_i2c_shutdown(struct i2c_client *client)
{
#ifdef OPLUS_BUG_COMPATIBILITY
	struct aw87xxx *aw87xxx = NULL;

	if (client == NULL) {
		pr_err("%s, client == NULL", __func__);

		return;
	}

	aw87xxx = i2c_get_clientdata(client);
	if (aw87xxx == NULL) {
		pr_warning("%s, aw87xxx == NULL", __func__);

		return;
	}

	pr_warning("%s, %d, aw87xxx = %p, aw87xxx->product = %#x, aw87xxx->chipid = %#x",__func__, __LINE__, aw87xxx, aw87xxx->product, aw87xxx->chipid);

	if (aw87xxx->chipid == AW87XXX_CHIPID_39
		|| aw87xxx->chipid == AW87XXX_CHIPID_59
		|| aw87xxx->chipid == AW87XXX_CHIPID_69
		|| aw87xxx->chipid == AW87XXX_CHIPID_5A) {
		pr_info("%s enter!\n", __func__);
		aw87xxx_audio_off(aw87xxx);
	}
#else
	struct aw87xxx *aw87xxx = i2c_get_clientdata(client);

	pr_info("%s enter!\n", __func__);
	aw87xxx_audio_off(aw87xxx);
#endif
}

static const struct i2c_device_id aw87xxx_i2c_id[] = {
	{AW87XXX_I2C_NAME, 0},
	{}
};

static const struct of_device_id extpa_of_match[] = {
	{.compatible = "awinic,aw87xxx_pa"},
	{},
};

static struct i2c_driver aw87xxx_i2c_driver = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = AW87XXX_I2C_NAME,
		   .of_match_table = extpa_of_match,
		   },
	.probe = aw87xxx_i2c_probe,
	.remove = aw87xxx_i2c_remove,
	.shutdown = aw87xxx_i2c_shutdown,
	.id_table = aw87xxx_i2c_id,
};

static int __init aw87xxx_pa_init(void)
{
	int ret;

	pr_info("%s: driver version: %s\n", __func__,
				AW87XXX_DRIVER_VERSION);

	ret = i2c_add_driver(&aw87xxx_i2c_driver);
	if (ret) {
		pr_info("****[%s] Unable to register driver (%d)\n",
			__func__, ret);
		return ret;
	}
	return 0;
}

static void __exit aw87xxx_pa_exit(void)
{
	pr_info("%s enter\n", __func__);
	i2c_del_driver(&aw87xxx_i2c_driver);
}

module_init(aw87xxx_pa_init);
module_exit(aw87xxx_pa_exit);

MODULE_AUTHOR("<zhaolei@awinic.com>");
MODULE_DESCRIPTION("awinic aw87xxx pa driver");
MODULE_LICENSE("GPL v2");
