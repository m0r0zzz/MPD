/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPD_VOLUME_RAMP_FILTER_PLUGIN_IMPL_HXX
#define MPD_VOLUME_RAMP_FILTER_PLUGIN_IMPL_HXX

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <set>
#include <mutex>
#include <vector>

#include "pcm/Volume.hxx"
#include "pcm/AudioFormat.hxx"
#include "filter/Filter.hxx"
#include "filter/Prepared.hxx"
#include "config/Block.hxx"

#include "util/DynamicFifoBuffer.hxx"

//class VolumeRampSingleton;

enum class VolumeRampState {
    RampDown,
    Silence,
    RampUp,
    Final
};

class VolumeRampFilter: public Filter {
    PcmVolume pv;
    VolumeRampState state;
    float cur_time;

    DynamicFifoBuffer<std::byte> delay_buf;
    size_t last_delay_buf_size;
    //PcmBuffer buffer; //Flush will use PcmVolume buffer directly, without intermediate buffering required for FilterPCM block processing
    std::mutex buffer_mtx;
    
    float volume_ramp_time;
    float silence_add_time;
    size_t ramp_block_size;
    AudioFormat input_fmt;

    //VolumeRampSingleton *par;

    //static constexpr float silence_fixup_delay = 0.05;
    //static constexpr size_t ramp_block_size = 64;
public:
    VolumeRampFilter(const AudioFormat& fmt, float ramp_time, float add_time, size_t block_size);

    void trigger();

    std::span<const std::byte> FilterPCM(std::span<const std::byte> src) override;
    void Reset() noexcept override;
    std::span<const std::byte> Flush() override;

    ~VolumeRampFilter();
};


/*class VolumeRampSingleton {
    std::mutex ramps_mtx;
    std::set<VolumeRampFilter*> ramps;

    static VolumeRampSingleton obj;

    VolumeRampSingleton();
public:
    void add(VolumeRampFilter* flt);
    void remove(VolumeRampFilter* flt);
    void trigger();

    static VolumeRampSingleton* get(){ return &obj; }
    };*/

class PreparedVolumeRampFilter: public PreparedFilter {
    float volume_ramp_time;
    float silence_add_time;
    size_t ramp_block_size;
public:
    PreparedVolumeRampFilter(const ConfigBlock& cfg);

    std::unique_ptr<Filter> Open(AudioFormat &af) override;
};

#endif
