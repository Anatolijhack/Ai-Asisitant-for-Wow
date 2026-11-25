#pragma once
// AudioCapture.h Ч захват системного звука в госте (WASAPI loopback) + кодирование Opus.
// ¬ыход: Opus-пакеты по 20 мс, 48 к√ц, stereo, —“–ќ√ќ непрерывный поток (тишина тоже передаЄтс€).
//
// «ависимости: libopus (vcpkg install opus:x64-windows)
// Ћинковка:    ole32.lib, opus.lib (Mmdevapi подключаетс€ через COM, отдельной lib не нужно)

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <opus/opus.h>      // если не находит: <opus.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

namespace aud
{
    using Microsoft::WRL::ComPtr;

    class AudioCapture
    {
    public:
        // data/size Ч один Opus-пакет (20 мс). seq Ч пор€дковый номер.
        // samplePos Ч позици€ в сэмплах (48000 = 1 секунда), дл€ синхронизации на хосте.
        using PacketCallback = std::function<void(const uint8_t* data, size_t size,
            uint64_t seq, uint64_t samplePos)>;

        static constexpr int kSampleRate = 48000;
        static constexpr int kChannels = 2;
        static constexpr int kFrameSize = 960;   // 20 мс при 48 к√ц (сэмплов на канал)

        ~AudioCapture() { Stop(); }

        bool Start(PacketCallback cb, int bitrate = 64000)
        {
            if (m_running) return true;
            m_cb = std::move(cb);
            m_bitrate = bitrate;
            m_running = true;
            m_thread = std::thread(&AudioCapture::ThreadProc, this);
            return true;
        }

        void Stop()
        {
            if (!m_running.exchange(false)) return;
            if (m_thread.joinable()) m_thread.join();
        }

        uint64_t PacketsSent()  const { return m_seq; }
        uint32_t Reconnects()   const { return m_reconnects; }

    private:
        // ќткрываем loopback на устройстве вывода по умолчанию.
        // ѕросим у Windows формат 48 к√ц / 16 бит / stereo Ч конвертацию делает сама система.
        bool OpenDevice(ComPtr<IAudioClient>& client, ComPtr<IAudioCaptureClient>& capture)
        {
            ComPtr<IMMDeviceEnumerator> en;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&en)))) return false;

            ComPtr<IMMDevice> dev;
            if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return false;

            if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())))) return false;

            WAVEFORMATEX fmt{};
            fmt.wFormatTag = WAVE_FORMAT_PCM;
            fmt.nChannels = kChannels;
            fmt.nSamplesPerSec = kSampleRate;
            fmt.wBitsPerSample = 16;
            fmt.nBlockAlign = fmt.nChannels * fmt.wBitsPerSample / 8;
            fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;

            const REFERENCE_TIME bufferDuration = 2000000;   // 200 мс (в единицах 100 нс)
            HRESULT hr = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                bufferDuration, 0, &fmt, nullptr);
            if (FAILED(hr)) return false;

            if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) return false;
            return true;
        }

        void EmitFrame(OpusEncoder* enc, const int16_t* pcm)
        {
            uint8_t out[1500];
            int n = opus_encode(enc, pcm, kFrameSize, out, (opus_int32)sizeof(out));
            if (n > 0 && m_cb)
                m_cb(out, (size_t)n, m_seq, m_samplePos);
            ++m_seq;
            m_samplePos += kFrameSize;
        }

        void ThreadProc()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            int err = 0;
            OpusEncoder* enc = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_AUDIO, &err);
            if (!enc || err != OPUS_OK) { m_running = false; CoUninitialize(); return; }
            opus_encoder_ctl(enc, OPUS_SET_BITRATE(m_bitrate));

            using clock = std::chrono::steady_clock;
            const auto frameDur = std::chrono::milliseconds(20);
            std::vector<int16_t> pending;   // чередующиес€ L,R
            pending.reserve(kFrameSize * kChannels * 8);

            while (m_running)
            {
                ComPtr<IAudioClient> client;
                ComPtr<IAudioCaptureClient> capture;
                if (!OpenDevice(client, capture))
                {
                    // Ќет устройства вывода или оно сменилось Ч пробуем снова
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    ++m_reconnects;
                    continue;
                }

                if (FAILED(client->Start())) { ++m_reconnects; continue; }

                pending.clear();
                auto lastEmit = clock::now();
                bool ok = true;

                while (m_running && ok)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));

                    // 1) забираем всЄ, что накопилось в loopback
                    UINT32 pkt = 0;
                    if (FAILED(capture->GetNextPacketSize(&pkt))) { ok = false; break; }

                    while (pkt > 0)
                    {
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                        {
                            ok = false; break;
                        }

                        size_t old = pending.size();
                        pending.resize(old + (size_t)frames * kChannels);
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                            std::memset(pending.data() + old, 0, (size_t)frames * kChannels * sizeof(int16_t));
                        else
                            std::memcpy(pending.data() + old, data, (size_t)frames * kChannels * sizeof(int16_t));

                        capture->ReleaseBuffer(frames);
                        if (FAILED(capture->GetNextPacketSize(&pkt))) { ok = false; break; }
                    }
                    if (!ok) break;

                    // 2) кодируем полные 20-мс кадры
                    const size_t frameSamples = (size_t)kFrameSize * kChannels;
                    size_t offset = 0;
                    while (pending.size() - offset >= frameSamples)
                    {
                        EmitFrame(enc, pending.data() + offset);
                        offset += frameSamples;
                        lastEmit = clock::now();
                    }
                    if (offset) pending.erase(pending.begin(), pending.begin() + offset);

                    // 3) ≈сли в системе тишина, Windows loopback не присылает пакеты вообще.
                    //    „тобы поток на хосте не прерывалс€, сами досылаем кадры тишины.
                    auto now = clock::now();
                    while (now - lastEmit >= frameDur * 2)
                    {
                        std::vector<int16_t> frame(frameSamples, 0);
                        // добиваем остаток реальных данных нул€ми, если он есть
                        if (!pending.empty())
                        {
                            std::memcpy(frame.data(), pending.data(),
                                (std::min)(pending.size(), frameSamples) * sizeof(int16_t));
                            pending.clear();
                        }
                        EmitFrame(enc, frame.data());
                        lastEmit += frameDur;
                    }
                }

                client->Stop();
                if (m_running) { ++m_reconnects; std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
            }

            opus_encoder_destroy(enc);
            CoUninitialize();
        }

        PacketCallback m_cb;
        int m_bitrate = 64000;
        std::thread m_thread;
        std::atomic<bool> m_running{ false };
        std::atomic<uint64_t> m_seq{ 0 };
        std::atomic<uint32_t> m_reconnects{ 0 };
        uint64_t m_samplePos = 0;
    };

} // namespace aud