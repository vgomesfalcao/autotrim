#include "Registry.h"

#include "Dsp.h"

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

    void foldAgcRetrims()
    {
        for (auto& ch : channels())
        {
            const float agc = ch->agcOffsetDb.load();
            if (std::abs(agc) < 0.05f || ch->trimParam == nullptr || ch->trimDb == nullptr)
                continue;
            const float maxTrim = maxTrimDb.load();
            const float newTrim =
                juce::jlimit(-maxTrim, maxTrim, ch->trimDb->load() + agc);
            // Zero the offset first: one block may briefly under-apply, which
            // the gain smoother swallows (the reverse order would double-count).
            ch->agcOffsetDb.store(0.0f);
            ch->trimParam->beginChangeGesture();
            ch->trimParam->setValueNotifyingHost(ch->trimParam->convertTo0to1(newTrim));
            ch->trimParam->endChangeGesture();
        }
    }

    std::vector<std::shared_ptr<ChannelShared>> channels()
    {
        const juce::ScopedLock lock(registryLock());
        std::vector<std::shared_ptr<ChannelShared>> result;
        for (auto& ch : registryList())
            if (ch->alive.load() && ! ch->panelMode.load())
                result.push_back(ch);
        return result;
    }
} // namespace registry
} // namespace autotrim
