#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace isolator {
constexpr int64_t capacity = 1 << 18;
constexpr int64_t protocol = 0x495749333801LL;
enum Field { Magic, Stop, State, Epoch, Origin, InputEnd, Consumed, WetEpoch, WetEnd, Pid, NumFields };
struct alignas(64) Shared {
    volatile LONG64 h[16];
    float input[capacity];
    float output[capacity];
};
static_assert(sizeof(Shared) == 128 + 8 * capacity);

class Bridge {
public:
    ~Bridge() { close(); }
    bool start(double sampleRate);
    void close();
    int state() const;
    void reset(int64_t cursor);
    void publish(int64_t first, int count, const float* left, const float* right);
    int64_t wetEnd() const;
    float wet(int64_t pos) const { return shared->output[pos & (capacity - 1)]; }
    bool waitReady();
    void waitWet(int64_t target);
    int64_t origin = 0;
private:
    Shared* shared = nullptr;
    HANDLE mapping = nullptr, inputEvent = nullptr, outputEvent = nullptr, process = nullptr, job = nullptr;
    LONG64 epoch = 0;
    int64_t read(Field field) const { return InterlockedCompareExchange64(&shared->h[field], 0, 0); }
    void store(Field field, int64_t value) { InterlockedExchange64(&shared->h[field], value); }
};
}
