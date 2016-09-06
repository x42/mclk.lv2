/* mclk -- LV2 midi clock generator
 *
 * Copyright (C) 2016 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#define CLK_URI "http://gareus.org/oss/lv2/mclk"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/log/logger.h>
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_Sequence;
	LV2_URID midi_MidiEvent;
	LV2_URID atom_Float;
	LV2_URID atom_Int;
	LV2_URID atom_Long;
	LV2_URID time_Position;
	LV2_URID time_bar;
	LV2_URID time_barBeat;
	LV2_URID time_beatUnit;
	LV2_URID time_beatsPerBar;
	LV2_URID time_beatsPerMinute;
	LV2_URID time_speed;
	LV2_URID time_frame;
} MclkURIs;

/* MIDI System Real-Time Messages */
#define MIDI_RT_CLOCK    (0xF8)
#define MIDI_RT_START    (0xFA)
#define MIDI_RT_CONTINUE (0xFB)
#define MIDI_RT_STOP     (0xFC)

/* bitwise flags -- mode */
enum {
	MSG_NO_TRANSPORT  = 1, /**< do not send start/stop/continue messages */
	MSG_NO_POSITION   = 2, /**< do not send absolute song position */
	MSG_NO_CLOCK      = 4  /**< do not send MIDI Clock*/
};

typedef struct {
	/* ports */
	const LV2_Atom_Sequence* control;
	LV2_Atom_Sequence* midiout;
	float* p_mode;
	float* p_sync;
	float* p_bpm;
	float* p_transport;
	float* p_rewind;
	float* p_hostbpm;
	float* p_songpos;

	/* Cached Ports */
	float c_mode;
	float c_transport;
	float c_bpm;

	/* atom-forge and URI mapping */
	LV2_Atom_Forge forge;
	LV2_Atom_Forge_Frame frame;
	MclkURIs uris;

	/* LV2 Output */
	LV2_Log_Log* log;
	LV2_Log_Logger logger;

	/* Host Time */
	bool     host_info;
	float    host_bpm;
	float    bar_beats;
	float    host_speed;
	int      host_div;
	long int host_frame;

	/* Settings */
	float sample_rate;
	int mode;

	/* State */
	bool  rolling;
	float bb;
	int64_t last_bcnt;
	int64_t sample_pos;
	double mclk_last_tick; // in audio-samples
} Mclk;

/* *****************************************************************************
 * helper functions
 */

/** map uris */
static void
map_uris (LV2_URID_Map* map, MclkURIs* uris)
{
	uris->atom_Blank          = map->map (map->handle, LV2_ATOM__Blank);
	uris->atom_Object         = map->map (map->handle, LV2_ATOM__Object);
	uris->midi_MidiEvent      = map->map (map->handle, LV2_MIDI__MidiEvent);
	uris->atom_Sequence       = map->map (map->handle, LV2_ATOM__Sequence);
	uris->time_Position       = map->map (map->handle, LV2_TIME__Position);
	uris->atom_Long           = map->map (map->handle, LV2_ATOM__Long);
	uris->atom_Int            = map->map (map->handle, LV2_ATOM__Int);
	uris->atom_Float          = map->map (map->handle, LV2_ATOM__Float);
	uris->time_bar            = map->map (map->handle, LV2_TIME__bar);
	uris->time_barBeat        = map->map (map->handle, LV2_TIME__barBeat);
	uris->time_beatUnit       = map->map (map->handle, LV2_TIME__beatUnit);
	uris->time_beatsPerBar    = map->map (map->handle, LV2_TIME__beatsPerBar);
	uris->time_beatsPerMinute = map->map (map->handle, LV2_TIME__beatsPerMinute);
	uris->time_speed          = map->map (map->handle, LV2_TIME__speed);
	uris->time_frame          = map->map (map->handle, LV2_TIME__frame);
}

/**
 * Update the current position based on a host message. This is called by
 * run() when a time:Position is received.
 */
static void
update_position (Mclk* self, const LV2_Atom_Object* obj)
{
	const MclkURIs* uris = &self->uris;

	LV2_Atom* bar   = NULL;
	LV2_Atom* beat  = NULL;
	LV2_Atom* bunit = NULL;
	LV2_Atom* bpb   = NULL;
	LV2_Atom* bpm   = NULL;
	LV2_Atom* speed = NULL;
	LV2_Atom* frame = NULL;

	lv2_atom_object_get (
			obj,
			uris->time_bar, &bar,
			uris->time_barBeat, &beat,
			uris->time_beatUnit, &bunit,
			uris->time_beatsPerBar, &bpb,
			uris->time_beatsPerMinute, &bpm,
			uris->time_speed, &speed,
			uris->time_frame, &frame,
			NULL);

	if (   bpm   && bpm->type == uris->atom_Float
			&& bpb   && bpb->type == uris->atom_Float
			&& bar   && bar->type == uris->atom_Long
			&& beat  && beat->type == uris->atom_Float
			&& bunit && bunit->type == uris->atom_Int
			&& speed && speed->type == uris->atom_Float
			&& frame && frame->type == uris->atom_Long)
	{
		float    _bpb   = ((LV2_Atom_Float*)bpb)->body;
		long int _bar   = ((LV2_Atom_Long*)bar)->body;
		float    _beat  = ((LV2_Atom_Float*)beat)->body;

		self->host_div   = ((LV2_Atom_Int*)bunit)->body;
		self->host_bpm   = ((LV2_Atom_Float*)bpm)->body;
		self->host_speed = ((LV2_Atom_Float*)speed)->body;
		self->host_frame = ((LV2_Atom_Long*)frame)->body;

		self->bar_beats  = _bar * _bpb + _beat * self->host_div / 4.0;
		self->host_info  = true;
	}
}

/**
 * add a midi message to the output port
 */
static void
forge_midimessage (Mclk* self,
                   uint32_t tme,
                   const uint8_t* const buffer,
                   uint32_t size)
{
	LV2_Atom midiatom;
	midiatom.type = self->uris.midi_MidiEvent;
	midiatom.size = size;

	if (0 == lv2_atom_forge_frame_time (&self->forge, tme)) return;
	if (0 == lv2_atom_forge_raw (&self->forge, &midiatom, sizeof (LV2_Atom))) return;
	if (0 == lv2_atom_forge_raw (&self->forge, buffer, size)) return;
	lv2_atom_forge_pad (&self->forge, sizeof (LV2_Atom) + size);
}

/* *****************************************************************************
 * Midi Clock
 */

static int64_t
send_pos_message (Mclk* self, const int64_t bcnt)
{
	if (self->mode & MSG_NO_POSITION) return -1;
  /* send '0xf2' Song Position Pointer.
   * This is an internal 14 bit register that holds the number of
   * MIDI beats (1 beat = six MIDI clocks) since the start of the song.
   */
  if (bcnt < 0 || bcnt >= 16384) {
    return bcnt;
  }

  uint8_t buffer[3];
  buffer[0] = 0xf2;
  buffer[1] = bcnt & 0x7f; // LSB
  buffer[2] = (bcnt >> 7) & 0x7f; // MSB
	forge_midimessage (self, 0, buffer, 3);
	return bcnt;
}

/**
 * send 1 byte MIDI Message
 * @param port_buf buffer to write event to
 * @param time sample offset of event
 * @param rt_msg message byte
 */
static void
send_rt_message (Mclk* self, uint32_t tme, uint8_t rt_msg)
{
	forge_midimessage (self, tme, &rt_msg, 1);
}

/**
 * calculate song position (14 bit integer) from current BBT info.
 *
 * see "Song Position Pointer" at
 * http://www.midi.org/techspecs/midimessages.php
 *
 * Because this value is also used internally to sync/send
 * start/continue realtime messages, a 64 bit integer
 * is used to cover the complete range.
 */
static const int64_t
calc_song_pos (float bar_beat, float bpm, int off)
{
	const double resync_delay = 1.0; /**< seconds between 'pos' and 'continue' message */

  if (off < 0) {
    /* auto offset */
    if (bar_beat == 0) off = 0;
    else off = rintf (bpm * 4.f * resync_delay / 60.f);
  }
  return off + floor (4.f * bar_beat);
}

/* *****************************************************************************
 * LV2 Plugin
 */

static LV2_Handle
instantiate (const LV2_Descriptor*     descriptor,
             double                    rate,
             const char*               bundle_path,
             const LV2_Feature* const* features)
{
	Mclk* self = (Mclk*)calloc (1, sizeof (Mclk));
	LV2_URID_Map* map = NULL;

	int i;
	for (i=0; features[i]; ++i) {
		if (!strcmp (features[i]->URI, LV2_URID__map)) {
			map = (LV2_URID_Map*)features[i]->data;
		} else if (!strcmp (features[i]->URI, LV2_LOG__log)) {
			self->log = (LV2_Log_Log*)features[i]->data;
		}
	}

	lv2_log_logger_init (&self->logger, map, self->log);

	if (!map) {
		lv2_log_error (&self->logger, "Mclk.lv2 error: Host does not support urid:map\n");
		free (self);
		return NULL;
	}

	lv2_atom_forge_init (&self->forge, map);
	map_uris (map, &self->uris);

	self->sample_rate = rate;

	self->bb = 0;
	self->rolling = false;
	self->last_bcnt = -1;
	self->mclk_last_tick = 0;
	self->sample_pos = 0;

	return (LV2_Handle)self;
}

static void
connect_port (LV2_Handle instance,
              uint32_t   port,
              void*      data)
{
	Mclk* self = (Mclk*)instance;

	switch (port) {
		case 0:
			self->control = (const LV2_Atom_Sequence*)data;
			break;
		case 1:
			self->midiout = (LV2_Atom_Sequence*)data;
			break;
		case 2:
			self->p_mode = (float*)data;
			break;
		case 3:
			self->p_sync = (float*)data;
			break;
		case 4:
			self->p_bpm = (float*)data;
			break;
		case 5:
			self->p_transport = (float*)data;
			break;
		case 6:
			self->p_rewind = (float*)data;
			break;
		case 7:
			self->p_hostbpm = (float*)data;
			break;
		case 8:
			self->p_songpos = (float*)data;
			break;
		default:
			break;
	}
}


static void
run (LV2_Handle instance, uint32_t n_samples)
{
	Mclk* self = (Mclk*)instance;
	if (!self->midiout || !self->control) {
		return;
	}

	/* initialize output port */
	const uint32_t capacity = self->midiout->atom.size;
	lv2_atom_forge_set_buffer (&self->forge, (uint8_t*)self->midiout, capacity);
	lv2_atom_forge_sequence_head (&self->forge, &self->frame, 0);

	/* process control events */
	LV2_Atom_Event* ev = lv2_atom_sequence_begin (&(self->control)->body);
	while (!lv2_atom_sequence_is_end (&(self->control)->body, (self->control)->atom.size, ev)) {
		if (ev->body.type == self->uris.atom_Blank || ev->body.type == self->uris.atom_Object) {
			const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == self->uris.time_Position) {
				update_position (self, obj);
			}
		}
		ev = lv2_atom_sequence_next (ev);
	}

	bool rolling;
	float bpm;
	float bb;
	double quarter_notes_per_beat = 1.0;
	int64_t sample_position;

	/* set mode */
	switch ((int)rintf (*self->p_mode)) {
		case 0:
			self->mode = MSG_NO_POSITION | MSG_NO_CLOCK;
			break;
		case 1:
			self->mode = MSG_NO_POSITION | MSG_NO_TRANSPORT;
			break;
		case 2:
			self->mode = MSG_NO_POSITION;
			break;
		default:
		case 3:
			self->mode = 0;
			break;
	}

	/* set BarBeat & transport state */
	if (self->host_info && *self->p_sync > 0) {
		*self->p_hostbpm = self->host_bpm;
		bpm = self->host_bpm * self->host_speed;
		sample_position = self->host_frame;
		rolling = self->host_speed > 0;
		bb = self->bar_beats;
		if (self->host_speed < 0) {
			goto noroll;
		}
		if (fabsf(self->bb - self->bar_beats) > 1) {
			/* located */
			self->rolling = rolling = false;
			self->bb = -1;
		}
	} else {
		*self->p_hostbpm = self->host_info ? -1 : 0;
		bpm = *self->p_bpm;
		if (*self->p_rewind > 0) {
			bb = self->bb = 0;
			self->last_bcnt = -1;
			rolling = false;
			sample_position = self->sample_pos = 0;
		} else {
			rolling = *self->p_transport > 0;
			bb = self->bb;
			sample_position = self->sample_pos;
		}
	}

	const double samples_per_beat = (double) self->sample_rate * 60.0 / bpm;
	const double samples_per_quarter_note = samples_per_beat / quarter_notes_per_beat;
	const double clock_tick_interval = samples_per_quarter_note / 24.0;

	/* send position updates if stopped and located */
	if (!rolling && !self->rolling) {
		if (bb != self->bb) {
			self->last_bcnt = send_pos_message (self, calc_song_pos (bb, bpm, -1));
		}
	}

	/* send RT messages start/stop/continue if transport state changed */
	if (rolling != self->rolling) {
		if (rolling) {
			/* stop -> playing */
			if (bb == 0 || 0 != (self->mode & MSG_NO_POSITION)) {
				/* start playing now */
				if (!(self->mode & MSG_NO_TRANSPORT)) {
					send_rt_message (self, 0, MIDI_RT_START);
				}
				self->last_bcnt = -1; /* 'start' at 0, don't queue 'continue' message */
			} else {
				/* continue after pause
				 *
				 * only send continue message here if song-position is not used.
				 * w/song-pos it is queued just-in-time with clock
				 */
				if (0 != (self->mode & MSG_NO_POSITION)) {
					if (0 == (self->mode & MSG_NO_TRANSPORT)) {
						send_rt_message (self, 0, MIDI_RT_CONTINUE);
					}
				}
			}
			/* initial beat tick */
			if (0 == bb || 0 != (self->mode & (MSG_NO_POSITION))) {
				if (0 == (self->mode & MSG_NO_CLOCK)) {
					//send_rt_message (self, 0, MIDI_RT_CLOCK);
				}
			}
		} else {
			/* playing -> stop */
			if (0 == (self->mode & MSG_NO_TRANSPORT)) {
				send_rt_message (self, 0, MIDI_RT_STOP);
			}
			self->last_bcnt = send_pos_message (self, calc_song_pos (bb, bpm, -1));
		}
		self->mclk_last_tick = samples_per_beat * bb;
	}

	self->rolling = rolling;

	if (!rolling || (self->mode & MSG_NO_CLOCK)) {
		goto noroll;
	}

	/* send clock ticks for this cycle */
	int ticks_sent_this_cycle = 0;
	while (1) {
		const double next_tick = self->mclk_last_tick + clock_tick_interval;
		const int64_t next_tick_offset = llrint (next_tick) - sample_position;
		if (next_tick_offset >= n_samples) break;

		if (next_tick_offset >= 0) {
			if (self->last_bcnt > 0 && !(self->mode & MSG_NO_POSITION)) {
				/* send 'continue' realtime message on time */
				const int64_t bcnt = calc_song_pos (bb, bpm, 0);
				/* 4 MIDI-beats per quarter note (jack beat) */
				if (bcnt + ticks_sent_this_cycle / 6 >= self->last_bcnt) {
					if (!(self->mode & MSG_NO_TRANSPORT)) {
						send_rt_message (self, next_tick_offset, MIDI_RT_CONTINUE);
					}
					self->last_bcnt = -1;
				}
			}
			/* enqueue clock tick */
			send_rt_message (self, next_tick_offset, MIDI_RT_CLOCK);
			ticks_sent_this_cycle++;
		}

		self->mclk_last_tick = next_tick;
	}

noroll:
	*self->p_songpos = bb;

	/* keep track of host position.. */
	if (self->host_info) {
		self->bar_beats += n_samples * self->host_bpm * self->host_speed / (60.f * self->sample_rate);
		self->host_frame += n_samples * self->host_speed;
	}

	/* prepare for next cycle */
	if (self->host_info && *self->p_sync > 0) {
		self->bb = self->bar_beats;
		self->sample_pos = self->host_frame;
	} else if (rolling) {
		self->bb += n_samples * bpm / (60.f * self->sample_rate);
		self->sample_pos += n_samples;
	}
}

static void
cleanup (LV2_Handle instance)
{
	free (instance);
}

static const void*
extension_data (const char* uri)
{
	return NULL;
}

static const LV2_Descriptor descriptor = {
	CLK_URI,
	instantiate,
	connect_port,
	NULL,
	run,
	NULL,
	cleanup,
	extension_data
};

#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
#    define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
#    define LV2_SYMBOL_EXPORT  __attribute__ ((visibility ("default")))
#endif
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor (uint32_t index)
{
	switch (index) {
	case 0:
		return &descriptor;
	default:
		return NULL;
	}
}
