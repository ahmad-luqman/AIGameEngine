// Audio files through the decoders the engine plays with (dr_wav, dr_flac, dr_mp3): opening, reading a stretch of them as
// streaming does, and seeking back as a loop does.
#include "FuzzCommon.h"

#include <Breccia/Audio/AudioEngine.h>

#include <miniaudio.h>

#include <span>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	BrecciaFuzz::Init();
	std::string error;
	(void)Breccia::AudioEngine::CanDecode(std::span(data, size), error);

	ma_decoder decoder;
	const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, 48000);
	if (ma_decoder_init_memory(data, size, &config, &decoder) != MA_SUCCESS)
		return 0;
	float frames[2 * 1024];
	// Bounded: a few header bytes can declare hours of audio.
	for (int chunk = 0; chunk < 256; chunk++)
	{
		if (ma_decoder_read_pcm_frames(&decoder, frames, 1024, nullptr) != MA_SUCCESS)
			break;
	}
	ma_decoder_seek_to_pcm_frame(&decoder, 0);
	ma_decoder_read_pcm_frames(&decoder, frames, 1024, nullptr);
	ma_decoder_uninit(&decoder);
	return 0;
}
