// CaptureTest.cpp — проверка этапа 2: стабильность захвата в VM
// Что смотрим: FPS, не чёрный ли кадр, не замёрз ли поток, WARP или GPU.
#include "ScreenCapture.h"
#include <cstdio>
#include <thread>

static bool LooksBlack(const cap::Frame& f)
{
    // Проверяем ~1000 точек: если все почти чёрные — захват пустой
    size_t step = (f.bgra.size() / 4) / 1000 + 1;
    for (size_t i = 0; i < f.bgra.size() / 4; i += step)
        if (f.bgra[i * 4] > 8 || f.bgra[i * 4 + 1] > 8 || f.bgra[i * 4 + 2] > 8)
            return false;
    return true;
}

int main()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    cap::ScreenCapture capture;
    if (!capture.Start(10))
    {
        printf("Start failed (WGC не поддерживается или нет D3D11-устройства)\n");
        return 1;
    }
    printf("Capture started. WARP=%d\n", capture.UsingWarp() ? 1 : 0);

    uint64_t lastId = 0;
    for (int sec = 1; sec <= 600; ++sec)   // 10 минут; увеличьте для длительного теста
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto f = capture.Latest();
        if (!f) { printf("[%ds] кадров нет\n", sec); continue; }

        auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - f->time).count();
        printf("[%ds] %dx%d  fps=%llu  age=%lldms  black=%d\n",
            sec, f->width, f->height,
            (unsigned long long)(f->id - lastId), (long long)ageMs, LooksBlack(*f) ? 1 : 0);
        lastId = f->id;
    }
    capture.Stop();
    return 0;
}