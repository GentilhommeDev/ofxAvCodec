//
//  ofxAvAudioPlayer.cpp
//  emptyExample
//
//  Created by Hansi on 13.07.15.
//
// a huge chunk of this file is based on the
// blog entry https://blinkingblip.wordpress.com/2011/10/08/decoding-and-playing-an-audio-stream-using-libavcodec-libavformat-and-libao/
// (had to make some adjustments for changes in ffmpeg
// and libavcodecs examples
// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decoding_encoding.c

#include "ofxAvAudioPlayer.h"
#include "ofMain.h"
#include "ofxAvUtils.h"

using namespace std;

#define die(msg) { unload(); cerr << msg << endl; return false; }

ofxAvAudioPlayer::ofxAvAudioPlayer(){
	ofxAvUtils::init(); 
	// default audio settings
	output_channel_layout = av_get_default_channel_layout(2);
	output_sample_rate = 44100;
	output_num_channels = 2;
	output_config_changed = false; 
	volume = 1;
	
	forceNativeFormat = false;
	
	isLooping = false;
	container = NULL; 
	decoded_frame = NULL;
	codec_context = NULL;
	buffer_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
	swr_context = NULL; 
	av_init_packet(&packet);
	unload();
	
}

ofxAvAudioPlayer::~ofxAvAudioPlayer(){
	unload();
}

bool ofxAvAudioPlayer::loadSound(string fileName, bool stream){
	return load(fileName, stream);
}

bool ofxAvAudioPlayer::load(string fileName, bool stream){
	unload();
	
	string fileNameAbs = ofToDataPath(fileName,true);
	const char * input_filename = fileNameAbs.c_str();
	// the first finds the right codec, following  https://blinkingblip.wordpress.com/2011/10/08/decoding-and-playing-an-audio-stream-using-libavcodec-libavformat-and-libao/
	container = 0;
	if (avformat_open_input(&container, input_filename, NULL, NULL) < 0) {
		die("Could not open file");
	}
 
	if (avformat_find_stream_info(container,NULL) < 0) {
		die("Could not find file info");
	}
 
	audio_stream_id = -1;
 
	// To find the first audio stream. This process may not be necessary
	// if you can gurarantee that the container contains only the desired
	// audio stream
	int i;
	for (i = 0; i < container->nb_streams; i++) {
		if (container->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream_id = i;
			break;
		}
	}
 
	if (audio_stream_id == -1) {
		die("Could not find an audio stream");
	}
 
	// Find the apropriate codec and open it
	codec_context = container->streams[audio_stream_id]->codec;
	if( forceNativeFormat ){
		output_sample_rate = codec_context->sample_rate;
		output_channel_layout = codec_context->channel_layout;
		if( output_channel_layout == 0 ){
			output_num_channels = codec_context->channels;
			output_channel_layout = av_get_default_channel_layout(output_num_channels);
		}
		else{
			output_num_channels = av_get_channel_layout_nb_channels( output_channel_layout );
		}
		cout << "native audio thing: " << output_sample_rate << "Hz / " << output_num_channels << " channels" << endl;
	}
	
	AVCodec* codec = avcodec_find_decoder(codec_context->codec_id);
	if (avcodec_open2(codec_context, codec,NULL)) {
		die("Could not find open the needed codec");
	}
	
	// from here on it's mostly following
	// https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decoding_encoding.c
	//packet.data = inbuf;
	//packet.size = fread(inbuf, 1, AVCODEC_AUDIO_INBUF_SIZE, f);
	av_init_packet(&packet);
	packet.data = NULL;
	packet.size = 0;

	swr_context = NULL;
	fileLoaded = true;
	isPlaying = true;
	
	// we continue here:
	decode_next_frame();
	duration = av_time_to_millis(container->streams[audio_stream_id]->duration);

	return true;
}

bool ofxAvAudioPlayer::setupAudioOut( int numChannels, int sampleRate ){
	if( numChannels != output_num_channels || sampleRate != output_sample_rate ){
		output_channel_layout = av_get_default_channel_layout(numChannels);
		output_sample_rate = sampleRate;
		output_num_channels = numChannels;
		
		if( swr_context != NULL ){
			output_config_changed = true;
		}
	}
	
	return true;
}

void ofxAvAudioPlayer::unloadSound(){
	unload();
}

void ofxAvAudioPlayer::unload(){
	len = 0;
	fileLoaded = false;
	isPlaying = false;
	packet_data_size = 0;
	decoded_buffer_pos = 0;
	decoded_buffer_len = 0;
	next_seekTarget = -1;

	av_free_packet(&packet);
	
	if( decoded_frame ){
		av_frame_unref(decoded_frame);
		av_frame_free(&decoded_frame);
		decoded_frame = NULL;
	}
	
	if( container ){
		avcodec_close(codec_context);
		avformat_close_input(&container);
		avformat_free_context(container);
		av_free(container); 
		container = NULL;
		codec_context = NULL;
	}
	
	if( swr_context ){
		swr_close(swr_context); 
		swr_free(&swr_context);
		swr_context = NULL;
	}
}

int ofxAvAudioPlayer::audioOut(float *output, int bufferSize, int nChannels){
	if( !fileLoaded ){ return 0; }
	
	if( next_seekTarget >= 0 ){
		//av_seek_frame(container,-1,next_seekTarget,AVSEEK_FLAG_ANY);
		avformat_seek_file(container,audio_stream_id,0,next_seekTarget,next_seekTarget,AVSEEK_FLAG_ANY);
		next_seekTarget = -1;
		avcodec_flush_buffers(codec_context);
		decode_next_frame();
	}
	
	if( !isPlaying ){ return 0; }
	
	
	int max_read_packets = 4;
	if( decoded_frame && decoded_frame);
	// number of samples read per channel (up to bufferSize)
	int num_samples_read = 0;
	
	if( decoded_frame == NULL ){
		decode_next_frame();
	}
	
	while( decoded_frame != NULL && max_read_packets > 0 ){
		max_read_packets --;
		
		int missing_samples = bufferSize*nChannels - num_samples_read;
		int available_samples = decoded_buffer_len - decoded_buffer_pos;
		if( missing_samples > 0 && available_samples > 0 ){
			int samples = min( missing_samples, available_samples );
			
			if( volume != 0 ){
				memcpy(output+num_samples_read, decoded_buffer+decoded_buffer_pos, samples*sizeof(float) );
			}
			
			if( volume != 1 && volume != 0 ){
				for( int i = 0; i < samples; i++ ){
					output[i+num_samples_read] *= volume;
				}
			}
			
			decoded_buffer_pos += samples;
			num_samples_read += samples;
		}
		
		if( num_samples_read >= bufferSize*nChannels ){
			return bufferSize;
		}
		else{
			decode_next_frame();
		}
	}
	
	return num_samples_read/nChannels;
}

bool ofxAvAudioPlayer::decode_next_frame(){
	av_free_packet(&packet);
	int res = av_read_frame(container, &packet);
	bool didRead = res >= 0;

	if( didRead ){
		int got_frame = 0;
		if (!decoded_frame) {
			if (!(decoded_frame = av_frame_alloc())) {
				fprintf(stderr, "Could not allocate audio frame\n");
				return false;
			}
		}
		else{
			av_frame_unref(decoded_frame);
		}
		
		len = avcodec_decode_audio4(codec_context, decoded_frame, &got_frame, &packet);
		if (len < 0) {
			// no data
			return false;
		}
		
		if( packet.stream_index != audio_stream_id ){
			return false;
		}
		
		if (got_frame) {
			
			if( swr_context != NULL && output_config_changed ){
				output_config_changed = false;
				if( swr_context ){
					swr_close(swr_context);
					swr_free(&swr_context);
					swr_context = NULL;
				}
			}
			
			if( swr_context == NULL ){
				int input_channel_layout = decoded_frame->channel_layout;
				if( input_channel_layout == 0 ){
					input_channel_layout = av_get_default_channel_layout( codec_context->channels );
				}
				swr_context = swr_alloc_set_opts(NULL,
												 output_channel_layout, AV_SAMPLE_FMT_FLT, output_sample_rate,
												 input_channel_layout, (AVSampleFormat)decoded_frame->format, decoded_frame->sample_rate,
												 0, NULL);
				swr_init(swr_context);

				if (!swr_context){
					fprintf(stderr, "Could not allocate resampler context\n");
					return false;
				}
			}

			/* if a frame has been decoded, resample to desired rate */
			int samples_per_channel = AVCODEC_MAX_AUDIO_FRAME_SIZE/output_num_channels;
			//samples_per_channel = 512;
			uint8_t * out = (uint8_t*)decoded_buffer;
			int samples_converted = swr_convert(swr_context,
												(uint8_t**)&out, samples_per_channel,
												(const uint8_t**)decoded_frame->extended_data, decoded_frame->nb_samples);
			decoded_buffer_len = samples_converted*output_num_channels;
			decoded_buffer_pos = 0;
		}

		packet.size -= len;
		packet.data += len;
//		packet->dts =
//		packet->pts = AV_NOPTS_VALUE;
		
		return true;
	}
	else{
		// no data read...
		packet_data_size = 0;
		decoded_buffer_len = 0;
		decoded_buffer_pos = 0;
		if( isLooping ){
			avformat_seek_file(container,audio_stream_id,0,0,0,AVSEEK_FLAG_ANY);
			avcodec_flush_buffers(codec_context);
			decode_next_frame();
		}
		else{
			isPlaying = false;
		}
		
		return false;
	}
}

unsigned long long ofxAvAudioPlayer::av_time_to_millis( int64_t av_time ){
	return av_rescale(1000*av_time,(uint64_t)container->streams[audio_stream_id]->time_base.num,container->streams[audio_stream_id]->time_base.den);
	//alternative:
	//return av_time*1000*av_q2d(container->streams[audio_stream_id]->time_base);
}

int64_t ofxAvAudioPlayer::millis_to_av_time( unsigned long long ms ){
	//TODO: fix conversion
/*	int64_t timeBase = (int64_t(codec_context->time_base.num) * AV_TIME_BASE) / int64_t(codec_context->time_base.den);
	int64_t seekTarget = int64_t(ms) / timeBase;*/
	return av_rescale(ms,container->streams[audio_stream_id]->time_base.den,(uint64_t)container->streams[audio_stream_id]->time_base.num)/1000;
}



void ofxAvAudioPlayer::setPositionMS(unsigned long long ms){
	next_seekTarget = millis_to_av_time(ms);
}

int ofxAvAudioPlayer::getPositionMS(){
	if( !fileLoaded ) return 0;
	int64_t ts = packet.pts;
	return av_time_to_millis( ts );
}

float ofxAvAudioPlayer::getPosition(){
	return duration == 0? 0 : (getPositionMS()/(float)duration);
}

void ofxAvAudioPlayer::setPosition(float percent){
	if(duration>0) setPositionMS((int)(percent*duration));
}

void ofxAvAudioPlayer::stop(){
	isPlaying =false;
}

void ofxAvAudioPlayer::play(){
	if( fileLoaded ){
		isPlaying = true;
	}
}

void ofxAvAudioPlayer::setPaused(bool paused){
	isPlaying = fileLoaded?false:!paused;
}

void ofxAvAudioPlayer::setLoop(bool loop){
	isLooping = loop;
}

void ofxAvAudioPlayer::setVolume(float vol){
	this->volume = vol;
}

float ofxAvAudioPlayer::getVolume(){
	return volume;
}

bool ofxAvAudioPlayer::isLoaded(){
	return fileLoaded; 
}

unsigned long long ofxAvAudioPlayer::getDurationMs(){
	return duration;
}

bool ofxAvAudioPlayer::getIsPlaying(){
	return isPlaying; 
}

string ofxAvAudioPlayer::getMetadata( string key ){
	if( container != NULL ){
		AVDictionaryEntry * entry = av_dict_get(container->metadata, key.c_str(), NULL, 0);
		if( entry == NULL ) return "";
		else return string(entry->value);
	}
	else{
		return "";
	}
}

map<string,string> ofxAvAudioPlayer::getMetadata(){
	map<string,string> meta;
	AVDictionary * d = container->metadata;
	AVDictionaryEntry *t = NULL;
	while ((t = av_dict_get(d, "", t, AV_DICT_IGNORE_SUFFIX))!=0){
		meta[string(t->key)] = string(t->value);
	}
	
	return meta; 
}

