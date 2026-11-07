#include "../thread.h"
#include "../../utf.h"
#include <pthread.h>
#include <sched.h>

namespace caspar {

void set_thread_name(const std::wstring& name)
{
    // Linux caps thread names at 15 bytes; a longer one made pthread_setname_np fail (ERANGE) and the thread stayed
    // "casparcg" in top -H — e.g. [ffmpeg::av_producer]. Keep the first 15 bytes, on a UTF-8 boundary (HighAsCG).
    auto n = u8(name);
    if (n.size() > 15) {
        size_t cut = 15;
        while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80)
            --cut;
        n.resize(cut);
    }
    pthread_setname_np(pthread_self(), n.c_str());
}

void set_thread_realtime_priority()
{
    pthread_t          handle = pthread_self();
    int                policy;
    struct sched_param param;
    if (pthread_getschedparam(handle, &policy, &param) != 0)
        return;
    param.sched_priority = 2;
    pthread_setschedparam(handle, SCHED_FIFO, &param);
}

} // namespace caspar
