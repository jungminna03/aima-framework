#include "arimu/App.hpp"

namespace Arimu {

void App::Tick(uint8_t sceneIndex, double frameDt) {
    if (!m_world.HasResource<FixedTime>()) {
        m_world.InsertResource<FixedTime>();
    }
    m_world.GetResource<FixedTime>().Accumulate(frameDt);

    auto& schedule = m_schedules[sceneIndex];

    schedule.Run(m_world, Phase::PreLogic);
    schedule.Run(m_world, Phase::Input);

    while (m_world.GetResource<FixedTime>().Consume()) {
        schedule.Run(m_world, Phase::FixedLogic);
    }

    schedule.Run(m_world, Phase::Logic);
    schedule.Run(m_world, Phase::SceneTransition);
    schedule.Run(m_world, Phase::Render);
    schedule.Run(m_world, Phase::Cleanup);

    m_world.FlushAllEventChannels();
}

} // namespace Arimu
