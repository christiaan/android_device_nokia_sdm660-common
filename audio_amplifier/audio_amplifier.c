/*
 * SPDX-FileCopyrightText: 2015 The CyanogenMod Project
 * SPDX-FileCopyrightText: 2020-2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "audio_amplifier"
//#define LOG_NDEBUG 0

#include <cutils/properties.h>
#include <cutils/str_parms.h>
#include <dlfcn.h>
#include <errno.h>
#include <hardware/audio_amplifier.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <system/audio.h>
#include <tinyalsa/asoundlib.h>
#include <unistd.h>

/* clang-format off */
#include "platform.h"
#include "platform_api.h"
/* clang-format on */

#define UNUSED __attribute__((unused))

/* Nokia devices: mixer profile, no VI feedback exists */
#define TFA98XX_NOKIA_MIXER_CARD 0
#define TFA98XX_NOKIA_MIXER_CTL "speaker Profile"

/* Settle time before a profile switch, from stock audio hal*/
#define TFA98XX_NOKIA_SWITCH_DELAY_US 20000

/* ro.boot.device from stock audio hal */
static const char* const nokia_devices[] = {
        "PL2", "DRG", "B2N", "CTL", "SS2", "SD1", "HH1", "HH6", "HG1",
};
/* profiles from stock hal */
typedef enum {
    PROFILE_NONE = -1,
    PROFILE_MUSIC = 0,
    PROFILE_VOICE,
    PROFILE_RINGTONE,
    // PROFILE_SYSTEM_SOUND, not used on stock hal
    PROFILE_BYPASS,
    PROFILE_COUNT,
} tfa_profile_t;

static const char* const tfa_profile_names[PROFILE_COUNT] = {
        [PROFILE_MUSIC] = "music",
        [PROFILE_VOICE] = "voice",
        [PROFILE_RINGTONE] = "ringtone",
        // [PROFILE_SYSTEM_SOUND] = "system_sound", not used on stock hal
        [PROFILE_BYPASS] = "bypass",
};

typedef struct amp_device {
    amplifier_device_t amp_dev;
    struct audio_device* adev;
    struct audio_usecase* usecase_tx;
    struct pcm* tfa98xx_out;
    const struct hw_module_t* module_ahal;
    typeof(enable_snd_device)* enable_snd_device;
    typeof(enable_audio_route)* enable_audio_route;
    typeof(disable_snd_device)* disable_snd_device;
    typeof(disable_audio_route)* disable_audio_route;
    typeof(platform_get_pcm_device_id)* platform_get_pcm_device_id;
    typeof(get_usecase_from_list)* get_usecase_from_list;

    /* Nokia mixer profile */
    bool is_fih;
    pthread_mutex_t lock;
    struct mixer* mixer;
    struct mixer_ctl* ctl;
    bool ctl_absent;
    audio_mode_t mode;
    uint32_t out_devices;
    tfa_profile_t active_profile;
} tfa_t;

static tfa_t* tfa_dev = NULL;

static int is_speaker(uint32_t snd_device) {
    int speaker = 0;
    switch (snd_device) {
        case SND_DEVICE_OUT_SPEAKER:
        case SND_DEVICE_OUT_SPEAKER_REVERSE:
        case SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES:
        case SND_DEVICE_OUT_VOICE_SPEAKER:
        case SND_DEVICE_OUT_VOICE_SPEAKER_2:
        case SND_DEVICE_OUT_SPEAKER_AND_HDMI:
        case SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET:
        case SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET:
            speaker = 1;
            break;
    }

    return speaker;
}

/* Nokia Amp without feedback start */
static bool is_fih_device(void) {
    char board[PROPERTY_VALUE_MAX] = {0};
    char smartamp[PROPERTY_VALUE_MAX] = {0};
    bool is_nokia_board = false;

    property_get("ro.boot.device", board, "");
    for (size_t i = 0; i < sizeof(nokia_devices) / sizeof(nokia_devices[0]); i++) {
        if (strcmp(board, nokia_devices[i]) == 0) {
            is_nokia_board = true;
            break;
        }
    }

    if (!is_nokia_board) {
        return false;
    }

    property_get("ro.boot.smartamp", smartamp, "nxp");
    if (strcmp(smartamp, "nxp") == 0) {
        return true;
    } else {
        ALOGI("%s: not a nxp amp (%s)", __func__, smartamp);
        return false;
    }
}

static bool is_speaker_device(uint32_t devices) {
    return (devices & AUDIO_DEVICE_OUT_SPEAKER) != 0;
}
static struct mixer_ctl* tfa98xx_get_ctl(void) {
    if (tfa_dev->ctl) return tfa_dev->ctl;
    if (tfa_dev->ctl_absent) return NULL;

    if (!tfa_dev->mixer) {
        tfa_dev->mixer = mixer_open(TFA98XX_NOKIA_MIXER_CARD);
        if (!tfa_dev->mixer) {
            ALOGW("%s: mixer_open(%d) failed", __func__, TFA98XX_NOKIA_MIXER_CARD);
            return NULL;
        }
    }

    tfa_dev->ctl = mixer_get_ctl_by_name(tfa_dev->mixer, TFA98XX_NOKIA_MIXER_CTL);
    if (!tfa_dev->ctl) {
        ALOGI("%s: '%s' not present, amplifier disabled", __func__, TFA98XX_NOKIA_MIXER_CTL);
        tfa_dev->ctl_absent = true;
        mixer_close(tfa_dev->mixer);
        tfa_dev->mixer = NULL;
    }
    return tfa_dev->ctl;
}

static void tfa98xx_set_profile(tfa_profile_t profile) {
    if (profile <= PROFILE_NONE || profile >= PROFILE_COUNT) return;
    if (tfa_dev->active_profile == profile) return;

    struct mixer_ctl* ctl = tfa98xx_get_ctl();
    if (!ctl) return;

    usleep(TFA98XX_NOKIA_SWITCH_DELAY_US);

    int rc = mixer_ctl_set_enum_by_string(ctl, tfa_profile_names[profile]);
    if (rc != 0) {
        ALOGE("%s: failed to set '%s' to '%s' (%d)", __func__, TFA98XX_NOKIA_MIXER_CTL,
              tfa_profile_names[profile], rc);
        return;
    }

    ALOGI("%s: profile change -- %s", __func__, tfa_profile_names[profile]);
    tfa_dev->active_profile = profile;
}

static void tfa98xx_update_route(void) {
    tfa_profile_t profile;
    char smartamp[PROPERTY_VALUE_MAX];

    if (!is_speaker_device(tfa_dev->out_devices)) {
        tfa_dev->active_profile = PROFILE_NONE;
        return;
    }

    switch (tfa_dev->mode) {
        case AUDIO_MODE_IN_CALL:
        case AUDIO_MODE_IN_COMMUNICATION:
        case AUDIO_MODE_CALL_SCREEN:
            profile = PROFILE_VOICE;
            break;
        case AUDIO_MODE_RINGTONE:
            profile = PROFILE_RINGTONE;
            break;
        case AUDIO_MODE_NORMAL:
            property_get("persist.sys.smartamp", smartamp, "1");
            profile = strcmp(smartamp, "1") == 0 ? PROFILE_MUSIC : PROFILE_BYPASS;
            break;
        default:
            ALOGW("%s: no profile for mode %d, leaving", __func__, tfa_dev->mode);
            return;
    }

    tfa98xx_set_profile(profile);
}

static int amp_set_mode(UNUSED amplifier_device_t* device, audio_mode_t mode) {
    if (!tfa_dev) return -EINVAL;

    pthread_mutex_lock(&tfa_dev->lock);
    tfa_dev->mode = mode;
    tfa98xx_update_route();
    pthread_mutex_unlock(&tfa_dev->lock);
    return 0;
}

static int amp_set_output_devices(UNUSED amplifier_device_t* device, uint32_t devices) {
    if (!tfa_dev) return -EINVAL;

    pthread_mutex_lock(&tfa_dev->lock);
    tfa_dev->out_devices = devices;
    tfa98xx_update_route();
    pthread_mutex_unlock(&tfa_dev->lock);
    return 0;
}
/* Nokia Amp without feedback end */

static int tfa98xx_start_feedback(void* adev, uint32_t snd_device) {
    tfa_dev->adev = (struct audio_device*)adev;
    int pcm_dev_tx_id = 0, rc = 0;
    struct pcm_config pcm_config_tfa98xx = {
            .channels = 2,
            .rate = 48000,
            .period_size = 256,
            .period_count = 4,
            .format = PCM_FORMAT_S16_LE,
            .start_threshold = 0,
            .stop_threshold = INT_MAX,
            .silence_threshold = 0,
    };

    if (!tfa_dev) {
        ALOGE("%d: Invalid params", __LINE__);
        return -EINVAL;
    }

    if (tfa_dev->tfa98xx_out || !is_speaker(snd_device)) return 0;

    tfa_dev->usecase_tx = (struct audio_usecase*)calloc(1, sizeof(struct audio_usecase));
    if (!tfa_dev->usecase_tx) {
        ALOGE("%d: failed to allocate usecase", __LINE__);
        return -ENOMEM;
    }
    tfa_dev->usecase_tx->id = USECASE_AUDIO_SPKR_CALIB_TX;
    tfa_dev->usecase_tx->type = PCM_CAPTURE;
    tfa_dev->usecase_tx->in_snd_device = SND_DEVICE_IN_CAPTURE_VI_FEEDBACK;
#if __has_include("device_utils.h")
    list_init(&tfa_dev->usecase_tx->device_list);
#endif

    list_add_tail(&tfa_dev->adev->usecase_list, &tfa_dev->usecase_tx->list);
    tfa_dev->enable_snd_device(tfa_dev->adev, tfa_dev->usecase_tx->in_snd_device);
    tfa_dev->enable_audio_route(tfa_dev->adev, tfa_dev->usecase_tx);

    pcm_dev_tx_id =
            tfa_dev->platform_get_pcm_device_id(tfa_dev->usecase_tx->id, tfa_dev->usecase_tx->type);
    ALOGD("pcm_dev_tx_id = %d", pcm_dev_tx_id);
    if (pcm_dev_tx_id < 0) {
        ALOGE("%d: Invalid pcm device for usecase (%d)", __LINE__, tfa_dev->usecase_tx->id);
        rc = -ENODEV;
        goto error;
    }

    tfa_dev->tfa98xx_out =
            pcm_open(tfa_dev->adev->snd_card, pcm_dev_tx_id, PCM_IN, &pcm_config_tfa98xx);
    if (!pcm_is_ready(tfa_dev->tfa98xx_out)) {
        ALOGE("%d: %s", __LINE__, pcm_get_error(tfa_dev->tfa98xx_out));
        rc = -EIO;
        goto error;
    }

    rc = pcm_start(tfa_dev->tfa98xx_out);
    if (rc < 0) {
        ALOGE("%d: pcm start for TX failed", __LINE__);
        rc = -EINVAL;
        goto error;
    }
    return 0;

error:
    ALOGE("%s: error case", __func__);
    if (tfa_dev->tfa98xx_out != 0) {
        pcm_close(tfa_dev->tfa98xx_out);
        tfa_dev->tfa98xx_out = NULL;
    }
    list_remove(&tfa_dev->usecase_tx->list);
    tfa_dev->disable_snd_device(tfa_dev->adev, tfa_dev->usecase_tx->in_snd_device);
    tfa_dev->disable_audio_route(tfa_dev->adev, tfa_dev->usecase_tx);
    free(tfa_dev->usecase_tx);

    return rc;
}

static void tfa98xx_stop_feedback(void* adev, uint32_t snd_device) {
    tfa_dev->adev = (struct audio_device*)adev;
    if (!tfa_dev) {
        ALOGE("%s: Invalid params", __func__);
        return;
    }

    if (!is_speaker(snd_device)) return;

    if (tfa_dev->tfa98xx_out) {
        pcm_close(tfa_dev->tfa98xx_out);
        tfa_dev->tfa98xx_out = NULL;
    }

    tfa_dev->disable_snd_device(tfa_dev->adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);

    tfa_dev->usecase_tx =
            tfa_dev->get_usecase_from_list(tfa_dev->adev, USECASE_AUDIO_SPKR_CALIB_TX);
    if (tfa_dev->usecase_tx) {
        list_remove(&tfa_dev->usecase_tx->list);
        tfa_dev->disable_audio_route(tfa_dev->adev, tfa_dev->usecase_tx);
        free(tfa_dev->usecase_tx);
    }
    return;
}

static int amp_set_feedback(UNUSED amplifier_device_t* device, void* adev, uint32_t devices,
                            bool enable) {
    /* Nokia devices don't use VI feedback, stock hal never sets it */
    if (tfa_dev->is_fih) return 0;

    tfa_dev->adev = (struct audio_device*)adev;
    if (enable) {
        tfa98xx_start_feedback(tfa_dev->adev, devices);
    } else {
        tfa98xx_stop_feedback(tfa_dev->adev, devices);
    }
    return 0;
}

static int amp_dev_close(hw_device_t* device) {
    tfa_t* dev = (tfa_t*)device;
    if (dev) {
        if (dev->is_fih) {
            if (dev->mixer) mixer_close(dev->mixer);
            pthread_mutex_destroy(&dev->lock);
        }
        free(dev);
    }

    return 0;
}

static int amp_module_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (strcmp(name, AMPLIFIER_HARDWARE_INTERFACE)) {
        ALOGE("%s:%d: %s does not match amplifier hardware interface name\n", __func__, __LINE__,
              name);
        return -ENODEV;
    }

    tfa_dev = calloc(1, sizeof(tfa_t));
    if (!tfa_dev) {
        ALOGE("%s:%d: Unable to allocate memory for amplifier device\n", __func__, __LINE__);
        return -ENOMEM;
    }

    tfa_dev->amp_dev.common.tag = HARDWARE_DEVICE_TAG;
    tfa_dev->amp_dev.common.module = (hw_module_t*)module;
    tfa_dev->amp_dev.common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    tfa_dev->amp_dev.common.close = amp_dev_close;

    tfa_dev->amp_dev.set_input_devices = NULL;
    tfa_dev->amp_dev.set_output_devices = NULL;
    tfa_dev->amp_dev.enable_output_devices = NULL;
    tfa_dev->amp_dev.enable_input_devices = NULL;
    tfa_dev->amp_dev.set_mode = NULL;
    tfa_dev->amp_dev.output_stream_start = NULL;
    tfa_dev->amp_dev.input_stream_start = NULL;
    tfa_dev->amp_dev.output_stream_standby = NULL;
    tfa_dev->amp_dev.input_stream_standby = NULL;
    tfa_dev->amp_dev.set_parameters = NULL;
    tfa_dev->amp_dev.out_set_parameters = NULL;
    tfa_dev->amp_dev.in_set_parameters = NULL;
    tfa_dev->amp_dev.set_feedback = amp_set_feedback;

    tfa_dev->is_fih = is_fih_device();
    if (tfa_dev->is_fih) {
        pthread_mutex_init(&tfa_dev->lock, NULL);
        tfa_dev->mode = AUDIO_MODE_NORMAL;
        tfa_dev->active_profile = PROFILE_NONE;
        tfa_dev->amp_dev.set_output_devices = amp_set_output_devices;
        tfa_dev->amp_dev.set_mode = amp_set_mode;

        *device = (hw_device_t*)tfa_dev;
        return 0;
    }

    if (hw_get_module_by_class(AUDIO_HARDWARE_MODULE_ID, AUDIO_HARDWARE_MODULE_ID_PRIMARY,
                               &tfa_dev->module_ahal)) {
        ALOGW("%s: Failed to load audio.primary", __func__);
        return -ENODEV;
    }

#define LOAD_AHAL_SYMBOL(symbol)                                          \
    do {                                                                  \
        tfa_dev->symbol = dlsym(tfa_dev->module_ahal->dso, #symbol);      \
        if (tfa_dev->symbol == NULL) {                                    \
            ALOGW("%s: %s not found (%s)", __func__, #symbol, dlerror()); \
            free(tfa_dev);                                                \
            return -ENODEV;                                               \
        }                                                                 \
    } while (0)

    LOAD_AHAL_SYMBOL(enable_snd_device);
    LOAD_AHAL_SYMBOL(enable_audio_route);
    LOAD_AHAL_SYMBOL(disable_snd_device);
    LOAD_AHAL_SYMBOL(disable_audio_route);
    LOAD_AHAL_SYMBOL(platform_get_pcm_device_id);
    LOAD_AHAL_SYMBOL(get_usecase_from_list);

#undef LOAD_AHAL_SYMBOL

    *device = (hw_device_t*)tfa_dev;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
        .open = amp_module_open,
};

/* clang-format off */
amplifier_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AMPLIFIER_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AMPLIFIER_HARDWARE_MODULE_ID,
        .name = "Nokia SDM660 TFA98XX audio amplifier HAL",
        .author = "The LineageOS Open Source Project",
        .methods = &hal_module_methods,
    },
};
