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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <fstream>
#include <sstream>

#include "VolumeRampFilterPlugin.hxx"

#include "LogLevel.hxx"
#include "config/Block.hxx"
#include "filter/Filter.hxx"
#include "filter/FilterPlugin.hxx"
#include "filter/Prepared.hxx"
#include "pcm/Buffer.hxx"
#include "pcm/Volume.hxx"
#include "pcm/AudioFormat.hxx"
#include "Log.hxx"
#include "util/Domain.hxx"
#include "pcm/Silence.hxx"
#include "util/StringBuffer.hxx"

#include "VolumeRampFilterPluginImpl.hxx"

static const Domain filter_domain("volume_ramp");

VolumeRampFilter::VolumeRampFilter(const AudioFormat& fmt, float ramp_time, float add_time, size_t block_size):
    Filter(fmt), pv(), state(VolumeRampState::Silence), cur_time(0), delay_buf(nullptr), last_delay_buf_size(0), buffer_mtx(),
    volume_ramp_time(ramp_time), silence_add_time(add_time), ramp_block_size(block_size), input_fmt(fmt)
{
    //allocate delay buffer and fill it with silence (zeroes)
    size_t buf_cap = input_fmt.TimeToSize(std::chrono::duration<float>(volume_ramp_time));
    delay_buf.Grow(buf_cap);
    auto fill_span = delay_buf.Write();
    //std::fill(fill_span.begin(), fill_span.end(), (std::byte)0);
    PcmSilence(fill_span, input_fmt.format);
    delay_buf.Append(buf_cap);
    
    out_audio_format.format = pv.Open(out_audio_format.format, false); //don't convert so we can implement passthrough
    LogFmt(LogLevel::DEBUG, filter_domain, "Created {}: time {}, silence {}, block {}, ifmt {}, ofmt {}", (void*)this, volume_ramp_time, silence_add_time, ramp_block_size, ToString(input_fmt), ToString(out_audio_format));
}

void VolumeRampFilter::trigger(){
    cur_time = 0.0;
    state = VolumeRampState::Silence;
    LogFmt(LogLevel::DEBUG, filter_domain, "Triggered {}", (void*)this);
}

std::span<const std::byte> VolumeRampFilter::FilterPCM(std::span<const std::byte> src) {
    std::scoped_lock<std::mutex> buf_lock(buffer_mtx);
    if(last_delay_buf_size != 0){
	//LogFmt(LogLevel::DEBUG, filter_domain, "Filter {}: consume {} bytes", (void*)this, last_delay_buf_size);
	delay_buf.Consume(last_delay_buf_size);
	last_delay_buf_size = 0;
    }
    
    if(state == VolumeRampState::Final){
	delay_buf.WantWrite(src.size());
	std::span<std::byte> dst = delay_buf.Write().subspan(0, src.size());
	std::copy(src.begin(), src.end(), dst.begin());
	delay_buf.Append(src.size());

	//return delayed data untouched
	LogFmt(LogLevel::DEBUG, filter_domain, "Filter {}: pass, {} bytes", (void*)this, src.size());
	std::span<std::byte> delayed_src = delay_buf.Read().subspan(0, src.size());
	last_delay_buf_size = src.size();
	return delayed_src;
    } else {
	delay_buf.WantWrite(src.size());
	std::span<std::byte> dst = delay_buf.Write().subspan(0, src.size());
	std::copy(src.begin(), src.end(), dst.begin());
	delay_buf.Append(src.size());
	
	std::span<std::byte> delayed_src = delay_buf.Read().subspan(0, src.size());
	last_delay_buf_size = src.size();
	size_t block_size = input_fmt.GetFrameSize() * ramp_block_size;
	

	for(size_t i = 0; i < src.size(); i += block_size){
	    size_t cur_block_size = (src.size() - i);
	    if(cur_block_size > block_size) cur_block_size = block_size;
	    std::span<std::byte> cur_delayed_src = delayed_src.subspan(i, cur_block_size);
		
	    switch (state) {
	    case VolumeRampState::RampDown:
	    case VolumeRampState::Silence: {
		LogFmt(LogLevel::DEBUG, filter_domain, "Filter {}: silence, {}%", (void*)this, (cur_time/(volume_ramp_time + silence_add_time) * 100.0f));
		pv.SetVolume(0);
		cur_time += input_fmt.SizeToTime<std::chrono::duration<float>>(cur_delayed_src.size()).count();
		if(cur_time >= (volume_ramp_time + silence_add_time)){
		    cur_time = 0.0;
		    state = VolumeRampState::RampUp;
		}
		//return silence
		PcmSilence(cur_delayed_src, out_audio_format.format);
	    } break;
	    case VolumeRampState::RampUp: {
		//unsigned volume = std::min((unsigned)(std::pow(10.0f, (volume_ramp_floor * (1.0f - cur_time / volume_ramp_time))/20.0f) * PCM_VOLUME_1), PCM_VOLUME_1);
		unsigned volume = std::min((unsigned)((cur_time / volume_ramp_time) * PCM_VOLUME_1), PCM_VOLUME_1);
		LogFmt(LogLevel::DEBUG, filter_domain, "Filter {}: ramp up, {}%, {} vol", (void*)this, (cur_time/volume_ramp_time * 100.0f), volume);
		pv.SetVolume(volume);
		cur_time += input_fmt.SizeToTime<std::chrono::duration<float>>(cur_delayed_src.size()).count();
		if(cur_time >= volume_ramp_time){
		    cur_time = 0.0;
		    state = VolumeRampState::Final;
		    LogFmt(LogLevel::DEBUG, filter_domain, "Filter {}: final", (void*)this);
		}
		//return processed data
		std::span<const std::byte> processed_data = pv.Apply(cur_delayed_src);
		std::copy(processed_data.begin(), processed_data.end(), cur_delayed_src.begin());
	    }
		break;
	    case VolumeRampState::Final:
		//TF??
		//just copy data
		LogFmt(LogLevel::DEBUG, filter_domain, "Filter {}: final, {} bytes", (void*)this, cur_delayed_src.size());
		std::copy(cur_delayed_src.begin(), cur_delayed_src.end(), cur_delayed_src.begin());
		break;
	    }
	}
	return delayed_src;
    }
}

void VolumeRampFilter::Reset() noexcept {
    LogFmt(LogLevel::DEBUG, filter_domain, "Reset {}", (void*)this);
    cur_time = 0.0;
    state = VolumeRampState::Silence;
}

/*template <class T, size_t E>
static std::string stringify_span(const std::span<T, E>& data){
    std::ostringstream sstr;
    if(data.empty())
	return "";
    sstr << std::hex << (int)data.front();
    for(const auto& i: data.subspan(1)){
	sstr << ", " << (int)i;
    }
    return sstr.str();
}

template <class T, size_t E>
static std::string stringify_span_as_float(const std::span<T, E>& data){
    std::span<const float> rdata{reinterpret_cast<const float*>(data.data()), data.size()/sizeof(float)};
    std::ostringstream sstr;
    if(rdata.empty())
	return "";
    sstr << rdata.front();
    for(const auto& i: rdata.subspan(1)){
	sstr << ", " << i;
    }
    return sstr.str();
}

template <class T, size_t E>
static void append_span_to_file(char const* fname, const std::span<T, E>& data){
    std::ofstream fout(fname, std::ios_base::out | std::ios_base::binary | std::ios_base::app);
    if(fout.is_open()){
	fout.write((const std::ofstream::char_type*)(data.data()), data.size_bytes());
	fout.close();
    } else {
	throw std::runtime_error("haram");
    }
    }*/

std::span<const std::byte> VolumeRampFilter::Flush() {
    std::scoped_lock<std::mutex> buf_lock(buffer_mtx);
    if(last_delay_buf_size != 0){
	//LogFmt(LogLevel::DEBUG, filter_domain, "Flush {}: consume {} bytes", (void*)this, last_delay_buf_size);
	delay_buf.Consume(last_delay_buf_size);
	last_delay_buf_size = 0;
    }
    
    switch (state) {
    case VolumeRampState::RampUp:
	cur_time = (volume_ramp_time - cur_time);
	[[gnu::fallthrough]];
    case VolumeRampState::Final:
	state = VolumeRampState::RampDown;
	//append_span_to_file("/home/morozzz/Records/Flush_fifo_debug.bin", delay_buf.Read());
	[[gnu::fallthrough]];
    case VolumeRampState::RampDown: {
	//unsigned volume = std::min((unsigned)(std::pow(10.0f, (volume_ramp_floor * (cur_time / volume_ramp_time))/20.0f) * PCM_VOLUME_1), PCM_VOLUME_1);
	unsigned volume = std::min((unsigned)((1.0f - cur_time / volume_ramp_time) * PCM_VOLUME_1), PCM_VOLUME_1);
	LogFmt(LogLevel::DEBUG, filter_domain, "Flush {}: ramp down, {}%, {} vol", (void*)this, (cur_time/volume_ramp_time * 100.0f), volume);
	size_t cur_block_size = ramp_block_size * input_fmt.GetFrameSize();
	pv.SetVolume(volume);
	cur_time += input_fmt.SizeToTime<std::chrono::duration<float>>(cur_block_size).count();
	if(cur_time >= volume_ramp_time){
	    cur_time = 0.0;
	    state = VolumeRampState::Silence;
	}

	delay_buf.WantWrite(cur_block_size);
	std::span<std::byte> dst = delay_buf.Write().subspan(0, cur_block_size);
	PcmSilence(dst, input_fmt.format);
	delay_buf.Append(cur_block_size);
	
	std::span<const std::byte> delayed_src = delay_buf.Read().subspan(0, cur_block_size);
	last_delay_buf_size = cur_block_size;
	//append_span_to_file("/home/morozzz/Records/Flush_src_debug.bin", delayed_src);
	std::span<const std::byte> ret = pv.Apply(delayed_src);
	//LogFmt(LogLevel::DEBUG, filter_domain, "Flush {} data: {}", (void*)this, stringify_span_as_float(ret));
	//append_span_to_file("/home/morozzz/Records/Flush_pv_debug.bin", ret);
	return ret;
    }
	break;
    case VolumeRampState::Silence:
	LogFmt(LogLevel::DEBUG, filter_domain, "Flush {}: silence", (void*)this);
	pv.SetVolume(0);
	cur_time = 0.0;
	return std::span<const std::byte>{};
	break;
    }
    
    return std::span<const std::byte>{};
}

VolumeRampFilter::~VolumeRampFilter() {
    /*if(par){
	par->remove(this);
	par = nullptr;
	}*/
}


/*VolumeRampSingleton::VolumeRampSingleton() : ramps_mtx(), ramps() {}

void VolumeRampSingleton::add(VolumeRampFilter* flt){
    std::lock_guard<std::mutex> lock(ramps_mtx);
    ramps.insert(flt);
}
    
void VolumeRampSingleton::remove(VolumeRampFilter* flt){
    std::lock_guard<std::mutex> lock(ramps_mtx);
    ramps.erase(flt);
}

void VolumeRampSingleton::trigger(){
    std::lock_guard<std::mutex> lock(ramps_mtx);
    for(auto& i: ramps) i->trigger();
}

VolumeRampSingleton VolumeRampSingleton::obj{};*/


PreparedVolumeRampFilter::PreparedVolumeRampFilter(const ConfigBlock& cfg):
    volume_ramp_time(0.1), silence_add_time(0.1), ramp_block_size(64){
    float cfg_ramp_time = strtof(cfg.GetBlockValue("ramp_seconds", ""), nullptr);
    float cfg_add_time = strtof(cfg.GetBlockValue("silence_seconds", ""), nullptr);
    size_t cfg_block_size = strtoul(cfg.GetBlockValue("block_size", ""), nullptr, 10);
    if(cfg_ramp_time != 0.0f) volume_ramp_time = cfg_ramp_time;
    if(cfg_add_time != 0.0f) silence_add_time = cfg_add_time;
    if(cfg_block_size != 0) ramp_block_size = cfg_block_size;
}

std::unique_ptr<Filter> PreparedVolumeRampFilter::Open(AudioFormat &af){
    return std::make_unique<VolumeRampFilter>(af, volume_ramp_time, silence_add_time, ramp_block_size);
}

static std::unique_ptr<PreparedFilter> volume_ramp_filter_plugin_init(const ConfigBlock &block){
    return std::make_unique<PreparedVolumeRampFilter>(block);
}

const FilterPlugin volume_ramp_filter_plugin = {
    .name = "volume_ramp",
    .init = volume_ramp_filter_plugin_init,
};
