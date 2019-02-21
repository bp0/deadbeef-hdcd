/*
 *  Copyright (C) 2016, Burt P. <pburt0@gmail.com>,
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without modification,
 *  are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. The names of its contributors may not be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * HDCD decoding DSP plugin for deadbeef audio player
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <deadbeef/deadbeef.h>
#include <hdcd/hdcd_simple.h>

#define hdcd_msg(fmt, ...) fprintf (stderr, "[%s] " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

enum {
    HDCD_ENABLED             = 0,
    HDCD_PARAM_ANALYZE_MODE  = 1,
    HDCD_PARAM_COUNT         = 2
};

static DB_functions_t *deadbeef;
static DB_dsp_t plugin;

typedef struct {
    ddb_dsp_context_t ctx;

    hdcd_simple *hdcd;
    int amode;
    int enabled;

    uint64_t samples_since_reset;

    int log_detect_data_period;
    int log_detect_data_counter;

    int not_16bit;
} ddb_hdcdcontext_t;

ddb_dsp_context_t*
dsp_hdcd_open (void) {
    ddb_hdcdcontext_t *hdcdctx = malloc (sizeof (ddb_hdcdcontext_t));
    DDB_INIT_DSP_CONTEXT (hdcdctx,ddb_hdcdcontext_t,&plugin);

    // initialize
    hdcdctx->log_detect_data_counter =
        hdcdctx->log_detect_data_period = 441000; // 10 sec.
    hdcdctx->amode = 0; // will be set by config
    hdcdctx->enabled = 1; // will be set by config

    hdcdctx->samples_since_reset = 0;
    hdcdctx->not_16bit = 0;

    hdcdctx->hdcd = hdcd_new();

    return (ddb_dsp_context_t *)hdcdctx;
}

void
dsp_hdcd_close (ddb_dsp_context_t *ctx) {
    ddb_hdcdcontext_t *hdcdctx = (ddb_hdcdcontext_t *)ctx;

    // free instance-specific allocations
    hdcd_free(hdcdctx->hdcd);
    free (hdcdctx);
}

void
dsp_hdcd_reset (ddb_dsp_context_t *ctx) {
    ddb_hdcdcontext_t *hdcdctx = (ddb_hdcdcontext_t *)ctx;
    // use this method to flush dsp buffers, reset filters, etc

    hdcd_reset(hdcdctx->hdcd);
    hdcd_analyze_mode(hdcdctx->hdcd, hdcdctx->amode);
    hdcdctx->samples_since_reset = 0;
    hdcdctx->not_16bit = 0;
}

int
dsp_hdcd_can_bypass (ddb_dsp_context_t *ctx, ddb_waveformat_t *fmt) {
    ddb_hdcdcontext_t *hdcdctx = (ddb_hdcdcontext_t *)ctx;
    if (fmt->channels != 2) return 1;
    if (fmt->samplerate != 44100) return 1;
    if (hdcdctx->not_16bit) return 1;
    return 0;
}

int
dsp_hdcd_process (ddb_dsp_context_t *ctx, float *samples, int nframes, int maxframes, ddb_waveformat_t *fmt, float *r) {
    ddb_hdcdcontext_t *hdcdctx = (ddb_hdcdcontext_t *)ctx;
    int32_t *s32_samples;
    char dstr[256];
    int not_16bit = 0;

    if (fmt->channels != 2) return nframes;
    if (fmt->samplerate != 44100) return nframes;
    if (hdcdctx->not_16bit) return nframes;

    // convert to s16
    s32_samples = malloc(nframes * fmt->channels * sizeof(int32_t));
    if (!s32_samples) return nframes;
    for (int i = 0; i < nframes * fmt->channels; i++) {
        if (samples[i] > 65535 || samples[i] < -65536)
            not_16bit++;
        s32_samples[i] = samples[i] * 0x8000U;
    }

    /* deadbeef converts all input to 32bit float for DSPs, so is_float and
     * bps aren't useful here. Try and detect if there are any sample values
     * outside of 16bit range and then stop decoding HDCD. */

    if (not_16bit) {
        hdcd_msg("not_16bit: %d [%d]", not_16bit, hdcdctx->not_16bit);
        hdcdctx->not_16bit = 1;
        free(s32_samples);
        return nframes;
    }

    if (hdcdctx->enabled) {
        // processing expands into s32
        hdcd_process(hdcdctx->hdcd, s32_samples, nframes);
        hdcdctx->samples_since_reset += nframes * fmt->channels;

        if (hdcdctx->log_detect_data_period) {
            hdcdctx->log_detect_data_counter -= nframes;
            if (hdcdctx->log_detect_data_counter < 0) {
                hdcdctx->log_detect_data_counter = hdcdctx->log_detect_data_period;
                hdcd_detect_str(hdcdctx->hdcd, dstr, sizeof(dstr));
                hdcd_msg("[%" PRIu64 "] %s", hdcdctx->samples_since_reset, dstr);
            }
        }
    } else {
        for (int i = 0; i < nframes * fmt->channels; i++)
            s32_samples[i] <<= 15;
    }

    // convert s32 back to float
    for (int i = 0; i < nframes * fmt->channels; i++) {
        samples[i] = s32_samples[i];
        samples[i] /= 0x80000000U;
    }
    fmt->bps = 32;
    fmt->is_float = 1;

    if ((void *)s32_samples != (void *)samples)
        free(s32_samples);

    return nframes;
}

const char *
dsp_hdcd_get_param_name (int p) {
    switch (p) {
    case HDCD_ENABLED:
        return "Enabled";
    case HDCD_PARAM_ANALYZE_MODE:
        return "Analyze Mode";
    default:
        hdcd_msg("invalid param index (%d)", p);
    }
    return NULL;
}

int
dsp_hdcd_num_params (void) {
    return HDCD_PARAM_COUNT;
}

static const char am_str[8][5] = {
    "off", "lle", "pe", "cdt", "tgm", "pel", "ltgm", "?"
};

void
dsp_hdcd_set_param (ddb_dsp_context_t *ctx, int p, const char *val) {
    ddb_hdcdcontext_t *hdcdctx = (ddb_hdcdcontext_t *)ctx;
    int i;
    switch (p) {
    case HDCD_ENABLED:
        i = atoi (val);
        hdcdctx->enabled = i;
        break;
    case HDCD_PARAM_ANALYZE_MODE:
        i = atoi (val);
        if (i < 0 || i > 6 ) i = 0;
        hdcd_analyze_mode(hdcdctx->hdcd, i);
        hdcdctx->amode = i;
        break;
    default:
        hdcd_msg("invalid param index (%d)", p);
    }
}

void
dsp_hdcd_get_param (ddb_dsp_context_t *ctx, int p, char *val, int sz) {
    ddb_hdcdcontext_t *hdcdctx = (ddb_hdcdcontext_t *)ctx;
    int amode;
    switch (p) {
    case HDCD_ENABLED:
        snprintf (val, sz, "%d", hdcdctx->enabled);
        break;
    case HDCD_PARAM_ANALYZE_MODE:
        amode = hdcdctx->amode;
        if (amode > 7) amode = 7;
        snprintf (val, sz, "%d", amode);
        break;
    default:
        hdcd_msg("invalid param index (%d)", p);
    }
}

static const char settings_dlg[] =
    "property \"Enabled\" checkbox 0 1;\n"
    "property \"Analyze Mode\" select[7] 1 0 off lle pe cdt tgm pel ltgm;\n"
;

static DB_dsp_t plugin = {
    //.plugin.api_vmajor = DB_API_VERSION_MAJOR,
    //.plugin.api_vminor = DB_API_VERSION_MINOR,
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 8,
    .open = dsp_hdcd_open,
    .close = dsp_hdcd_close,
    .process = dsp_hdcd_process,
    .can_bypass = dsp_hdcd_can_bypass,
    .plugin.version_major = 0,
    .plugin.version_minor = 5,
    .plugin.type = DB_PLUGIN_DSP,
    .plugin.id = "hdcd",
    .plugin.name = "HDCD decoder",
    .plugin.descr = "Apply High Definition Compatible Digital (HDCD) decoding.",
    .plugin.copyright = "Burt P, libhdcd AUTHORS",
    .plugin.website = "https://github.com/bp0/deadbeef-hdcd",
    .num_params = dsp_hdcd_num_params,
    .get_param_name = dsp_hdcd_get_param_name,
    .set_param = dsp_hdcd_set_param,
    .get_param = dsp_hdcd_get_param,
    .reset = dsp_hdcd_reset,
    .configdialog = settings_dlg,
};

DB_plugin_t *
ddb_hdcd_load (DB_functions_t *f) {
    deadbeef = f;
    return &plugin.plugin;
}
