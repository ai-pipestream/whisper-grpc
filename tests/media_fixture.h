#pragma once
#include <string>

std::string wav_fixture(unsigned sample_rate = 16000, unsigned channels = 1, unsigned seconds = 3);
std::string nut_fixture(unsigned width = 4, unsigned height = 4, unsigned frames = 30);
