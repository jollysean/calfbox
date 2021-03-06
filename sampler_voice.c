/*
Calf Box, an open source musical instrument.
Copyright (C) 2010-2013 Krzysztof Foltman

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "config-api.h"
#include "dspmath.h"
#include "errors.h"
#include "midi.h"
#include "module.h"
#include "rt.h"
#include "sampler.h"
#include "sampler_impl.h"
#include "sfzloader.h"
#include "stm.h"
#include <assert.h>
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <memory.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>


static void lfo_update_freq(struct sampler_lfo *lfo, struct sampler_lfo_params *lfop, int srate, double srate_inv)
{
    lfo->delta = (uint32_t)(lfop->freq * 65536.0 * 65536.0 * CBOX_BLOCK_SIZE * srate_inv);
    lfo->delay = (uint32_t)(lfop->delay * srate);
    lfo->fade = (uint32_t)(lfop->fade * srate);
}

static void lfo_init(struct sampler_lfo *lfo, struct sampler_lfo_params *lfop, int srate, double srate_inv)
{
    lfo->phase = 0;
    lfo->age = 0;
    lfo_update_freq(lfo, lfop, srate, srate_inv);
}

static inline float lfo_run(struct sampler_lfo *lfo)
{
    if (lfo->age < lfo->delay)
    {
        lfo->age += CBOX_BLOCK_SIZE;
        return 0.f;
    }

    const int FRAC_BITS = 32 - 11;
    lfo->phase += lfo->delta;
    uint32_t iphase = lfo->phase >> FRAC_BITS;
    float frac = (lfo->phase & ((1 << FRAC_BITS) - 1)) * (1.0 / (1 << FRAC_BITS));

    float v = sampler_sine_wave[iphase] + (sampler_sine_wave[iphase + 1] - sampler_sine_wave[iphase]) * frac;
    if (lfo->fade && lfo->age < lfo->delay + lfo->fade)
    {
        v *= (lfo->age - lfo->delay) * 1.0 / lfo->fade;
        lfo->age += CBOX_BLOCK_SIZE;
    }

    return v;
}

static gboolean is_tail_finished(struct sampler_voice *v)
{
    if (v->layer->cutoff == -1)
        return TRUE;
    double eps = 1.0 / 65536.0;
    if (cbox_biquadf_is_audible(&v->filter_left, eps))
        return FALSE;
    if (cbox_biquadf_is_audible(&v->filter_right, eps))
        return FALSE;
    if (sampler_layer_data_is_4pole(v->layer))
    {
        if (cbox_biquadf_is_audible(&v->filter_left2, eps))
            return FALSE;
        if (cbox_biquadf_is_audible(&v->filter_right2, eps))
            return FALSE;
    }
    
    return TRUE;
}

#if USE_NEON

#include <arm_neon.h>

static inline void mix_block_into_with_gain(cbox_sample_t **outputs, int oofs, float *src_leftright, float gain)
{
    float *dst_left = outputs[oofs];
    float *dst_right = outputs[oofs + 1];
    float32x2_t gain2 = {gain, gain};
    for (size_t i = 0; i < CBOX_BLOCK_SIZE; i += 2)
    {
        float32x2_t lr1 = vld1_f32(&src_leftright[2 * i]);
        float32x2_t lr2 = vld1_f32(&src_leftright[2 * i + 2]);
        float32x2x2_t lr12 = vtrn_f32(lr1, lr2);
        float32x2_t dl1 = vld1_f32(&dst_left[i]);
        float32x2_t dr1 = vld1_f32(&dst_right[i]);
        
        float32x2_t l1 = vmla_f32(dl1, lr12.val[0], gain2);
        vst1_f32(&dst_left[i], l1);
        float32x2_t r1 = vmla_f32(dr1, lr12.val[1], gain2);
        vst1_f32(&dst_right[i], r1);
    }
}

static inline void mix_block_into(cbox_sample_t **outputs, int oofs, float *src_leftright)
{
    float *dst_left = outputs[oofs];
    float *dst_right = outputs[oofs + 1];
    for (size_t i = 0; i < CBOX_BLOCK_SIZE; i += 2)
    {
        float32x2_t lr1 = vld1_f32(&src_leftright[2 * i]);
        float32x2_t lr2 = vld1_f32(&src_leftright[2 * i + 2]);
        float32x2x2_t lr12 = vtrn_f32(lr1, lr2);
        float32x2_t dl1 = vld1_f32(&dst_left[i]);
        float32x2_t dr1 = vld1_f32(&dst_right[i]);
        
        float32x2_t l1 = vadd_f32(dl1, lr12.val[0]);
        vst1_f32(&dst_left[i], l1);
        float32x2_t r1 = vadd_f32(dr1, lr12.val[1]);
        vst1_f32(&dst_right[i], r1);
    }
}

#else

static inline void mix_block_into_with_gain(cbox_sample_t **outputs, int oofs, float *src_leftright, float gain)
{
    cbox_sample_t *dst_left = outputs[oofs];
    cbox_sample_t *dst_right = outputs[oofs + 1];
    for (size_t i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        dst_left[i] += gain * src_leftright[2 * i];
        dst_right[i] += gain * src_leftright[2 * i + 1];
    }
}

static inline void mix_block_into(cbox_sample_t **outputs, int oofs, float *src_leftright)
{
    cbox_sample_t *dst_left = outputs[oofs];
    cbox_sample_t *dst_right = outputs[oofs + 1];
    for (size_t i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        dst_left[i] += src_leftright[2 * i];
        dst_right[i] += src_leftright[2 * i + 1];
    }
}

#endif

////////////////////////////////////////////////////////////////////////////////

void sampler_voice_activate(struct sampler_voice *v, enum sampler_player_type mode)
{
    assert(v->gen.mode == spt_inactive);
    sampler_voice_unlink(&v->program->module->voices_free, v);
    assert(mode != spt_inactive);
    assert(v->channel);
    v->gen.mode = mode;
    sampler_voice_link(&v->channel->voices_running, v);
}

void sampler_voice_start(struct sampler_voice *v, struct sampler_channel *c, struct sampler_layer_data *l, int note, int vel, int *exgroups, int *pexgroupcount)
{
    struct sampler_module *m = c->module;
    sampler_gen_reset(&v->gen);
    
    v->age = 0;
    if (l->trigger == stm_release)
    {
        // time since last 'note on' for that note
        v->age = m->current_time - c->prev_note_start_time[note];
        double age = v->age *  m->module.srate_inv;
        // if attenuation is more than 84dB, ignore the release trigger
        if (age * l->rt_decay > 84)
            return;
    }
    uint32_t end = l->eff_waveform->info.frames;
    if (l->end != 0)
        end = (l->end == -1) ? 0 : l->end;
    v->last_waveform = l->eff_waveform;
    v->gen.cur_sample_end = end;
    if (end > l->eff_waveform->info.frames)
        end = l->eff_waveform->info.frames;
    
    assert(!v->current_pipe);
    if (end > l->eff_waveform->preloaded_frames)
    {
        if (l->eff_loop_mode == slm_loop_continuous && l->loop_end < l->eff_waveform->preloaded_frames)
        {
            // Everything fits in prefetch, because loop ends in prefetch and post-loop part is not being played
        }
        else
        {
            uint32_t loop_start = -1, loop_end = end;
            // If in loop mode, set the loop over the looped part... unless we're doing sustain-only loop on prefetch area only. Then
            // streaming will only cover the release part, and it shouldn't be looped.
            if (l->eff_loop_mode == slm_loop_continuous || (l->eff_loop_mode == slm_loop_sustain && l->loop_end >= l->eff_waveform->preloaded_frames))
            {
                loop_start = l->loop_start;
                loop_end = l->loop_end;
            }
            // Those are initial values only, they will be adjusted in process function
            v->current_pipe = cbox_prefetch_stack_pop(m->pipe_stack, l->eff_waveform, loop_start, loop_end, l->count);
            if (!v->current_pipe)
            {
                g_warning("Prefetch pipe pool exhausted, no streaming playback will be possible");
                end = l->eff_waveform->preloaded_frames;
                v->gen.cur_sample_end = end;
            }
        }
    }
    
    v->output_pair_no = (l->output + c->output_shift) % m->output_pairs;
    v->serial_no = m->serial_no;
    
    float delay = l->delay;
    if (l->delay_random)
        delay += rand() * (1.0 / RAND_MAX) * l->delay_random;
    if (delay > 0)
        v->delay = (int)(delay * m->module.srate);
    else
        v->delay = 0;
    v->gen.loop_overlap = l->loop_overlap;
    v->gen.loop_overlap_step = l->loop_overlap > 0 ? 1.0 / l->loop_overlap : 0;
    v->gain_fromvel = 1.0 + (l->eff_velcurve[vel] - 1.0) * l->amp_veltrack * 0.01;
    v->gain_shift = 0.0;
    v->note = note;
    v->vel = vel;
    v->pitch_shift = 0;
    v->released = 0;
    v->released_with_sustain = 0;
    v->released_with_sostenuto = 0;
    v->captured_sostenuto = 0;
    v->channel = c;
    v->layer = l;
    v->program = c->program;
    v->amp_env.shape = &l->amp_env_shape;
    v->filter_env.shape = &l->filter_env_shape;
    v->pitch_env.shape = &l->pitch_env_shape;
    
    v->cutoff_shift = vel * l->fil_veltrack / 127.0 + (note - l->fil_keycenter) * l->fil_keytrack;
    v->loop_mode = l->eff_loop_mode;
    v->off_by = l->off_by;
    v->reloffset = l->reloffset;
    int auxes = (m->module.outputs - m->module.aux_offset) / 2;
    if (l->effect1bus >= 1 && l->effect1bus < 1 + auxes)
        v->send1bus = l->effect1bus;
    else
        v->send1bus = 0;
    if (l->effect2bus >= 1 && l->effect2bus < 1 + auxes)
        v->send2bus = l->effect2bus;
    else
        v->send2bus = 0;
    v->send1gain = l->effect1 * 0.01;
    v->send2gain = l->effect2 * 0.01;
    if (l->group >= 1 && *pexgroupcount < MAX_RELEASED_GROUPS)
    {
        gboolean found = FALSE;
        for (int j = 0; j < *pexgroupcount; j++)
        {
            if (exgroups[j] == l->group)
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
        {
            exgroups[(*pexgroupcount)++] = l->group;
        }
    }
    lfo_init(&v->amp_lfo, &l->amp_lfo, m->module.srate, m->module.srate_inv);
    lfo_init(&v->filter_lfo, &l->filter_lfo, m->module.srate, m->module.srate_inv);
    lfo_init(&v->pitch_lfo, &l->pitch_lfo, m->module.srate, m->module.srate_inv);
    
    cbox_biquadf_reset(&v->filter_left);
    cbox_biquadf_reset(&v->filter_right);
    cbox_biquadf_reset(&v->filter_left2);
    cbox_biquadf_reset(&v->filter_right2);
    cbox_onepolef_reset(&v->onepole_left);
    cbox_onepolef_reset(&v->onepole_right);
    // set gain later (it's a less expensive operation)
    if (l->tonectl_freq != 0)
        cbox_onepolef_set_highshelf_tonectl(&v->onepole_coeffs, l->tonectl_freq * M_PI * m->module.srate_inv, 1.0);
    
    GSList *nif = v->layer->nifs;
    while(nif)
    {
        struct sampler_noteinitfunc *p = nif->data;
        p->notefunc(p, v);
        nif = nif->next;
    }
    v->offset = l->offset;
    if (v->reloffset != 0)
    {
        uint32_t maxend = v->current_pipe ? (l->eff_waveform->preloaded_frames >> 1) : l->eff_waveform->preloaded_frames;
        int32_t pos = v->offset + v->reloffset * maxend * 0.01;
        if (pos < 0)
            pos = 0;
        if (pos > maxend)
            pos = maxend;
        v->offset = pos;
    }
    
    cbox_envelope_reset(&v->amp_env);
    cbox_envelope_reset(&v->filter_env);
    cbox_envelope_reset(&v->pitch_env);

    v->last_eq_bitmask = 0;

    sampler_voice_activate(v, l->eff_waveform->info.channels == 2 ? spt_stereo16 : spt_mono16);
    
    uint32_t pos = v->offset;
    if (l->offset_random)
        pos += ((uint32_t)(rand() + (rand() << 16))) % l->offset_random;
    if (pos >= end)
        pos = end;
    v->gen.bigpos = ((uint64_t)pos) << 32;
    v->gen.virtpos = ((uint64_t)pos) << 32;
    
    if (v->current_pipe && v->gen.bigpos)
        cbox_prefetch_pipe_consumed(v->current_pipe, v->gen.bigpos >> 32);
    v->layer_changed = TRUE;
}

void sampler_voice_link(struct sampler_voice **pv, struct sampler_voice *v)
{
    v->prev = NULL;
    v->next = *pv;
    if (*pv)
        (*pv)->prev = v;
    *pv = v;
}

void sampler_voice_unlink(struct sampler_voice **pv, struct sampler_voice *v)
{
    if (*pv == v)
        *pv = v->next;
    if (v->prev)
        v->prev->next = v->next;
    if (v->next)
        v->next->prev = v->prev;
    v->prev = NULL;
    v->next = NULL;
}

void sampler_voice_inactivate(struct sampler_voice *v, gboolean expect_active)
{
    assert((v->gen.mode != spt_inactive) == expect_active);
    sampler_voice_unlink(&v->channel->voices_running, v);
    v->gen.mode = spt_inactive;
    if (v->current_pipe)
    {
        cbox_prefetch_stack_push(v->program->module->pipe_stack, v->current_pipe);
        v->current_pipe = NULL;
    }
    v->channel = NULL;
    sampler_voice_link(&v->program->module->voices_free, v);
}

void sampler_voice_release(struct sampler_voice *v, gboolean is_polyaft)
{
    if ((v->loop_mode == slm_one_shot_chokeable) != is_polyaft)
        return;
    if (v->delay >= v->age + CBOX_BLOCK_SIZE)
    {
        v->released = 1;
        sampler_voice_inactivate(v, TRUE);
    }
    else
    {
        if (v->loop_mode != slm_one_shot && !v->layer->count)
        {
            v->released = 1;
            if (v->loop_mode == slm_loop_sustain && v->current_pipe)
            {
                // Break the loop
                v->current_pipe->file_loop_end = v->gen.cur_sample_end;
                v->current_pipe->file_loop_start = -1;
            }
        }
    }
}

void sampler_voice_update_params_from_layer(struct sampler_voice *v)
{
    struct sampler_layer_data *l = v->layer;
    struct sampler_module *m = v->program->module;
    lfo_update_freq(&v->amp_lfo, &l->amp_lfo, m->module.srate, m->module.srate_inv);
    lfo_update_freq(&v->filter_lfo, &l->filter_lfo, m->module.srate, m->module.srate_inv);
    lfo_update_freq(&v->pitch_lfo, &l->pitch_lfo, m->module.srate, m->module.srate_inv);
    cbox_envelope_update_shape(&v->amp_env, &l->amp_env_shape);
    cbox_envelope_update_shape(&v->filter_env, &l->filter_env_shape);
    cbox_envelope_update_shape(&v->pitch_env, &l->pitch_env_shape);
}

void sampler_voice_process(struct sampler_voice *v, struct sampler_module *m, cbox_sample_t **outputs)
{
    struct sampler_layer_data *l = v->layer;
    assert(v->gen.mode != spt_inactive);
    
    // if it's a DAHD envelope without sustain, consider the note finished
    if (__builtin_expect(v->amp_env.cur_stage == 4 && v->amp_env.shape->stages[3].end_value <= 0.f, 0))
        cbox_envelope_go_to(&v->amp_env, 15);                

    struct sampler_channel *c = v->channel;
    v->age += CBOX_BLOCK_SIZE;
    
    if (__builtin_expect(v->age < v->delay, 0))
        return;

    // XXXKF I'm sacrificing sample accuracy for delays for now
    v->delay = 0;
    const float velscl = v->vel * (1.f / 127.f);
    if (__builtin_expect(v->layer_changed, 0))
    {
        v->last_level = -1;
        if (v->last_waveform != v->layer->eff_waveform)
        {
            v->last_waveform = v->layer->eff_waveform;
            if (v->layer->eff_waveform)
            {
                v->gen.mode = v->layer->eff_waveform->info.channels == 2 ? spt_stereo16 : spt_mono16;
                v->gen.cur_sample_end = v->layer->eff_waveform->info.frames;
            }
            else
            {
                sampler_voice_inactivate(v, TRUE);
                return;
            }        
        }
    #define RECALC_EQ_IF(index) \
        if (l->eq_bitmask & (1 << (index - 1))) \
        { \
            cbox_biquadf_set_peakeq_rbj_scaled(&v->eq_coeffs[index - 1], l->eq##index.effective_freq + velscl * l->eq##index.vel2freq, 1.0 / l->eq##index.bw, dB2gain(0.5 * (l->eq##index.gain + velscl * l->eq##index.vel2gain)), m->module.srate); \
            if (!(v->last_eq_bitmask & (1 << (index - 1)))) \
            { \
                cbox_biquadf_reset(&v->eq_left[index-1]); \
                cbox_biquadf_reset(&v->eq_right[index-1]); \
            } \
        }

        RECALC_EQ_IF(1)
        RECALC_EQ_IF(2)
        RECALC_EQ_IF(3)
        v->last_eq_bitmask = l->eq_bitmask;
        v->layer_changed = FALSE;
    }
    
    float pitch = (v->note - l->pitch_keycenter) * l->pitch_keytrack + l->tune + l->transpose * 100 + v->pitch_shift;
    float modsrcs[smsrc_pernote_count];
    modsrcs[smsrc_vel - smsrc_pernote_offset] = v->vel * velscl;
    modsrcs[smsrc_pitch - smsrc_pernote_offset] = pitch * (1.f / 100.f);
    modsrcs[smsrc_polyaft - smsrc_pernote_offset] = 0.f; // XXXKF not supported yet
    modsrcs[smsrc_pitchenv - smsrc_pernote_offset] = cbox_envelope_get_next(&v->pitch_env, v->released) * 0.01f;
    modsrcs[smsrc_filenv - smsrc_pernote_offset] = cbox_envelope_get_next(&v->filter_env, v->released) * 0.01f;
    modsrcs[smsrc_ampenv - smsrc_pernote_offset] = cbox_envelope_get_next(&v->amp_env, v->released) * 0.01f;

    modsrcs[smsrc_amplfo - smsrc_pernote_offset] = lfo_run(&v->amp_lfo);
    modsrcs[smsrc_fillfo - smsrc_pernote_offset] = lfo_run(&v->filter_lfo);
    modsrcs[smsrc_pitchlfo - smsrc_pernote_offset] = lfo_run(&v->pitch_lfo);
    
    if (__builtin_expect(v->amp_env.cur_stage < 0, 0))
    {
        if (__builtin_expect(is_tail_finished(v), 0))
        {
            sampler_voice_inactivate(v, TRUE);
            return;
        }
    }
    
    float moddests[smdestcount];
    moddests[smdest_gain] = 0;
    moddests[smdest_pitch] = pitch;
    moddests[smdest_cutoff] = v->cutoff_shift;
    moddests[smdest_resonance] = 0;
    moddests[smdest_tonectl] = 0;
    GSList *mod = l->modulations;
    if (__builtin_expect(l->trigger == stm_release, 0))
        moddests[smdest_gain] -= v->age * l->rt_decay * m->module.srate_inv;
    
    if (c->pitchwheel)
        moddests[smdest_pitch] += c->pitchwheel * (c->pitchwheel > 0 ? l->bend_up : l->bend_down) >> 13;
    
    static const int modoffset[4] = {0, -1, -1, 1 };
    static const int modscale[4] = {1, 1, 2, -2 };
    while(mod)
    {
        struct sampler_modulation *sm = mod->data;
        float value = 0.f, value2 = 1.f;
        if (sm->src < smsrc_pernote_offset)
            value = c->cc[sm->src] * (1.f / 127.f);
        else
            value = modsrcs[sm->src - smsrc_pernote_offset];
        value = modoffset[sm->flags & 3] + value * modscale[sm->flags & 3];

        if (sm->src2 != smsrc_none)
        {
            if (sm->src2 < smsrc_pernote_offset)
                value2 = c->cc[sm->src2] * (1.f / 127.f);
            else
                value2 = modsrcs[sm->src2 - smsrc_pernote_offset];
            
            value2 = modoffset[(sm->flags & 12) >> 2] + value2 * modscale[(sm->flags & 12) >> 2];
            value *= value2;
        }
        moddests[sm->dest] += value * sm->amount;
        
        mod = g_slist_next(mod);
    }
    
    double maxv = 127 << 7;
    double freq = l->eff_freq * cent2factor(moddests[smdest_pitch]) ;
    uint64_t freq64 = (uint64_t)(freq * 65536.0 * 65536.0 * m->module.srate_inv);

    gboolean playing_sustain_loop = !v->released && v->loop_mode == slm_loop_sustain;
    uint32_t loop_start, loop_end;
    gboolean bandlimited = FALSE;

    if (!v->current_pipe)
    {
        v->gen.sample_data = v->last_waveform->data;
        if (v->last_waveform->levels)
        {
            gboolean use_cached = v->last_level > 0 && v->last_level < v->last_waveform->level_count
                && freq64 > v->last_level_min_rate && freq64 <= v->last_waveform->levels[v->last_level].max_rate;
            if (__builtin_expect(use_cached, 1))
            {
                v->gen.sample_data = v->last_waveform->levels[v->last_level].data;
                bandlimited = TRUE;
            }
            else
            {
                for (int i = 0; i < v->last_waveform->level_count; i++)
                {
                    if (freq64 <= v->last_waveform->levels[i].max_rate)
                    {
                        v->last_level = i;
                        v->gen.sample_data = v->last_waveform->levels[i].data;
                        bandlimited = TRUE;
                        
                        break;
                    }
                    v->last_level_min_rate = v->last_waveform->levels[i].max_rate;
                }
            }
        }
    }
    
    gboolean play_loop = v->layer->loop_end && (v->loop_mode == slm_loop_continuous || playing_sustain_loop) && v->layer->on_cc_number == -1;
    loop_start = play_loop ? v->layer->loop_start : (v->layer->count ? 0 : (uint32_t)-1);
    loop_end = play_loop ? v->layer->loop_end : v->gen.cur_sample_end;

    if (v->current_pipe)
    {
        v->gen.sample_data = v->gen.loop_count ? v->current_pipe->data : v->last_waveform->data;
        v->gen.streaming_buffer = v->current_pipe->data;
        
        v->gen.prefetch_only_loop = (loop_end < v->last_waveform->preloaded_frames);
        v->gen.loop_overlap = 0;
        if (v->gen.prefetch_only_loop)
        {
            assert(!v->gen.in_streaming_buffer); // XXXKF this won't hold true when loops are edited while sound is being played (but that's not supported yet anyway)
            v->gen.loop_start = loop_start;
            v->gen.loop_end = loop_end;
            v->gen.streaming_buffer_frames = 0;
        }
        else
        {
            v->gen.loop_start = 0;
            v->gen.loop_end = v->last_waveform->preloaded_frames;
            v->gen.streaming_buffer_frames = v->current_pipe->buffer_loop_end;
        }
    }
    else
    {
        v->gen.loop_count = v->layer->count;
        v->gen.loop_start = loop_start;
        v->gen.loop_end = loop_end;
        
        if (!bandlimited)
        {
            // Use pre-calculated join
            v->gen.scratch = loop_start == (uint32_t)-1 ? v->layer->scratch_end : v->layer->scratch_loop;
        }
        else
        {
            // The standard waveforms have extra MAX_INTERPOLATION_ORDER of samples from the loop start added past loop_end,
            // to avoid wasting time generating the joins in all the practical cases. The slow path covers custom loops
            // (i.e. partial loop or no loop) over bandlimited versions of the standard waveforms, and those are probably
            // not very useful anyway, as changing the loop removes the guarantee of the waveform being bandlimited and
            // may cause looping artifacts or introduce DC offset (e.g. if only a positive part of a sine wave is looped).
            if (loop_start == 0 && loop_end == l->eff_waveform->info.frames)
                v->gen.scratch = v->gen.sample_data + l->eff_waveform->info.frames - MAX_INTERPOLATION_ORDER;
            else
            {
                // Generate the join for the current wave level
                // XXXKF this could be optimised further, by checking if waveform and loops are the same as the last
                // time. However, this code is not likely to be used... ever, so optimising it is not the priority.
                int shift = l->eff_waveform->info.channels == 2 ? 1 : 0;
                uint32_t halfscratch = MAX_INTERPOLATION_ORDER << shift;
                
                v->gen.scratch = v->gen.scratch_bandlimited;
                memcpy(&v->gen.scratch_bandlimited[0], &v->gen.sample_data[(loop_end - MAX_INTERPOLATION_ORDER) << shift], halfscratch * sizeof(int16_t) );
                if (loop_start != (uint32_t)-1)
                    memcpy(v->gen.scratch_bandlimited + halfscratch, &v->gen.sample_data[loop_start << shift], halfscratch * sizeof(int16_t));
                else
                    memset(v->gen.scratch_bandlimited + halfscratch, 0, halfscratch * sizeof(int16_t));
            }
        }
    }
        
    if (l->timestretch)
    {
        v->gen.bigdelta = freq64;
        v->gen.virtdelta = (uint64_t)(l->eff_freq * 65536.0 * 65536.0 * m->module.srate_inv);
        v->gen.stretching_jump = l->timestretch_jump;
        v->gen.stretching_crossfade = l->timestretch_crossfade;
    }
    else
    {
        v->gen.bigdelta = freq64;
        v->gen.virtdelta = freq64;
    }
    float gain = modsrcs[smsrc_ampenv - smsrc_pernote_offset] * l->volume_linearized * v->gain_fromvel * c->channel_volume_cc * sampler_channel_addcc(c, 11) / (maxv * maxv);
    if (moddests[smdest_gain] != 0.f)
        gain *= dB2gain(moddests[smdest_gain]);
    // http://drealm.info/sfz/plj-sfz.xhtml#amp "The overall gain must remain in the range -144 to 6 decibels."
    if (gain > 2.f)
        gain = 2.f;
    float pan = (l->pan + 100.f) * (1.f / 200.f) + (c->channel_pan_cc * 1.f / maxv - 0.5f) * 2.f;
    if (pan < 0.f)
        pan = 0.f;
    if (pan > 1.f)
        pan = 1.f;
    v->gen.lgain = gain * (1.f - pan)  / 32768.f;
    v->gen.rgain = gain * pan / 32768.f;
    struct cbox_biquadf_coeffs *second_filter = &v->filter_coeffs;
    gboolean is4p = sampler_layer_data_is_4pole(v->layer);
    if (l->cutoff != -1.f)
    {
        float logcutoff = l->logcutoff + moddests[smdest_cutoff];
        if (logcutoff < 0)
            logcutoff = 0;
        if (logcutoff > 12798)
            logcutoff = 12798;
        //float resonance = v->resonance*pow(32.0,c->cc[71]/maxv);
        float resonance = l->resonance_linearized * dB2gain((is4p ? 0.5 : 1) * moddests[smdest_resonance]);
        if (resonance < 0.7f)
            resonance = 0.7f;
        if (resonance > 32.f)
            resonance = 32.f;
        switch(l->fil_type)
        {
        case sft_lp24hybrid:
            cbox_biquadf_set_lp_rbj_lookup(&v->filter_coeffs, &m->sincos[(int)logcutoff], resonance * resonance);
            cbox_biquadf_set_1plp_lookup(&v->filter_coeffs_extra, &m->sincos[(int)logcutoff], 1);
            second_filter = &v->filter_coeffs_extra;
            break;
            
        case sft_lp12:
        case sft_lp24:
            cbox_biquadf_set_lp_rbj_lookup(&v->filter_coeffs, &m->sincos[(int)logcutoff], resonance);
            break;
        case sft_hp12:
        case sft_hp24:
            cbox_biquadf_set_hp_rbj_lookup(&v->filter_coeffs, &m->sincos[(int)logcutoff], resonance);
            break;
        case sft_bp6:
        case sft_bp12:
            cbox_biquadf_set_bp_rbj_lookup(&v->filter_coeffs, &m->sincos[(int)logcutoff], resonance);
            break;
        case sft_lp6:
        case sft_lp12nr:
        case sft_lp24nr:
            cbox_biquadf_set_1plp_lookup(&v->filter_coeffs, &m->sincos[(int)logcutoff], l->fil_type != sft_lp6);
            break;
        case sft_hp6:
        case sft_hp12nr:
        case sft_hp24nr:
            cbox_biquadf_set_1php_lookup(&v->filter_coeffs, &m->sincos[(int)logcutoff], l->fil_type != sft_hp6);
            break;
        default:
            assert(0);
        }
    }
    if (__builtin_expect(l->tonectl_freq != 0, 0))
    {
        float ctl = l->tonectl + moddests[smdest_tonectl];
        if (fabs(ctl) > 0.0001f)
            cbox_onepolef_set_highshelf_setgain(&v->onepole_coeffs, dB2gain(ctl));
        else
            cbox_onepolef_set_highshelf_setgain(&v->onepole_coeffs, 1.0);
    }
    
    float leftright[2 * CBOX_BLOCK_SIZE];
        
    uint32_t samples = 0;


    if (v->current_pipe)
    {
        uint32_t limit = cbox_prefetch_pipe_get_remaining(v->current_pipe);
        if (limit <= 4)
            v->gen.mode = spt_inactive;
        else
        {
            samples = sampler_gen_sample_playback(&v->gen, leftright, limit - 4);
            cbox_prefetch_pipe_consumed(v->current_pipe, v->gen.consumed);
            v->gen.consumed = 0;
        }
    }
    else
    {
        samples = sampler_gen_sample_playback(&v->gen, leftright, (uint32_t)-1);
    }

    for (int i = 2 * samples; i < 2 * CBOX_BLOCK_SIZE; i++)
        leftright[i] = 0.f;
    if (l->cutoff != -1)
    {
        cbox_biquadf_process_stereo(&v->filter_left, &v->filter_right, &v->filter_coeffs, leftright);
        if (is4p)
            cbox_biquadf_process_stereo(&v->filter_left2, &v->filter_right2, second_filter, leftright);
    }
    if (__builtin_expect(l->tonectl_freq != 0, 0))
    {
        cbox_onepolef_process_stereo(&v->onepole_left, &v->onepole_right, &v->onepole_coeffs, leftright);
    }
    if (__builtin_expect(l->eq_bitmask, 0))
    {
        for (int eq = 0; eq < 3; eq++)
        {
            if (l->eq_bitmask & (1 << eq))
            { 
                cbox_biquadf_process_stereo(&v->eq_left[eq], &v->eq_right[eq], &v->eq_coeffs[eq], leftright);
            }
        }
    }
        
    mix_block_into(outputs, v->output_pair_no * 2, leftright);
    if (__builtin_expect((v->send1bus > 0 && v->send1gain != 0) || (v->send2bus > 0 && v->send2gain != 0), 0))
    {
        if (v->send1bus > 0 && v->send1gain != 0)
        {
            int oofs = m->module.aux_offset + (v->send1bus - 1) * 2;
            mix_block_into_with_gain(outputs, oofs, leftright, v->send1gain);
        }
        if (v->send2bus > 0 && v->send2gain != 0)
        {
            int oofs = m->module.aux_offset + (v->send2bus - 1) * 2;
            mix_block_into_with_gain(outputs, oofs, leftright, v->send2gain);
        }
    }
    if (v->gen.mode == spt_inactive)
        sampler_voice_inactivate(v, FALSE);
}

