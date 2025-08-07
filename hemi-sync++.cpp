#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <RtAudio.h>
#include <sndfile.h>
#include <cstring>

class MCP3008 {
private:
    int spi_fd;
    const char* spi_device = "/dev/spidev0.0";
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = 1000000; // 1MHz
    
public:
    bool initialize() {
        spi_fd = open(spi_device, O_RDWR);
        if (spi_fd < 0) {
            std::cerr << "Error opening SPI device" << std::endl;
            return false;
        }
        
        // Set SPI mode
        if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
            std::cerr << "Error setting SPI mode" << std::endl;
            return false;
        }
        
        // Set bits per word
        if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
            std::cerr << "Error setting SPI bits per word" << std::endl;
            return false;
        }
        
        // Set max speed
        if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
            std::cerr << "Error setting SPI speed" << std::endl;
            return false;
        }
        
        return true;
    }
    
    uint16_t readChannel(int channel) {
        if (channel < 0 || channel > 7) return 0;
        
        uint8_t tx[3] = {1, (uint8_t)((8 + channel) << 4), 0};
        uint8_t rx[3] = {0};
        
        struct spi_ioc_transfer tr = {};
        tr.tx_buf = (unsigned long)tx;
        tr.rx_buf = (unsigned long)rx;
        tr.len = 3;
        tr.delay_usecs = 0;
        tr.speed_hz = speed;
        tr.bits_per_word = bits;
        
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            std::cerr << "Error reading SPI data" << std::endl;
            return 0;
        }
        
        return ((rx[1] & 3) << 8) + rx[2];
    }
    
    ~MCP3008() {
        if (spi_fd >= 0) {
            close(spi_fd);
        }
    }
};

class AudioSynthesizer {
private:
    RtAudio audio;
    std::vector<float> riverBuffer;
    size_t riverBufferPos = 0;
    
    // Binaural chord definitions
    struct Chord {
        std::vector<float> leftFreqs;
        std::vector<float> rightFreqs;
    };
    
    std::vector<Chord> chords = {
        {{300}, {310}},
        {{300}, {316}},
        {{150}, {152}},
        {{150}, {156}},
        {{88.2f, 176.4f, 441.0f, 529.2f, 705.6f, 882.0f}, 
         {94.5f, 183.4f, 463.0f, 592.2f, 775.6f, 922.0f}},
        {{272.2f, 332.0f, 421.3f, 289.4f, 367.5f, 442.0f, 295.7f, 414.7f}, 
         {280.53f, 340.03f, 428.83f, 297.23f, 374.83f, 449.83f, 303.53f, 422.53f}},
        {{110.0f, 250.0f, 400.0f}, {117.83f, 270.215f, 438.0f}},
        {{99.5f, 202.7f}, {101.0f, 204.2f}},
        {{100.0f, 200.0f, 250.0f, 300.0f, 400.0f, 500.0f, 600.0f}, 
         {101.5f, 204.0f, 254.0f, 304.0f, 410.0f, 510.1f, 604.8f}},
        {{50.0f, 400.0f, 503.0f, 600.0f, 750.0f, 900.0f}, 
         {50.8f, 404.0f, 507.2f, 604.0f, 754.0f, 904.0f}},
        {{200.0f, 250.0f, 300.0f, 600.0f, 750.0f, 900.0f}, 
         {204.0f, 254.0f, 304.0f, 616.2f, 765.9f, 916.2f}},
        {{308.0f, 500.0f}, {322.0f, 515.0f}}
    };
    
    // Phase accumulators for sine waves
    std::vector<std::vector<float>> leftPhases;
    std::vector<std::vector<float>> rightPhases;
    
public:
    std::atomic<float> riverVolume{0.5f};
    std::atomic<int> selectedChord{0};
    std::atomic<float> sineVolume{0.5f};
    std::atomic<float> sampleRate{44100.0f};
    
    bool initialize() {
        // Initialize phase accumulators
        leftPhases.resize(chords.size());
        rightPhases.resize(chords.size());
        
        for (size_t i = 0; i < chords.size(); i++) {
            leftPhases[i].resize(chords[i].leftFreqs.size(), 0.0f);
            rightPhases[i].resize(chords[i].rightFreqs.size(), 0.0f);
        }
        
        // Setup audio output
        RtAudio::StreamParameters outputParams;
        outputParams.deviceId = audio.getDefaultOutputDevice();
        outputParams.nChannels = 2;
        outputParams.firstChannel = 0;
        
        unsigned int bufferFrames = 256;
        
        try {
            audio.openStream(&outputParams, nullptr, RTAUDIO_FLOAT32, 
                           44100, &bufferFrames, &audioCallback, this);
            audio.startStream();
            sampleRate.store(44100.0f);
        } catch (RtAudioError& e) {
            std::cerr << "RtAudio error: " << e.getMessage() << std::endl;
            return false;
        }
        
        return true;
    }
    
    bool loadRiverSound(const std::string& filename) {
        SF_INFO sfinfo;
        SNDFILE* file = sf_open(filename.c_str(), SFM_READ, &sfinfo);
        
        if (!file) {
            std::cerr << "Error loading river sound file: " << filename << std::endl;
            return false;
        }
        
        // Load entire file into buffer
        riverBuffer.resize(sfinfo.frames * sfinfo.channels);
        sf_readf_float(file, riverBuffer.data(), sfinfo.frames);
        sf_close(file);
        
        std::cout << "Loaded river sound: " << sfinfo.frames << " frames, " 
                  << sfinfo.channels << " channels" << std::endl;
        
        return true;
    }
    
    static int audioCallback(void* outputBuffer, void* inputBuffer,
                           unsigned int nBufferFrames, double streamTime,
                           RtAudioStreamStatus status, void* userData) {
        AudioSynthesizer* synth = static_cast<AudioSynthesizer*>(userData);
        float* buffer = static_cast<float*>(outputBuffer);
        
        float riverVol = synth->riverVolume.load();
        int chord = synth->selectedChord.load();
        float sineVol = synth->sineVolume.load();
        float sr = synth->sampleRate.load();
        
        // Ensure chord index is valid
        chord = std::max(0, std::min(chord, static_cast<int>(synth->chords.size() - 1)));
        
        for (unsigned int i = 0; i < nBufferFrames; i++) {
            float leftOut = 0.0f;
            float rightOut = 0.0f;
            
            // Add river sound (stereo)
            if (!synth->riverBuffer.empty()) {
                size_t pos = synth->riverBufferPos;
                if (pos < synth->riverBuffer.size() - 1) {
                    leftOut += synth->riverBuffer[pos] * riverVol;
                    rightOut += synth->riverBuffer[pos + 1] * riverVol;
                    synth->riverBufferPos += 2;
                    
                    // Loop the river sound
                    if (synth->riverBufferPos >= synth->riverBuffer.size()) {
                        synth->riverBufferPos = 0;
                    }
                }
            }
            
            // Add binaural chord (left channel)
            for (size_t j = 0; j < synth->chords[chord].leftFreqs.size(); j++) {
                float freq = synth->chords[chord].leftFreqs[j];
                float phase = synth->leftPhases[chord][j];
                leftOut += std::sin(phase) * sineVol * 0.1f; // Scale down to prevent clipping
                
                // Update phase
                synth->leftPhases[chord][j] += 2.0f * M_PI * freq / sr;
                if (synth->leftPhases[chord][j] > 2.0f * M_PI) {
                    synth->leftPhases[chord][j] -= 2.0f * M_PI;
                }
            }
            
            // Add binaural chord (right channel)
            for (size_t j = 0; j < synth->chords[chord].rightFreqs.size(); j++) {
                float freq = synth->chords[chord].rightFreqs[j];
                float phase = synth->rightPhases[chord][j];
                rightOut += std::sin(phase) * sineVol * 0.1f; // Scale down to prevent clipping
                
                // Update phase
                synth->rightPhases[chord][j] += 2.0f * M_PI * freq / sr;
                if (synth->rightPhases[chord][j] > 2.0f * M_PI) {
                    synth->rightPhases[chord][j] -= 2.0f * M_PI;
                }
            }
            
            // Clamp output to prevent distortion
            leftOut = std::max(-1.0f, std::min(1.0f, leftOut));
            rightOut = std::max(-1.0f, std::min(1.0f, rightOut));
            
            buffer[i * 2] = leftOut;
            buffer[i * 2 + 1] = rightOut;
        }
        
        return 0;
    }
    
    ~AudioSynthesizer() {
        try {
            if (audio.isStreamOpen()) {
                audio.closeStream();
            }
        } catch (RtAudioError& e) {
            std::cerr << "Error closing audio stream: " << e.getMessage() << std::endl;
        }
    }
};

int main() {
    std::cout << "Initializing Raspberry Pi Audio Controller..." << std::endl;
    
    // Initialize MCP3008
    MCP3008 adc;
    if (!adc.initialize()) {
        std::cerr << "Failed to initialize MCP3008" << std::endl;
        return 1;
    }
    
    // Initialize audio synthesizer
    AudioSynthesizer synth;
    if (!synth.initialize()) {
        std::cerr << "Failed to initialize audio synthesizer" << std::endl;
        return 1;
    }
    
    // Load river sound (you need to provide this file)
    if (!synth.loadRiverSound("river2.wav")) {
        std::cout << "Warning: Could not load river2.wav - continuing without river sound" << std::endl;
    }
    
    std::cout << "System initialized. Reading knobs..." << std::endl;
    std::cout << "Knob 0: River Volume" << std::endl;
    std::cout << "Knob 1: Chord Group (0=Simple, 1=Complex, 2=Mid-range)" << std::endl;
    std::cout << "Knob 2: Chord within Group (0-3)" << std::endl;
    std::cout << "Knob 3: Sine Wave Volume" << std::endl;
    std::cout << "Groups: 0(chords 0-3), 1(chords 4-7), 2(chords 8-11)" << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    
    // Main control loop
    while (true) {
        // Read ADC values
        uint16_t knob0 = adc.readChannel(0);
        uint16_t knob1 = adc.readChannel(1);
        uint16_t knob2 = adc.readChannel(2);
        uint16_t knob3 = adc.readChannel(3);
        
        // Convert ADC values to control parameters
        synth.riverVolume.store(knob0 / 1023.0f);
        
        // Map knob1 to group selection (3 groups: 0-2)
        int group = static_cast<int>((knob1 / 1023.0f) * 2.99f);
        
        // Map knob2 to chord within group (4 chords per group: 0-3)
        int chordInGroup = static_cast<int>((knob2 / 1023.0f) * 3.99f);
        
        // Calculate final chord index (0-11)
        int chordIndex = group * 4 + chordInGroup;
        synth.selectedChord.store(chordIndex);
        
        synth.sineVolume.store(knob3 / 1023.0f);
        
        // Print current values
        static int printCounter = 0;
        if (printCounter++ % 100 == 0) { // Print every 100 iterations
            std::cout << "River Vol: " << synth.riverVolume.load() 
                      << " | Group: " << group << " | Chord: " << chordInGroup
                      << " | Total: " << chordIndex << " | Sine Vol: " << synth.sineVolume.load() << std::endl;
        }
        
        // Small delay to prevent excessive CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    return 0;
}