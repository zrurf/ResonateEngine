#include <resonate/window/session.hpp>

#include <cstdint>

#include <resonate/window/host.hpp>

namespace resonate::window
{
namespace
{

template <typename Capability> ResonateCapabilityRecord recordFor(Capability& instance)
{
    ResonateCapabilityRecord record = {};
    record.id = detail::CapabilityTraits<Capability>::id.value();
    record.version = detail::CapabilityTraits<Capability>::version;
    record.instance = &instance;
    record.struct_size = instance.header.struct_size;
    record.name = detail::CapabilityTraits<Capability>::name;
    return record;
}

} // namespace

Session::~Session()
{
    stop();
}

ResonateStatus Session::start(ResonateCapabilityRegistry* registry, const Config& config)
{
    if (started_)
    {
        return RESONATE_E_STATE;
    }
    if (registry == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    if (config.width <= 0 || config.height <= 0)
    {
        return RESONATE_E_INVALID;
    }

    /* Published before the window exists, because a module resolves them during
       on_attach and the queries answer zero until there is a window. */
    ResonateWindow& window = windowCapability();
    ResonateInput& input = inputCapability();
    ResonateRenderCanvas& canvas = windowCanvas();

    const ResonateCapabilityRecord window_record = recordFor(window);
    const ResonateCapabilityRecord input_record = recordFor(input);
    const ResonateCapabilityRecord canvas_record = recordFor(canvas);
    if (resonate_capability_register(registry, &window_record) != RESONATE_OK ||
        resonate_capability_register(registry, &input_record) != RESONATE_OK ||
        resonate_capability_register(registry, &canvas_record) != RESONATE_OK)
    {
        return RESONATE_E_STATE;
    }
    started_ = true;

    if (window.create(window.self, config.title.c_str(), static_cast<uint32_t>(config.width),
                      static_cast<uint32_t>(config.height)) != RESONATE_OK)
    {
        return RESONATE_E_UNSUPPORTED;
    }
    return RESONATE_OK;
}

void Session::stop()
{
    ResonateWindow& window = windowCapability();
    window.destroy(window.self);
}

bool Session::pump()
{
    ResonateWindow& window = windowCapability();
    window.pump_events(window.self);

    /* A quit request destroys the window from inside the pump. */
    return window.width(window.self) > 0U;
}

} // namespace resonate::window
