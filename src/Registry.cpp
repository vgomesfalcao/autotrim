#include "Registry.h"

#include "Dsp.h"

#include <algorithm>

namespace autotrim
{
juce::String ChannelShared::displayName() const
{
    const juce::ScopedLock lock(nameLock);
    if (! name.trim().isEmpty())
        return name;
    if (! hostName.trim().isEmpty())
        return hostName;
    return "Canal " + juce::String(id);
}

namespace registry
{
    std::atomic<float> maxTrimDb { dsp::kDefaultMaxTrimDb };
    std::atomic<float> measDurationS { dsp::kDefaultMeasDurationS };
    std::atomic<float> measDrumWindowS { dsp::kDrumWindowDefaultS };
    std::atomic<int> peakFrequentPct { dsp::kDefaultPeakFrequentPct };

    namespace
    {
        juce::CriticalSection& registryLock()
        {
            static juce::CriticalSection lock;
            return lock;
        }

        std::vector<std::shared_ptr<ChannelShared>>& registryList()
        {
            static std::vector<std::shared_ptr<ChannelShared>> list;
            return list;
        }

        std::atomic<uint64_t> nextId { 1 };
    } // namespace

    std::shared_ptr<ChannelShared> registerInstance()
    {
        auto shared = std::make_shared<ChannelShared>();
        shared->id = nextId.fetch_add(1);
        // Default order matches registration order; setStateInformation
        // overwrites it with the persisted value once the host restores state.
        shared->order.store((int) shared->id);
        const juce::ScopedLock lock(registryLock());
        registryList().push_back(shared);
        return shared;
    }

    void unregisterInstance(const std::shared_ptr<ChannelShared>& shared)
    {
        shared->alive.store(false);
        const juce::ScopedLock lock(registryLock());
        auto& list = registryList();
        list.erase(std::remove(list.begin(), list.end(), shared), list.end());
    }

    std::vector<std::shared_ptr<ChannelShared>> channels()
    {
        std::vector<std::shared_ptr<ChannelShared>> result;
        {
            const juce::ScopedLock lock(registryLock());
            for (auto& ch : registryList())
                if (ch->alive.load() && ! ch->panelMode.load())
                    result.push_back(ch);
        }
        std::sort(result.begin(), result.end(),
                 [](const auto& a, const auto& b)
                 {
                     const int oa = a->order.load(), ob = b->order.load();
                     return oa != ob ? oa < ob : a->id < b->id;
                 });
        return result;
    }
} // namespace registry
} // namespace autotrim
